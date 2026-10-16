// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.10.2026
//
#include "smaaEffect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "colorTarget.h"
#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    namespace
    {
#include "smaaLookupData.inc"

        constexpr int kAreaWidth = 160;
        constexpr int kAreaHeight = 80;
        constexpr int kSearchWidth = 64;
        constexpr int kSearchHeight = 16;

        std::shared_ptr<Texture> createLookup(GraphicsDevice* device, const char* name, const int width,
            const int height, const PixelFormat format, const uint8_t* data, const size_t size)
        {
            auto texture = createColorTexture(device, name, format, width, height);
            texture->setLevelData(0, data, size);
            texture->upload();
            return texture;
        }

        // The compose chunk: the two debug views show the blend pass's output, which is the
        // view itself while one is selected (see the blend variant of smaa.slang).
        constexpr const char* kDebugChunk =
            "float3 debugSmaaedges() { return sceneTexture.SampleLevel(composeDebugUv, 0.0).rgb; }\n"
            "float3 debugSmaaweights() { return sceneTexture.SampleLevel(composeDebugUv, 0.0).rgb; }\n";
    }

    // One of the three passes: the variant of smaa.slang, up to three inputs, and the effect's
    // uniform block.
    class SmaaEffect::Pass final : public RenderPassShaderQuad
    {
    public:
        Pass(const std::shared_ptr<GraphicsDevice>& device, std::string variant, const Uniforms* uniforms)
            : RenderPassShaderQuad(device), _variant(std::move(variant)), _uniforms(uniforms)
        {
            _name = "RenderPassSmaa-" + _variant;
        }

        void setInput(const size_t slot, Texture* texture) { _inputs[slot] = texture; }

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangShader("smaa", _variant);
            }
        }

        void execute() override
        {
            prepareShaders();
            if (!shader() || !_inputs[0]) {
                return;
            }
            for (size_t i = 0; i < _inputs.size(); ++i) {
                setQuadTextureBinding(i, _inputs[i]);
            }
            setQuadUniforms(*_uniforms);
            RenderPassShaderQuad::execute();
        }

    private:
        std::string _variant;
        const Uniforms* _uniforms = nullptr;
        std::array<Texture*, 3> _inputs{};
    };

    SmaaEffect::SmaaEffect()
        : CameraFrameEffect("smaa", CameraFrameEffectOptions{.slang = kDebugChunk,
              .debugViews = {"smaaedges", "smaaweights"}})
    {
    }

    SmaaEffect::~SmaaEffect() = default;

    void SmaaEffect::createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes)
    {
        GraphicsDevice* device = resources.device.get();
        if (!device) {
            return;
        }
        _camera = resources.camera;
        if (!_areaTexture) {
            _areaTexture = createLookup(device, "SmaaArea", kAreaWidth, kAreaHeight, PixelFormat::PIXELFORMAT_RG8,
                kSmaaArea, sizeof(kSmaaArea));
            _searchTexture = createLookup(device, "SmaaSearch", kSearchWidth, kSearchHeight,
                PixelFormat::PIXELFORMAT_R8, kSmaaSearch, sizeof(kSmaaSearch));
        }

        // Sized from the scene from the first frame on; resolveScene keeps them following it.
        Texture* scene = resources.sceneColor;
        const int width = scene ? std::max(static_cast<int>(scene->width()), 1) : 4;
        const int height = scene ? std::max(static_cast<int>(scene->height()), 1) : 4;
        const PixelFormat outputFormat = scene ? scene->format() : PixelFormat::PIXELFORMAT_RGBA16F;

        auto edges = createColorTarget(device, "SmaaEdges", PixelFormat::PIXELFORMAT_RG8, width, height);
        auto weights = createColorTarget(device, "SmaaWeights", PixelFormat::PIXELFORMAT_RGBA8, width, height);
        auto output = createColorTarget(device, "SmaaOutput", outputFormat, width, height);
        _edges = edges.texture;
        _weights = weights.texture;
        _output = output.texture;

        // The edge and weight targets are cleared, the pixels without an edge left at zero.
        const Color clear(0.0f, 0.0f, 0.0f, 0.0f);
        _passes[0] = std::make_shared<Pass>(resources.device, "edge", &_uniforms);
        _passes[0]->init(edges.target);
        _passes[0]->setClearColor(&clear);

        _passes[1] = std::make_shared<Pass>(resources.device, "weights", &_uniforms);
        _passes[1]->init(weights.target);
        _passes[1]->setClearColor(&clear);
        _passes[1]->setInput(0, _edges.get());
        _passes[1]->setInput(1, _areaTexture.get());
        _passes[1]->setInput(2, _searchTexture.get());

        _passes[2] = std::make_shared<Pass>(resources.device, "blend", &_uniforms);
        _passes[2]->init(output.target);
        _passes[2]->setInput(1, _weights.get());
        _passes[2]->setInput(2, _edges.get());

        _source = nullptr;
        for (const auto& pass : _passes) {
            passes.resolve.push_back(pass);
        }
    }

    void SmaaEffect::destroyPasses()
    {
        _passes = {};
        _edges.reset();
        _weights.reset();
        _output.reset();
        _source = nullptr;
    }

    void SmaaEffect::update()
    {
        _uniforms.params[0] = std::clamp(threshold, 0.0f, 0.5f);
        _uniforms.params[1] = static_cast<float>(std::clamp(maxSearchSteps, 1, 112));
        _uniforms.params[2] = static_cast<float>(std::clamp(maxSearchStepsDiag, 0, 20));
        _uniforms.params[3] = std::clamp(cornerRounding, 0.0f, 100.0f) / 100.0f;

        const std::string& view = _camera ? _camera->rendering().composeDebug : std::string();
        _uniforms.output[1] = view == "smaaedges" ? 1.0f : view == "smaaweights" ? 2.0f : 0.0f;
    }

    void SmaaEffect::resolveScene(CameraFrameEffectScene& scene)
    {
        if (!_passes[2] || !scene.texture) {
            return;
        }
        // The passes read the scene resolved before them and are sized from it; it can be a
        // different texture every frame (TAA's alternating history).
        if (scene.texture != _source) {
            _source = scene.texture;
            auto options = std::make_shared<RenderPassOptions>();
            options->resizeSource = std::shared_ptr<Texture>(scene.texture, [](Texture*) {});
            for (const auto& pass : _passes) {
                pass->setOptions(options);
            }
            _passes[0]->setInput(0, scene.texture);
            _passes[2]->setInput(0, scene.texture);
        }

        _uniforms.metrics[0] = 1.0f / static_cast<float>(std::max(scene.width, 1));
        _uniforms.metrics[1] = 1.0f / static_cast<float>(std::max(scene.height, 1));
        _uniforms.metrics[2] = static_cast<float>(scene.width);
        _uniforms.metrics[3] = static_cast<float>(scene.height);

        scene.texture = _output.get();
    }

    void SmaaEffect::frameUpdate(const CameraFrameEffectContext& context)
    {
        // The edges are found, and the pixels blended, at the exposure the scene is shown with.
        _uniforms.output[0] = context.displayExposure;
    }

    Texture* SmaaEffect::edgesTexture() const
    {
        return _edges.get();
    }

    Texture* SmaaEffect::weightsTexture() const
    {
        return _weights.get();
    }

    Texture* SmaaEffect::outputTexture() const
    {
        return _output.get();
    }
}
