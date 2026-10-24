// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "meshLineBatch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "framework/components/componentSystem.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/engine.h"
#include "core/shape/boundingBox.h"
#include "framework/entity.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shaderFeatures.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/material.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/slangShaders.h"

namespace visutwin::canvas
{
    namespace
    {
        // The rows of the world transform, then the packed colours: the 64-byte instancing record.
        constexpr int FLOATS_PER_LINE = 16;
        constexpr int MIN_CAPACITY = 16;

        // The fraction of its view distance a line is pulled toward the camera, past the
        // surface it lies on.
        constexpr float LINE_PULL = 0.004f;

        // Feature word 0 of the gizmo-line program (programs/gizmo-line.slang).
        constexpr uint32_t kOccluded = 1u << 0u;

        // Two 8-bit channels as high * 256 + low, exact in a float.
        float packChannels(const float high, const float low)
        {
            const auto byte = [](const float c) { return std::round(std::clamp(c, 0.0f, 1.0f) * 255.0f); };
            return byte(high) * 256.0f + byte(low);
        }

        class GizmoLineMaterial final : public Material
        {
        public:
            GizmoLineMaterial(const std::shared_ptr<GraphicsDevice>& device, const bool occluded)
                : _device(device)
            {
                setName(occluded ? "gizmo-line-occluded" : "gizmo-line");
                setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
                auto depth = std::make_shared<DepthState>();
                if (occluded) {
                    depth->setDepthTest(false);
                    depth->setDepthWrite(false);
                }
                setDepthState(depth);
                setTransparent(true);
                if (device) {
                    ShaderFeatureSet features;
                    features.setWord(0, occluded ? kOccluded : 0u);
                    setShaderOverride(getOrCreateSlangShader(device.get(), "gizmo-line", {}, features));
                }
            }

            const void* customUniformData(size_t& outSize) const override
            {
                _block[0] = _device && _device->hdrPass() ? 1.0f : 0.0f;
                outSize = sizeof(_block);
                return _block;
            }

        private:
            std::shared_ptr<GraphicsDevice> _device;
            mutable float _block[4] = {};
        };
    }

    MeshLineBatch::MeshLineBatch(Engine* engine, const int layerId, Entity* parent, const uint32_t drawOrder)
        : drawOrder(drawOrder), _engine(engine), _device(engine ? engine->graphicsDevice() : nullptr),
          _parent(parent), _layerId(layerId), _pull(Matrix4::identity())
    {
        _material = std::make_shared<GizmoLineMaterial>(_device, false);
        _occludedMaterial = std::make_shared<GizmoLineMaterial>(_device, true);
        _mesh = createMeshFromGeometry(_device, createCylinderGeometry());
    }

    MeshLineBatch::~MeshLineBatch()
    {
        destroy();
    }

    void MeshLineBatch::setLayer(const int layerId)
    {
        _layerId = layerId;
        for (const auto& run : _runs) {
            if (auto* render = run->entity ? run->entity->findComponent<RenderComponent>() : nullptr) {
                render->setLayers({layerId});
            }
        }
    }

    void MeshLineBatch::setView(const Vector3& eye, const Vector3& forward, const bool orthographic)
    {
        float m[16] = {};
        if (orthographic) {
            // the view depth scaled toward the camera: p - k f (f . (p - eye))
            const float f[3] = {forward.getX(), forward.getY(), forward.getZ()};
            const float fe = forward.dot(eye);
            for (int c = 0; c < 3; ++c) {
                for (int r = 0; r < 3; ++r) {
                    m[c * 4 + r] = (c == r ? 1.0f : 0.0f) - LINE_PULL * f[r] * f[c];
                }
                m[12 + c] = LINE_PULL * f[c] * fe;
            }
        } else {
            // every point scaled toward the eye
            const float s = 1.0f - LINE_PULL;
            m[0] = m[5] = m[10] = s;
            m[12] = eye.getX() * LINE_PULL;
            m[13] = eye.getY() * LINE_PULL;
            m[14] = eye.getZ() * LINE_PULL;
        }
        m[15] = 1.0f;
        _pull = Matrix4::load(m);
    }

    void MeshLineBatch::begin()
    {
        _runCount = 0;
        _run = nullptr;
    }

    void MeshLineBatch::split()
    {
        _run = nullptr;
    }

    MeshLineBatch::Run& MeshLineBatch::nextRun()
    {
        if (_runCount == _runs.size()) {
            auto run = std::make_unique<Run>();
            auto entity = std::make_unique<Entity>();
            run->entity = entity.get();
            run->entity->setEngine(_engine);
            run->entity->setName("mesh-lines");
            if (auto* render = static_cast<RenderComponent*>(run->entity->addComponent<RenderComponent>())) {
                render->setLayers({_layerId});
                render->setCastShadows(false);
                render->setReceiveShadows(false);
                for (const auto& material : {_occludedMaterial, _material}) {
                    auto instance = std::make_unique<MeshInstance>(_mesh, material, run->entity);
                    instance->setCull(false);
                    instance->setCastShadow(false);
                    instance->setReceiveShadow(false);
                    run->meshInstances.push_back(render->addMeshInstance(std::move(instance)));
                }
            }
            _parent->addChild(std::move(entity));
            _runs.push_back(std::move(run));
        }
        return *_runs[_runCount++];
    }

    void MeshLineBatch::add(const Vector3& from, const Vector3& to, const float thickness, const Color& color,
        const Color& occludedColor)
    {
        Run* run = _run;
        if (!run) {
            run = _run = &nextRun();
            run->entity->setEnabled(true);
            run->count = 0;
            run->data.clear();
            for (MeshInstance* instance : run->meshInstances) {
                instance->setDrawOrder(static_cast<double>(drawOrder));
            }
        }

        // a unit cylinder along y, turned to the line
        const Vector3 axis = to - from;
        const float length = axis.length();
        const Vector3 dir = length > 0.0f ? axis * (1.0f / length) : Vector3(0.0f, 1.0f, 0.0f);
        const Vector3 helper = std::abs(dir.getY()) < 0.99f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
        const Vector3 x = helper.cross(dir).normalized();
        const Vector3 z = x.cross(dir);
        const Vector3 mid = (from + to) * 0.5f + _parent->position();
        const float local[16] = {
            x.getX() * thickness, x.getY() * thickness, x.getZ() * thickness, 0.0f,
            axis.getX(), axis.getY(), axis.getZ(), 0.0f,
            z.getX() * thickness, z.getY() * thickness, z.getZ() * thickness, 0.0f,
            mid.getX(), mid.getY(), mid.getZ(), 1.0f,
        };
        float m[16];
        (_pull * Matrix4::load(local)).store(m);

        // the world bounds of the line's ends, widened by its thickness
        const Vector3 a = from + _parent->position();
        const Vector3 b = to + _parent->position();
        const Vector3 pad(thickness);
        const Vector3 lo = Vector3::min(a, b) - pad;
        const Vector3 hi = Vector3::max(a, b) + pad;
        run->boundsMin = run->count == 0 ? lo : Vector3::min(run->boundsMin, lo);
        run->boundsMax = run->count == 0 ? hi : Vector3::max(run->boundsMax, hi);

        for (int row = 0; row < 3; ++row) {
            run->data.push_back(m[row]);
            run->data.push_back(m[row + 4]);
            run->data.push_back(m[row + 8]);
            run->data.push_back(m[row + 12]);
        }
        run->data.push_back(packChannels(color.r, color.g));
        run->data.push_back(packChannels(color.b, color.a));
        run->data.push_back(packChannels(occludedColor.r, occludedColor.g));
        run->data.push_back(packChannels(occludedColor.b, occludedColor.a));
        ++run->count;
    }

    void MeshLineBatch::upload(Run& run)
    {
        if (!_device) {
            return;
        }
        const size_t slots = static_cast<size_t>(std::max(_device->maxFramesInFlight(), 1));
        if (run.buffers.size() != slots) {
            run.buffers.assign(slots, nullptr);
            run.capacities.assign(slots, 0);
            run.slot = 0;
            run.written = false;
        }
        const int key = _device->renderVersion();
        if (run.written && key != run.lastKey) {
            run.slot = (run.slot + 1) % slots;
        }
        run.written = true;
        run.lastKey = key;

        auto& buffer = run.buffers[run.slot];
        int& capacity = run.capacities[run.slot];
        const int needed = std::max(capacity, MIN_CAPACITY);
        int newCapacity = needed;
        while (newCapacity < run.count) {
            newCapacity *= 2;
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(newCapacity) * FLOATS_PER_LINE * sizeof(float), 0);
        std::memcpy(bytes.data(), run.data.data(), run.data.size() * sizeof(float));
        if (!buffer || newCapacity != capacity) {
            VertexBufferOptions options;
            options.usage = BUFFER_DYNAMIC;
            options.data = std::move(bytes);
            buffer = _device->createVertexBuffer(VertexFormat::defaultInstancingFormat(), newCapacity, options);
            capacity = buffer ? newCapacity : 0;
        } else {
            buffer->setData(bytes);
        }
        // the instance records are not matrices, so the bounds are given rather than derived
        const BoundingBox bounds((run.boundsMin + run.boundsMax) * 0.5f, (run.boundsMax - run.boundsMin) * 0.5f);
        for (MeshInstance* instance : run.meshInstances) {
            instance->setCustomAabb(bounds);
            instance->setInstancing(buffer, run.count);
        }
    }

    void MeshLineBatch::end()
    {
        for (size_t i = 0; i < _runs.size(); ++i) {
            Run& run = *_runs[i];
            if (i >= _runCount) {
                run.entity->setEnabled(false);
                continue;
            }
            upload(run);
        }
        _run = nullptr;
    }

    size_t MeshLineBatch::lineCount() const
    {
        size_t count = 0;
        for (size_t i = 0; i < _runCount; ++i) {
            count += static_cast<size_t>(_runs[i]->count);
        }
        return count;
    }

    void MeshLineBatch::destroy()
    {
        if (_destroyed) {
            return;
        }
        _destroyed = true;
        for (const auto& run : _runs) {
            if (run->entity && run->entity->parent()) {
                auto owned = run->entity->remove();
            }
        }
        _runs.clear();
        _runCount = 0;
        _run = nullptr;
    }
}
