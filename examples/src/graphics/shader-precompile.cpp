// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.11.2026
//
// Port of upstream's graphics/shader-precompile example.
//
// Many StandardMaterial variants, each needing shaders of its own. With precompile on, the
// shaders and render pipelines are prepared before the first frame, and again after a ring of
// materials is added, so that they compile in parallel (Engine::precompile). The camera sits
// inside two rings of 24 spheres (radius 7, rows 1.6 apart, the rows offset by half a step), so
// most of them start outside its view, and turns 15 degrees a second. Each sphere's material
// takes three to five of sixteen features at random (seeded, so every run makes the same ones):
// a diffuse, normal, AO, metalness or gloss map, clearcoat, sheen, iridescence, an emissive map,
// alpha test, blending, anisotropy, a detail map, two-sided lighting, opacity dither, refraction.
// The helipad environment lights the scene (skybox mip 1, intensity 0.6), a sun (pitch 50, yaw 30)
// casts 2048 shadows out to 30 units and a warm omni lamp (range 14, intensity 2) at (0, 4, 0)
// casts its own; neutral tone mapping, fov 60.
//
// The shaders are made unique to the run: every material carries an override of the user hooks
// (`forward-vertex-local`, `forward-fragment-user`) holding a value drawn when the example starts,
// which the compiler cannot fold away. That forces the run-time Slang compile of the forward
// program (and of the shadow program, for the alpha-tested and dithered materials), and every
// pipeline, so neither Metal's nor our shader cache hides the cost of compiling them.
//
// Keys stand in for upstream's control panel:
//   A   adds a ring of 24 materials (precompiled first when precompile is on)
//   P   precompile on / off (on to begin with), for the next ring added
//   F   precompile with frustum culling on / off (off): only what the camera sees
//   O   camera rotation on / off
//   I   logs the stats: shaders, render pipelines, the last precompile's time, and the stalls
//       (the time frames that created shaders or pipelines, and the few after them, took over a
//       usual frame, without precompile)
//
// DEVIATIONS:
// - no progress bar: nothing renders while a precompile runs (upstream draws its bar in HTML,
//   outside the canvas), so the progress is logged.
// - the settings apply to the next ring added rather than being stored for the next load; the
//   first load always precompiles (VISUTWIN_PRECOMPILE_PROGRAMS=0 turns its program phase off).
// - the diffuse detail map is a detail NORMAL map (the only detail map here), the anisotropy is a
//   GGX lobe of strength 0.8, and refraction is transmission 0.5 at an IOR of 1.5 (upstream's
//   refractionIndex is eta, 1 / 1.5).
// - sheen is a colour (0.8, 0.3, 0.3) with roughness 0.5: there is no useSheen switch.
// - the back buffer renders at the examples' one pixel per point, not min(device ratio, 2).
//
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <numbers>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/light/lightComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kRingCount = 24;
    constexpr float kRingRadius = 7.0f;
    constexpr float kRingStep = 1.6f;

    // A small seeded generator (Park-Miller, as upstream's), so every run makes the same materials.
    class MinStd
    {
    public:
        double operator()()
        {
            _seed = (_seed * 16807u) % 2147483647u;
            return static_cast<double>(_seed - 1u) / 2147483646.0;
        }

    private:
        uint64_t _seed = 1234u;
    };
}

class ShaderPrecompileExample final: public ExampleApp
{
public:
    ShaderPrecompileExample(): ExampleApp({}) {}

protected:
    bool create() override
    {
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        for (const char* name : {"seaside-rocks01-color.jpg", "normal-map.png", "seaside-rocks01-ao.jpg",
                 "seaside-rocks01-diffuse-alpha.png", "heart.png"}) {
            _textureAssets.push_back(std::make_unique<Asset>(name, AssetType::TEXTURE,
                assetPath(std::string("textures/") + name)));
        }
        _color = _textureAssets[0]->resourceAs<Texture>();
        _normal = _textureAssets[1]->resourceAs<Texture>();
        _ao = _textureAssets[2]->resourceAs<Texture>();
        _alpha = _textureAssets[3]->resourceAs<Texture>();
        _emissive = _textureAssets[4]->resourceAs<Texture>();
        if (!_color || !_normal || !_ao || !_alpha || !_emissive) {
            spdlog::error("shader-precompile: a texture failed to load");
            return false;
        }
        // The detail map tiles.
        _normal->setAddressU(ADDRESS_REPEAT);
        _normal->setAddressV(ADDRESS_REPEAT);

        if (Texture* envAtlas = _helipad->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }
        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(0.6f);

        // A value unique to this run, compiled into every shader of the materials.
        std::random_device device;
        _runSeed = std::to_string(1.0 + std::uniform_real_distribution<double>(0.0, 1.0)(device)) + "e-30";

        addRing(kRingCount);
        addRing(kRingCount);

        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.4f, 0.4f, 0.45f, 1.0f));
        makeShadersUnique(*_groundMaterial);
        Entity* ground = createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, -0.9f, 0.0f),
            Vector3(30.0f, 1.0f, 30.0f));
        ground->setName("Ground");

        _camera = createCamera(Vector3(0.0f, 0.8f, 0.0f));
        _camera->setName("Camera");
        if (auto* component = _camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->setToneMapping(TONEMAP_NEUTRAL);
            component->camera()->setFov(60.0f);
        }

        Entity* sun = createDirectionalLight(Vector3(50.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        sun->setName("Sun");
        if (auto* light = sun->findComponent<LightComponent>()) {
            light->setShadowResolution(2048);
            light->setShadowDistance(30.0f);
            light->setShadowBias(0.2f);
            light->setShadowNormalBias(0.05f);
        }

        auto* lamp = new Entity();
        lamp->setName("Lamp");
        lamp->setEngine(engine());
        lamp->setLocalPosition(0.0f, 4.0f, 0.0f);
        auto* lampLight = static_cast<LightComponent*>(lamp->addComponent<LightComponent>());
        lampLight->setType(LightType::LIGHTTYPE_OMNI);
        lampLight->setColor(Color(1.0f, 0.8f, 0.6f, 1.0f));
        lampLight->setRange(14.0f);
        lampLight->setIntensity(2.0f);
        lampLight->setCastShadows(true);
        lampLight->setShadowBias(0.2f);
        lampLight->setShadowNormalBias(0.05f);
        root()->addChild(lamp);

        // The progress of a precompile, logged in steps: nothing renders while it runs.
        engine()->on("precompile:start", [this]() {
            _precompiling = true;
            _loggedProgress = -1;
            spdlog::info("Compiling shaders... 0%");
        });
        engine()->on("precompile:progress", [this](const float progress) {
            const int percent = static_cast<int>(progress * 100.0f);
            if (percent / 25 > _loggedProgress / 25 || percent == 100) {
                _loggedProgress = percent;
                spdlog::info("Compiling shaders... {}%", percent);
            }
        });

        precompile("Loading");
        spdlog::info("shader-precompile: A adds a ring of materials, P precompile, F frustum culling, O rotation, "
            "I stats");
        return true;
    }

    void update(const float dt) override
    {
        if (const auto* keyboard = engine()->keyboard()) {
            if (keyboard->wasPressed(Key::A)) {
                addRing(kRingCount);
                precompile("Added materials");
            }
            if (keyboard->wasPressed(Key::P)) {
                _precompile = !_precompile;
                spdlog::info("Precompile {}", _precompile ? "on" : "off");
            }
            if (keyboard->wasPressed(Key::F)) {
                _frustumCulling = !_frustumCulling;
                spdlog::info("Precompile frustum culling {}", _frustumCulling ? "on" : "off");
            }
            if (keyboard->wasPressed(Key::O)) {
                _rotate = !_rotate;
            }
            if (keyboard->wasPressed(Key::I)) {
                const auto counts = engine()->graphicsDevice()->liveResourceCounts();
                spdlog::info("Shaders {}, render pipelines {}, precompile {}, stalls {:.0f} ms", counts.shaders,
                    counts.renderPipelines, _precompileTime, _stallTime);
            }
        }
        if (_rotate) {
            _yaw += dt * 15.0f;
            _camera->setLocalEulerAngles(0.0f, _yaw, 0.0f);
        }
    }

    // The stalls: frames that created shaders or render pipelines outside a precompile, and the
    // few after them, measured by how much longer than a usual frame they took.
    void postRender() override
    {
        const auto now = std::chrono::steady_clock::now();
        const double frameTime = _lastFrame.time_since_epoch().count() > 0
            ? std::chrono::duration<double, std::milli>(now - _lastFrame).count() : _usualFrameTime;
        _lastFrame = now;

        const auto counts = engine()->graphicsDevice()->liveResourceCounts();
        const std::pair<int, int> current{counts.shaders, counts.renderPipelines};
        if (current != _lastCounts && !_precompiling) {
            _stallFrames = 4;
        }
        _lastCounts = current;
        if (_stallFrames > 0) {
            --_stallFrames;
            _stallTime += std::max(0.0, frameTime - _usualFrameTime);
        } else if (!_precompiling) {
            _usualFrameTime += (frameTime - _usualFrameTime) * 0.1;
        }
    }

    void destroy() override
    {
        _materials.clear();
    }

private:
    // Makes the shaders of a material unique to this run: the user hooks of both stages carry the
    // run's value, in a way the compiler keeps.
    void makeShadersUnique(StandardMaterial& material) const
    {
        material.setShaderChunk("forward-vertex-local",
            "void forwardLocalVertex(inout float3 position, inout float3 normal, inout float4 tangent, "
            "float2 uv0, float2 uv1)\n{\n    position.x += " + _runSeed + ";\n}\n");
        material.setShaderChunk("forward-fragment-user",
            "float3 forwardUserLitColor(float3 color, float3 albedo, float3 worldNormal, float3 worldPos)\n"
            "{ return color; }\n"
            "float forwardUserFogFactor(float fogFactor, float3 worldPos) { return fogFactor; }\n"
            "float3 forwardUserFragmentEnd(float3 color, float3 worldPos, float3 worldNormal)\n"
            "{ return color + float3(" + _runSeed + ", 0.0, 0.0); }\n");
    }

    // A combination of features not used yet.
    uint32_t nextMask()
    {
        for (;;) {
            uint32_t mask = 0;
            const int count = 3 + static_cast<int>(std::floor(_random() * 3.0));
            for (int i = 0; i < count; ++i) {
                mask |= 1u << static_cast<int>(std::floor(_random() * kFeatureCount));
            }
            if (_usedMasks.insert(mask).second) {
                return mask;
            }
        }
    }

    void applyFeature(StandardMaterial& m, const int feature) const
    {
        switch (feature) {
        case 0: m.setDiffuseMap(_color); break;
        case 1: m.setNormalMap(_normal); break;
        case 2: m.setAoMap(_ao); break;
        case 3:
            m.setUseMetalness(true);
            m.setMetalness(0.8f);
            m.setMetalnessMap(_ao);
            break;
        case 4:
            m.setClearCoat(0.8f);
            m.setClearCoatGloss(0.9f);
            break;
        case 5:
            m.setSheenColor(Color(0.8f, 0.3f, 0.3f, 1.0f));
            m.setSheenRoughness(0.5f);
            break;
        case 6:
            m.setIridescenceIntensity(1.0f);
            m.setIridescenceThicknessMax(600.0f);
            break;
        case 7:
            m.setEmissiveMap(_emissive);
            m.setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
            break;
        case 8:
            m.setAlphaMode(AlphaMode::MASK);
            m.setOpacityMap(_alpha);
            m.setAlphaCutoff(0.5f);
            break;
        case 9:
            m.setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
            m.setDepthState(std::make_shared<DepthState>(DepthState::noWrite()));
            m.setTransparent(true);
            m.setOpacity(0.6f);
            break;
        case 10: m.setGlossMap(_ao); break;
        case 11:
            m.setEnableGGXSpecular(true);
            m.setAnisotropy(0.8f);
            break;
        case 12:
            m.setDetailNormalMap(_normal);
            m.setDetailNormalTransform(TextureTransform{.tiling = Vector2(4.0f, 4.0f)});
            break;
        case 13:
            m.setCullMode(CullMode::CULLFACE_NONE);
            m.setTwoSidedLighting(true);
            break;
        case 14:
            m.setOpacityDitherMode(DitherMode::DITHER_BAYER8);
            m.setOpacity(0.7f);
            break;
        case 15:
            m.setTransmissionFactor(0.5f);
            m.setRefractionIndex(1.5f);
            break;
        default: break;
        }
    }

    // A ring of spheres around the camera, each with a material of its own combination of features.
    void addRing(const int count)
    {
        const float y = static_cast<float>(_ringIndex) * kRingStep;
        for (int i = 0; i < count; ++i) {
            auto material = std::make_shared<StandardMaterial>();
            const float r = static_cast<float>(0.3 + 0.7 * _random());
            const float g = static_cast<float>(0.3 + 0.7 * _random());
            const float b = static_cast<float>(0.3 + 0.7 * _random());
            material->setDiffuse(Color(r, g, b, 1.0f));
            material->setGloss(static_cast<float>(0.4 + 0.5 * _random()));
            const uint32_t mask = nextMask();
            for (int feature = 0; feature < kFeatureCount; ++feature) {
                if (mask & (1u << feature)) {
                    applyFeature(*material, feature);
                }
            }
            makeShadersUnique(*material);

            // The rows are offset by half a step, so the spheres of neighbouring rows interleave.
            const double angle = (static_cast<double>(i) + (_ringIndex % 2) * 0.5) / count * std::numbers::pi * 2.0;
            Entity* sphere = createPrimitive("sphere", material.get(),
                Vector3(static_cast<float>(std::sin(angle)) * kRingRadius, y,
                    static_cast<float>(std::cos(angle)) * kRingRadius),
                Vector3(1.3f, 1.3f, 1.3f));
            sphere->setName("ring" + std::to_string(_ringIndex) + "-" + std::to_string(i));
            const auto pitch = static_cast<float>(_random() * 360.0);
            const auto yaw = static_cast<float>(_random() * 360.0);
            sphere->setLocalEulerAngles(pitch, yaw, 0.0f);
            _materials.push_back(std::move(material));
        }
        ++_ringIndex;
    }

    // Prepares the shaders the next frame needs, when enabled, and logs the result.
    void precompile(const std::string& step)
    {
        if (!_precompile) {
            _precompileTime = "off";
            return;
        }
        _precompileTime = "compiling";
        engine()->precompile({.frustumCulling = _frustumCulling}, [this, step](const PrecompileStats& stats) {
            _precompileTime = std::to_string(static_cast<int>(stats.time)) + " ms";
            // The frames right after count towards the stalls.
            _precompiling = false;
            _stallFrames = 4;
            spdlog::info("Precompile {}: {} programs, {} shaders, {} pipelines, {:.0f} ms", step, stats.programs,
                stats.shaders, stats.pipelines, stats.time);
        });
    }

    static constexpr int kFeatureCount = 16;

    std::unique_ptr<Asset> _helipad;
    std::vector<std::unique_ptr<Asset>> _textureAssets;
    Texture* _color = nullptr;
    Texture* _normal = nullptr;
    Texture* _ao = nullptr;
    Texture* _alpha = nullptr;
    Texture* _emissive = nullptr;

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    Entity* _camera = nullptr;
    std::string _runSeed;
    MinStd _random;
    std::set<uint32_t> _usedMasks;
    int _ringIndex = 0;
    float _yaw = 0.0f;

    bool _precompile = true;
    bool _frustumCulling = false;
    bool _rotate = true;
    bool _precompiling = false;
    int _loggedProgress = -1;
    std::string _precompileTime = "-";

    std::chrono::steady_clock::time_point _lastFrame{};
    double _usualFrameTime = 1000.0 / 60.0;
    int _stallFrames = 0;
    double _stallTime = 0.0;
    std::pair<int, int> _lastCounts{0, 0};
};

VISUTWIN_EXAMPLE_MAIN(ShaderPrecompileExample)
