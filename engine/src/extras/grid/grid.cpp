// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "extras/grid/grid.h"

#include <algorithm>
#include <cstdint>

#include "framework/components/componentSystem.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/entity.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shaderFeatures.h"
#include "scene/materials/material.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/slangShaders.h"

namespace visutwin::canvas
{
    namespace
    {
        // Feature word 0 of the grid program (programs/grid.slang: kGrid*).
        enum GridSwitch : uint32_t
        {
            kContrast = 1u << 0u,
            kDepth = 1u << 1u,
            kDepthDither = 1u << 2u,
        };

        // The grid program's uniform block (programs/grid.slang: GridData).
        struct GridBlock
        {
            float axisU[4] = {};
            float axisV[4] = {};
            float params[4] = {};
            float thickness[4] = {};
            float colorU[4] = {};
            float colorV[4] = {};
            float majorColor[4] = {};
            float minorColor[4] = {};
            float output[4] = {};
        };
        static_assert(sizeof(GridBlock) == 144, "GridBlock mirrors GridData");

        void store(float (&out)[4], const Color& color)
        {
            out[0] = color.r;
            out[1] = color.g;
            out[2] = color.b;
            out[3] = color.a;
        }
    }

    class GridMaterial : public Material
    {
    public:
        GridMaterial(const std::shared_ptr<GraphicsDevice>& device, const bool depth)
            : _device(device), _depth(depth)
        {
            setName(depth ? "grid-depth" : "grid");
            auto blend = std::make_shared<BlendState>(BlendState::alphaBlend());
            if (depth) {
                blend->setRedWrite(false);
                blend->setGreenWrite(false);
                blend->setBlueWrite(false);
                blend->setAlphaWrite(false);
            }
            setBlendState(blend);
            setDepthTest(true);
            setCullMode(CullMode::CULLFACE_NONE);
            setTransparent(true);
        }

        void setDepthTest(const bool value)
        {
            auto depthState = std::make_shared<DepthState>();
            depthState->setDepthTest(value);
            depthState->setDepthWrite(_depth);
            setDepthState(depthState);
        }

        void setSwitches(uint32_t switches)
        {
            if (_depth) {
                switches |= kDepth;
            }
            if (_shaderBuilt && switches == _switches) {
                return;
            }
            _switches = switches;
            _shaderBuilt = true;
            if (_device) {
                ShaderFeatureSet features;
                features.setWord(0, switches);
                setShaderOverride(getOrCreateSlangShader(_device.get(), "grid", {}, features));
            }
        }

        GridBlock& block()
        {
            markUniformsDirty();
            return _source;
        }

        const void* customUniformData(size_t& outSize) const override
        {
            _drawn = _source;
            _drawn.thickness[3] = _device ? _device->pixelRatio() : 1.0f;
            _drawn.thickness[0] *= _drawn.thickness[3];
            _drawn.thickness[1] *= _drawn.thickness[3];
            _drawn.thickness[2] *= _drawn.thickness[3];
            _drawn.output[1] = _device && _device->hdrPass() ? 1.0f : 0.0f;
            outSize = sizeof(_drawn);
            return &_drawn;
        }

    private:
        std::shared_ptr<GraphicsDevice> _device;
        bool _depth = false;
        uint32_t _switches = 0;
        bool _shaderBuilt = false;
        GridBlock _source;
        mutable GridBlock _drawn;
    };

    Grid::Grid(const std::shared_ptr<GraphicsDevice>& device)
        : _device(device)
    {
        _materials[0] = std::make_shared<GridMaterial>(device, false);
        _materials[1] = std::make_shared<GridMaterial>(device, true);
        updateShaders();
        updatePlane();
        updateBlocks();
    }

    Grid::~Grid()
    {
        attach(nullptr);
    }

    void Grid::attach(Entity* entity)
    {
        if (_render) {
            for (MeshInstance*& instance : _meshInstances) {
                _render->removeMeshInstance(instance);
                instance = nullptr;
            }
            _render = nullptr;
        }
        _entityWatch.reset();
        if (!entity) {
            return;
        }

        auto* render = entity->findComponent<RenderComponent>();
        if (!render) {
            render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
            if (!render) {
                return;
            }
            render->setCastShadows(false);
        }
        // Moved in the vertex stage, so never culled; drawn first of the transparent draws, so
        // the depth pass can hide them.
        auto mesh = createMeshFromGeometry(_device, createPlaneGeometry());
        for (size_t i = 0; i < _materials.size(); ++i) {
            auto instance = std::make_unique<MeshInstance>(mesh, _materials[i], entity);
            instance->setCull(false);
            instance->setCastShadow(false);
            instance->setDrawBucket(255);
            _meshInstances[i] = render->addMeshInstance(std::move(instance));
        }
        _render = render;
        _entityWatch.watch(entity, [this]() {
            // The entity's render component frees the mesh instances with it.
            _render = nullptr;
            _meshInstances = {};
        });
        updateVisible();
    }

    void Grid::setTheme(const GridTheme& theme)
    {
        _theme = theme;
        updatePlane();
        updateBlocks();
    }

    void Grid::setPlane(const GridPlane plane)
    {
        _plane = plane;
        updatePlane();
    }

    void Grid::setShowAxes(const bool value)
    {
        _showAxes = value;
        updateBlocks();
    }

    void Grid::setContrast(const bool value)
    {
        _contrast = value;
        updateShaders();
    }

    void Grid::setSpacing(const float value)
    {
        _spacing = std::max(value, 1e-6f);
        updateBlocks();
    }

    void Grid::setSubdivisions(const float value)
    {
        _subdivisions = std::max(value, 1.0f);
        updateBlocks();
    }

    void Grid::setMajorLineThickness(const float value)
    {
        _majorLineThickness = value;
        updateBlocks();
    }

    void Grid::setMinorLineThickness(const float value)
    {
        _minorLineThickness = value;
        updateBlocks();
    }

    void Grid::setAxisLineThickness(const float value)
    {
        _axisLineThickness = value;
        updateBlocks();
    }

    void Grid::setFadeRange(const Vector2& value)
    {
        _fadeRange = value;
        updateBlocks();
    }

    void Grid::setInfinite(const bool value)
    {
        _infinite = value;
        updatePlane();
    }

    void Grid::setDepthOffset(const float value)
    {
        _depthOffset = value;
        updatePlane();
    }

    void Grid::setDepthTest(const bool value)
    {
        _depthTest = value;
        for (const auto& material : _materials) {
            material->setDepthTest(value);
        }
    }

    void Grid::setDepthWrite(const bool value)
    {
        _depthWrite = value;
        updateVisible();
    }

    void Grid::setDepthDither(const bool value)
    {
        _depthDither = value;
        updateShaders();
    }

    void Grid::setVisible(const bool value)
    {
        _visible = value;
        updateVisible();
    }

    void Grid::updatePlane()
    {
        // The local axes of each plane's u and v coordinates.
        static constexpr float kAxes[3][2][3] = {
            {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
            {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
            {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        };
        const auto& axes = kAxes[static_cast<int>(_plane)];
        const Color& colorU = _plane == GridPlane::YZ ? _theme.axisY : _theme.axisX;
        const Color& colorV = _plane == GridPlane::XY ? _theme.axisY : _theme.axisZ;
        for (const auto& material : _materials) {
            GridBlock& block = material->block();
            std::copy_n(axes[0], 3, block.axisU);
            std::copy_n(axes[1], 3, block.axisV);
            block.axisU[3] = _infinite ? 1.0f : 0.0f;
            block.axisV[3] = _depthOffset;
            store(block.colorU, colorU);
            store(block.colorV, colorV);
        }
    }

    void Grid::updateBlocks()
    {
        for (const auto& material : _materials) {
            GridBlock& block = material->block();
            block.params[0] = _fadeRange.x;
            block.params[1] = _fadeRange.y;
            block.params[2] = _spacing;
            block.params[3] = _subdivisions;
            block.thickness[0] = _majorLineThickness;
            block.thickness[1] = _minorLineThickness;
            block.thickness[2] = _axisLineThickness;
            store(block.majorColor, _theme.major);
            store(block.minorColor, _theme.minor);
            block.output[0] = _showAxes ? 1.0f : 0.0f;
        }
    }

    void Grid::updateShaders()
    {
        _materials[0]->setSwitches(_contrast ? kContrast : 0u);
        _materials[1]->setSwitches(_depthDither ? kDepthDither : 0u);
    }

    void Grid::updateVisible()
    {
        if (_meshInstances[0]) {
            _meshInstances[0]->setVisible(_visible);
        }
        if (_meshInstances[1]) {
            _meshInstances[1]->setVisible(_visible && _depthWrite);
        }
    }
}
