// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// MetalGraphicsDevice: the bindless table (argument buffer) and its residency sync.
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
    namespace
    {
        // EXPERIMENTAL bindless table: samplers first, then textures, one 64-bit GPU
        // resource id per entry, the layout Slang gives the parameter block's struct
        // (array<sampler, S> then array<texture2d, T>).
        class MetalBindlessTable final : public BindlessTable
        {
        public:
            MTL::Buffer* buffer = nullptr;
            std::vector<MTL::SamplerState*> samplers;   // owned
            std::vector<MTL::Texture*> textures;        // retained
            std::vector<MTL::Resource*> residency;       // the textures, for useResources

            ~MetalBindlessTable() override
            {
                for (auto* sampler : samplers) {
                    sampler->release();
                }
                for (auto* texture : textures) {
                    texture->release();
                }
                if (buffer) {
                    buffer->release();
                }
            }
        };
        static_assert(sizeof(MTL::ResourceID) == 8, "a GPU resource id is one 64-bit word");
    }

    bool MetalGraphicsDevice::supportsBindlessTextures() const
    {
        return _device != nullptr && _device->argumentBuffersSupport() == MTL::ArgumentBuffersTier2;
    }

    std::shared_ptr<BindlessTable> MetalGraphicsDevice::createBindlessTable(const BindlessTableDesc& desc)
    {
        if (!supportsBindlessTextures()) {
            spdlog::error("MetalGraphicsDevice: bindless tables need argument buffers tier 2");
            return nullptr;
        }
        if (desc.textures.size() > kBindlessTableTextureCapacity || desc.samplers.size() > kBindlessTableSamplerCapacity) {
            spdlog::error("MetalGraphicsDevice: bindless table of {} textures / {} samplers exceeds {} / {}",
                desc.textures.size(), desc.samplers.size(), kBindlessTableTextureCapacity, kBindlessTableSamplerCapacity);
            return nullptr;
        }
        auto table = std::make_shared<MetalBindlessTable>();
        const size_t bytes = (kBindlessTableSamplerCapacity + kBindlessTableTextureCapacity) * sizeof(MTL::ResourceID);
        table->buffer = _device->newBuffer(bytes, MTL::ResourceStorageModeShared);
        if (!table->buffer) {
            spdlog::error("MetalGraphicsDevice: bindless table buffer allocation failed");
            return nullptr;
        }
        table->buffer->setLabel(NS::String::string("bindless table", NS::UTF8StringEncoding));
        auto* ids = static_cast<MTL::ResourceID*>(table->buffer->contents());
        std::memset(ids, 0, bytes);
        for (size_t i = 0; i < desc.samplers.size(); ++i) {
            MTL::SamplerState* sampler = MetalSamplerCache::newSamplerState(_device, desc.samplers[i], true);
            if (!sampler) {
                spdlog::error("MetalGraphicsDevice: bindless sampler {} failed", i);
                return nullptr;
            }
            table->samplers.push_back(sampler);
            ids[i] = sampler->gpuResourceID();
        }
        for (size_t i = 0; i < desc.textures.size(); ++i) {
            Texture* texture = desc.textures[i];
            auto* hw = texture ? dynamic_cast<gpu::MetalTexture*>(texture->impl()) : nullptr;
            MTL::Texture* raw = hw ? hw->raw() : nullptr;
            if (!raw) {
                continue;
            }
            raw->retain();
            table->textures.push_back(raw);
            table->residency.push_back(raw);
            ids[kBindlessTableSamplerCapacity + i] = raw->gpuResourceID();
        }
        spdlog::info("Metal bindless table: {} textures and {} samplers in a {}-byte argument buffer "
            "(tier 2; device maxArgumentBufferSamplerCount={})",
            table->textures.size(), table->samplers.size(), bytes, _device->maxArgumentBufferSamplerCount());
        return table;
    }

    void MetalGraphicsDevice::setBindlessTable(BindlessTable* table)
    {
        _bindlessTable = table;
    }

    void MetalGraphicsDevice::bindBindlessTable(MTL::RenderCommandEncoder* passEncoder)
    {
        MTL::Buffer* buffer = nullptr;
        const std::vector<MTL::Resource*>* resident = nullptr;
        const void* key = nullptr;
        if (auto* table = static_cast<MetalBindlessTable*>(_bindlessTable)) {
            buffer = table->buffer;
            resident = &table->residency;
            key = table;
        } else {
            // The current slot's table, brought up to date whenever the registry moved since
            // it was written: onFrameStart writes it whole, and a texture that joins, is
            // recreated (a render target resized in the update) or leaves BEFORE a draw of
            // the same frame is seen by that draw, since the GPU reads the ids when the frame
            // runs. A draw outside a frame (an offline bake before the first one) writes it
            // here too.
            if (_bindlessIds.empty() || BindlessResidency::generation() != _bindlessGeneration ||
                BindlessSamplers::generation() != _bindlessSamplerGeneration) {
                syncBindlessTable();
            }
            buffer = _bindlessFrameBuffers[_bindlessFrameSlot];
            resident = &_bindlessResident;
            key = buffer;
        }
        if (!buffer) {
            return;
        }
        const bool rebind = key != _encoderBindlessKey;
        if (!rebind && _encoderBindlessResidentVersion == _bindlessResidentVersion) {
            return;
        }
        if (rebind) {
            passEncoder->setVertexBuffer(buffer, 0, kBindlessTableMetalBufferSlot);
            passEncoder->setFragmentBuffer(buffer, 0, kBindlessTableMetalBufferSlot);
        }
        // An argument buffer only names its textures; the encoder must be told they are
        // read, or they may not be resident when the draw runs. Told again whenever the
        // live set changed under the open encoder.
        if (!resident->empty()) {
            passEncoder->useResources(resident->data(), resident->size(),
                MTL::ResourceUsageRead, MTL::RenderStageVertex | MTL::RenderStageFragment);
        }
        _encoderBindlessKey = key;
        _encoderBindlessResidentVersion = _bindlessResidentVersion;
    }

    void MetalGraphicsDevice::syncBindlessTable()
    {
        if (!_device) {
            return;
        }
        constexpr size_t kSamplers = kBindlessTableSamplerCapacity;
        // The ids: the samplers, then each kind's textures at its offset, in the parameter
        // block's member order (bindings.slang).
        constexpr std::array<size_t, static_cast<size_t>(BindlessKind::Count)> kCapacities{
            kBindlessTableTextureCapacity, kBindlessTableTextureArrayCapacity, kBindlessTableVolumeCapacity};
        constexpr std::array<size_t, static_cast<size_t>(BindlessKind::Count)> kOffsets{
            kSamplers, kSamplers + kBindlessTableTextureCapacity,
            kSamplers + kBindlessTableTextureCapacity + kBindlessTableTextureArrayCapacity};
        constexpr size_t kEntries = kOffsets.back() + kCapacities.back();
        const size_t bytes = kEntries * sizeof(MTL::ResourceID);
        if (_bindlessIds.empty()) {
            // First use: the buffers, the white placeholder and every id pointing at it.
            for (auto& buffer : _bindlessFrameBuffers) {
                buffer = _device->newBuffer(bytes, MTL::ResourceStorageModeShared);
                if (buffer) {
                    buffer->setLabel(NS::String::string("bindless table", NS::UTF8StringEncoding));
                }
            }
            auto* descriptor = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, 1, 1, false);
            descriptor->setUsage(MTL::TextureUsageShaderRead);
            descriptor->setStorageMode(MTL::StorageModeShared);
            _bindlessWhite = _device->newTexture(descriptor);
            if (_bindlessWhite) {
                const uint8_t white[4] = {255, 255, 255, 255};
                _bindlessWhite->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, white, 4);
                _bindlessWhite->setLabel(NS::String::string("bindless white", NS::UTF8StringEncoding));
            }
            // Every texture entry starts at the white 1x1: a 2D read of it is white, and an
            // array or volume entry nothing wrote is never read (its lane is 0).
            _bindlessIds.assign(kEntries, _bindlessWhite ? _bindlessWhite->gpuResourceID() : MTL::ResourceID{});
            for (size_t i = 0; i < kSamplers; ++i) {
                _bindlessIds[i] = MTL::ResourceID{};
            }
            for (size_t k = 0; k < _bindlessKinds.size(); ++k) {
                _bindlessKinds[k].versions.assign(kCapacities[k], 0);
                _bindlessKinds[k].textures.assign(kCapacities[k], nullptr);
            }
            if (_bindlessWhite) {
                _bindlessResident.push_back(_bindlessWhite);
            }
        }

        // Samplers: a state joins the registry once and keeps its index, so only new ones
        // are created; argument-buffer samplers, clamped to this device's anisotropy.
        if (const uint64_t generation = BindlessSamplers::generation(); generation != _bindlessSamplerGeneration) {
            BindlessSamplers::snapshot(_bindlessSamplerStates);
            for (size_t i = _bindlessSamplers.size(); i < _bindlessSamplerStates.size() && i < kSamplers; ++i) {
                TextureSamplerState state = _bindlessSamplerStates[i];
                state.maxAnisotropy = std::min(state.maxAnisotropy, maxAnisotropy());
                MTL::SamplerState* sampler = MetalSamplerCache::newSamplerState(_device, state, true);
                if (!sampler) {
                    spdlog::error("MetalGraphicsDevice: bindless sampler {} failed", i);
                    break;
                }
                _bindlessSamplers.push_back(sampler);
                _bindlessIds[i] = sampler->gpuResourceID();
            }
            if (_bindlessSamplerStates.size() > kSamplers) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    spdlog::error("MetalGraphicsDevice: {} sampler states exceed the bindless table's {}",
                        _bindlessSamplerStates.size(), kSamplers);
                }
            }
            _bindlessSamplerGeneration = generation;
        }

        // Textures of every kind: the entries whose version moved since this device last
        // looked.
        if (const uint64_t generation = BindlessResidency::generation(); generation != _bindlessGeneration) {
            const MTL::ResourceID whiteId = _bindlessWhite ? _bindlessWhite->gpuResourceID() : MTL::ResourceID{};
            bool residentChanged = false;
            for (size_t k = 0; k < _bindlessKinds.size(); ++k) {
                BindlessKindState& state = _bindlessKinds[k];
                BindlessResidency::snapshot(state.snapshot, static_cast<BindlessKind>(k));
                const size_t count = std::min(state.snapshot.size(), kCapacities[k]);
                for (size_t i = 1; i < count; ++i) {
                    const auto& entry = state.snapshot[i];
                    if (entry.version == state.versions[i]) {
                        continue;
                    }
                    MTL::Texture* raw = nullptr;
                    if (entry.texture && entry.texture->device() == this) {
                        if (auto* hw = dynamic_cast<gpu::MetalTexture*>(entry.texture->impl())) {
                            raw = hw->raw();
                        }
                    }
                    state.textures[i] = raw;
                    _bindlessIds[kOffsets[k] + i] = raw ? raw->gpuResourceID() : whiteId;
                    state.versions[i] = entry.version;
                    residentChanged = true;
                }
                if (state.snapshot.size() > kCapacities[k]) {
                    static std::array<bool, static_cast<size_t>(BindlessKind::Count)> warned{};
                    if (!warned[k]) {
                        warned[k] = true;
                        spdlog::error("MetalGraphicsDevice: {} bindless indices of kind {} exceed the table's {}; the rest read white",
                            state.snapshot.size(), k, kCapacities[k]);
                    }
                }
            }
            if (residentChanged) {
                ++_bindlessResidentVersion;
                _bindlessResident.clear();
                if (_bindlessWhite) {
                    _bindlessResident.push_back(_bindlessWhite);
                }
                for (const BindlessKindState& state : _bindlessKinds) {
                    for (MTL::Texture* texture : state.textures) {
                        if (texture) {
                            _bindlessResident.push_back(texture);
                        }
                    }
                }
            }
            _bindlessGeneration = generation;
        }

        // The frame's copy: whole, since this slot was last written frames ago.
        if (MTL::Buffer* buffer = _bindlessFrameBuffers[_bindlessFrameSlot]) {
            std::memcpy(buffer->contents(), _bindlessIds.data(), bytes);
        }
    }

    void MetalGraphicsDevice::releaseBindlessTable()
    {
        for (auto& buffer : _bindlessFrameBuffers) {
            if (buffer) {
                buffer->release();
                buffer = nullptr;
            }
        }
        for (auto* sampler : _bindlessSamplers) {
            sampler->release();
        }
        _bindlessSamplers.clear();
        if (_bindlessWhite) {
            _bindlessWhite->release();
            _bindlessWhite = nullptr;
        }
        _bindlessIds.clear();
        for (auto& state : _bindlessKinds) {
            state = {};
        }
        _bindlessResident.clear();
        _bindlessGeneration = 0;
        _bindlessSamplerGeneration = 0;
    }
}
