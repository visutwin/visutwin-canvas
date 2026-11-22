// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// MetalGraphicsDevice: compute dispatch.
#include <cmath>
#include "metalGraphicsDevice.h"
#include "platform/graphics/bindlessTable.h"

#include "platform/graphics/textureSamplerState.h"
#include "platform/graphics/runLoopWait.h"
#include <typeinfo>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ranges>
#include "metalTexture.h"
#include "core/scopedTimer.h"
#include "platform/graphics/screenshot.h"
#include "metalComputePipeline.h"
#include "metalIndexBuffer.h"
#include "metalShader.h"
#include "metalRenderPipeline.h"
#include "metalRenderTarget.h"
#include "metalUtils.h"
#include "metalVertexBuffer.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/stencilParameters.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    void MetalGraphicsDevice::computeDispatch(const std::vector<Compute*>& computes, const std::string& label)
    {
        if (computes.empty() || !_commandQueue || !_device) {
            return;
        }

        if (_insideRenderPass || _renderPassEncoder || _computePassEncoder) {
            spdlog::warn("Skipping compute dispatch while a render/compute encoder is active");
            return;
        }

        // Into the open command buffer, in order with the passes around it. A buffer
        // per dispatch runs the queue out of buffers: a scene with a few hundred
        // particle emitters blocked in commandBuffer() every frame.
        auto* commandBuffer = openCommandBuffer();
        if (!commandBuffer) {
            spdlog::warn("Failed to allocate command buffer for compute dispatch");
            return;
        }

        auto* encoder = commandBuffer->computeCommandEncoder();
        if (!encoder) {
            spdlog::warn("Failed to allocate compute encoder");
            return;
        }

        if (!label.empty()) {
            encoder->pushDebugGroup(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
        }

        int dispatched = 0;
        for (auto* compute : computes) {
            if (!compute || !compute->shader()) {
                continue;
            }

            auto* pipelineState = _computePipeline->get(compute->shader());
            if (!pipelineState) {
                continue;
            }

            encoder->setComputePipelineState(pipelineState);
            ++dispatched;

            // The scene maps attached to the compute, resolved now, where the kernel runs in
            // the frame, and bound at their fixed slots outside the name-ordered table
            // (compute.h): the block, the depth and the colour with the post sampler (linear,
            // clamped, mipmapped), and the Hi-Z pyramid.
            if (compute->hasSceneMaps()) {
                compute->prepareDispatch();
                const ComputeSceneMapsBlock& block = compute->sceneMapsBlock();
                encoder->setBytes(&block, sizeof(block), kComputeSceneMapsMetalBufferSlot);
                const auto bindSceneMap = [encoder](Texture* texture, const uint32_t slot) {
                    auto* hwTexture = texture ? dynamic_cast<gpu::MetalTexture*>(texture->impl()) : nullptr;
                    encoder->setTexture(hwTexture ? hwTexture->raw() : nullptr, slot);
                };
                bindSceneMap(compute->sceneDepthTexture(), kComputeSceneDepthMetalTextureSlot);
                bindSceneMap(compute->sceneColorTexture(), kComputeSceneColorMetalTextureSlot);
                bindSceneMap(compute->sceneHiZTexture(), kComputeSceneHiZMetalTextureSlot);
                encoder->setSamplerState(_postSampler, kComputeSceneColorMetalSamplerSlot);
            }

            // DEVIATION: C++ port does not yet have compute bind group
            // declarations/reflection, so resources bind in deterministic name order.
            // Metal gives each resource kind its own index namespace, so this is
            // buffer(0..b-1) for the storage buffers, buffer(b) for the loose-uniform
            // block, and texture(0..t-1) for the textures — NOT the single flat run
            // Vulkan binds. The table in compute.h is the contract for both.
            // No sampler state is bound: an MSL kernel that filters declares its own.
            uint32_t bufferSlot = 0u;
            for (const auto& buffer : compute->bufferParameters() | std::views::values) {
                auto* hwBuffer = buffer ? static_cast<MTL::Buffer*>(buffer->nativeBuffer()) : nullptr;
                encoder->setBuffer(hwBuffer, 0, static_cast<NS::UInteger>(bufferSlot));
                ++bufferSlot;
            }

            // Loose scalar uniforms travel as one small block right after the buffers.
            const std::vector<uint8_t> uniformData = compute->uniformData();
            if (!uniformData.empty()) {
                encoder->setBytes(uniformData.data(), uniformData.size(),
                    static_cast<NS::UInteger>(bufferSlot));
            }

            std::vector<std::pair<std::string, Texture*>> textureBindings;
            textureBindings.reserve(compute->textureParameters().size());
            for (const auto& [name, texture] : compute->textureParameters()) {
                textureBindings.emplace_back(name, texture);
            }
            std::sort(textureBindings.begin(), textureBindings.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            uint32_t slot = 0u;
            for (const auto& [_, texture] : textureBindings) {
                auto* hwTexture = texture ? dynamic_cast<gpu::MetalTexture*>(texture->impl()) : nullptr;
                encoder->setTexture(hwTexture ? hwTexture->raw() : nullptr, static_cast<NS::UInteger>(slot));
                ++slot;
            }

            const MTL::Size threadgroups(
                static_cast<NS::UInteger>(compute->dispatchX()),
                static_cast<NS::UInteger>(compute->dispatchY()),
                static_cast<NS::UInteger>(compute->dispatchZ())
            );

            const MTL::Size threadsPerThreadgroup(
                static_cast<NS::UInteger>(compute->threadgroupSizeX()),
                static_cast<NS::UInteger>(compute->threadgroupSizeY()),
                static_cast<NS::UInteger>(compute->threadgroupSizeZ())
            );
            if (compute->indirectSlot() >= 0) {
                // The workgroup counts an earlier kernel wrote: x, y, z at the slot.
                const auto indirect = compute->indirectBuffer();
                auto* indirectBuffer = indirect ? static_cast<MTL::Buffer*>(indirect->nativeBuffer()) : nullptr;
                if (!indirectBuffer) {
                    spdlog::error("Compute {}: no indirect dispatch buffer", compute->name());
                    continue;
                }
                encoder->dispatchThreadgroups(indirectBuffer,
                    static_cast<NS::UInteger>(compute->indirectSlot()) * GraphicsDevice::kIndirectDispatchArgsSize,
                    threadsPerThreadgroup);
            } else {
                encoder->dispatchThreadgroups(threadgroups, threadsPerThreadgroup);
            }
        }

        if (!label.empty()) {
            encoder->popDebugGroup();
        }

        encoder->endEncoding();
        // One compute encoder per call: one compute pass, however many dispatches it holds.
        if (dispatched > 0) {
            recordComputePass(dispatched);
        }
        // Left in the open command buffer even outside a frame — a simulation steps in
        // the update, before the frame starts — and submitted with the next frame, as
        // the Vulkan backend queues a dispatch for its frame's command buffer.
    }
}
