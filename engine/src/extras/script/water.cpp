// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
#include "extras/script/water.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <cstdint>
#include <string>

#include <spdlog/spdlog.h>

#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "extras/script/planarRenderer.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/lightingBlock.h"
#include "platform/graphics/shaderFeatures.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/materials/material.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "scene/shader-lib/slangShaders.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;

        // Feature word 0 of the water program (programs/water.slang: kWater*).
        enum WaterSwitch : uint32_t
        {
            kReflectionPlanar = 1u << 0u,
            kReflectionSky = 1u << 1u,
            kRefraction = 1u << 2u,
            kDepthEffects = 1u << 3u,
            kFoam = 1u << 4u,
            kWaves = 1u << 5u,
        };

        // The water program's uniform block (programs/water.slang: WaterData).
        struct WaterBlock
        {
            float shallowColor[4] = {};
            float deepColor[4] = {};
            float lightDir[4] = {};
            float lightColor[4] = {};
            float cameraPosition[4] = {};
            float cameraForward[4] = {};
            float params0[4] = {};
            float params1[4] = {};
            float params2[4] = {};
            float underwaterFogColor[4] = {};
            float foam[4] = {};
            float waveParams[4] = {};
            float swellParams[4] = {};
            float waveDirs[4] = {};
            float output[4] = {};
        };
        static_assert(sizeof(WaterBlock) == 240, "WaterBlock mirrors WaterData");

        // The scene's user shader data the underwater chunk reads: [0] (level, fog level, time),
        // [1] the caustics (tiling, speed, strength, fade depth), [2] the caustics colour.
        constexpr int kUserLevels = 0;
        constexpr int kUserCaustics = 1;
        constexpr int kUserCausticsColor = 2;

        // A colour authored in gamma space, linear, times `intensity`.
        void linearColor(const Color& color, float (&out)[4], const float intensity = 1.0f)
        {
            out[0] = std::pow(color.r, 2.2f) * intensity;
            out[1] = std::pow(color.g, 2.2f) * intensity;
            out[2] = std::pow(color.b, 2.2f) * intensity;
        }

        void store3(float (&out)[4], const Vector3& v)
        {
            out[0] = v.getX();
            out[1] = v.getY();
            out[2] = v.getZ();
        }

        // The hooks of forward-fragment-user.slang for a material under the water. The fog is
        // gated per pixel to fragments below the water level, so an underwater fog colour does not
        // fog the world above; the caustics are applied below the level, fading in under the
        // waterline and out with depth, on surfaces facing up. No branch around the samples, so
        // their derivatives stay defined.
        std::string underwaterChunk(const bool heightFog, const bool caustics)
        {
            std::string source;
            if (caustics) {
                source += "VT_DECLARE_userMap\n\n";
            }
            source += "float forwardUserFogFactor(float fogFactor, float3 worldPos)\n{\n";
            if (heightFog) {
                source +=
                    "    // No fog above the water level, a soft blend across the waterline.\n"
                    "    const float below = clamp((lighting.userData[0].y - worldPos.y) * 4.0, 0.0, 1.0);\n"
                    "    return lerp(1.0, fogFactor, below);\n";
            } else {
                source += "    return fogFactor;\n";
            }
            source += "}\n\nfloat3 forwardUserFragmentEnd(float3 color, float3 worldPos, float3 worldNormal)\n{\n";
            if (caustics) {
                source +=
                    "    const float4 params = lighting.userData[1];\n"
                    "    const float depthBelow = lighting.userData[0].x - worldPos.y;\n"
                    "    const float2 cuv = worldPos.xz * params.x;\n"
                    "    const float ct = lighting.userData[0].z * params.y;\n"
                    "    const float ca = userMap.Sample(materialSampler, cuv + float2(ct, ct * 0.8)).x;\n"
                    "    const float cb = userMap.Sample(materialSampler, cuv * 1.37 + float2(-ct * 1.1, ct * 0.9)).y;\n"
                    "    const float caustic = pow(clamp((ca + cb) * 0.5 * 2.2 - 0.9, 0.0, 1.0), 2.0);\n"
                    "    float gate = exp(-max(depthBelow, 0.0) / params.w) * clamp(depthBelow * 4.0, 0.0, 1.0);\n"
                    "    gate *= clamp(worldNormal.y, 0.0, 1.0);\n"
                    "    return color * (float3(1.0) + lighting.userData[2].rgb * (caustic * params.z * gate));\n";
            } else {
                source += "    return color;\n";
            }
            source += "}\n";
            return source;
        }
    }

    // The water surface's material: the water program, its switches as feature word 0, its block
    // filled by the script. Whether the target takes linear HDR is known only at draw time.
    class Water::WaterMaterial final : public Material
    {
    public:
        explicit WaterMaterial(const std::shared_ptr<GraphicsDevice>& device)
            : _device(device)
        {
            setName("WaterMaterial");
            setTransparent(true);
            setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
            auto depthState = std::make_shared<DepthState>();
            depthState->setDepthWrite(false);
            setDepthState(depthState);
            // Seen from below the surface too.
            setCullMode(CullMode::CULLFACE_NONE);
        }

        void setSwitches(const uint32_t switches)
        {
            if (_shaderBuilt && switches == _switches) {
                return;
            }
            _switches = switches;
            _shaderBuilt = true;
            ShaderFeatureSet features;
            features.setWord(0, switches);
            setShaderOverride(getOrCreateSlangShader(_device.get(), "water", {}, features));
        }

        WaterBlock& block()
        {
            markUniformsDirty();
            return _source;
        }

        const void* customUniformData(size_t& outSize) const override
        {
            _drawn = _source;
            _drawn.output[2] = _device && _device->hdrPass() ? 1.0f : 0.0f;
            outSize = sizeof(_drawn);
            return &_drawn;
        }

    private:
        std::shared_ptr<GraphicsDevice> _device;
        uint32_t _switches = 0;
        bool _shaderBuilt = false;
        WaterBlock _source;
        mutable WaterBlock _drawn;
    };

    Water::Water() = default;

    Water::~Water()
    {
        releaseAll();
    }

    void Water::setCameraEntity(Entity* entity)
    {
        if (_cameraEntity != entity) {
            requestDepthMap(false);
        }
        _cameraEntity = entity;
        _cameraWatch.watch(entity, [this]() {
            _cameraEntity = nullptr;
            _depthMapRequested = false;
        });
    }

    void Water::setLightEntity(Entity* entity)
    {
        _lightEntity = entity;
        _lightWatch.watch(entity, [this]() { _lightEntity = nullptr; });
    }

    void Water::applyUnderwaterEffects(Material& material, const bool heightFog, const bool caustics)
    {
        if (!heightFog && !caustics) {
            if (material.removeShaderChunk("forward-fragment-user")) {
                material.markUniformsDirty();
            }
            return;
        }
        material.setShaderChunk("forward-fragment-user", underwaterChunk(heightFog, caustics));
    }

    void Water::initialize()
    {
        // The settings may still be arriving (a script is initialized as it is created into a
        // running engine), so the surface is set up on the first update that has a camera.
        on("disable", [this]() {
            enableCameras(false);
            requestDepthMap(false);
        });
    }

    bool Water::setUp()
    {
        if (_material) {
            return true;
        }
        Engine* engine = entity() ? entity()->engine() : nullptr;
        if (!engine || !engine->graphicsDevice() || !engine->scene()) {
            return false;
        }
        if (!_cameraEntity || !_cameraEntity->findComponent<CameraComponent>()) {
            if (!_setUpReported) {
                spdlog::error("Water: the camera entity must be set to an entity with a camera component");
                _setUpReported = true;
            }
            return false;
        }
        auto* render = entity()->findComponent<RenderComponent>();
        if (!render) {
            if (!_setUpReported) {
                spdlog::error("Water: the entity needs a render component");
                _setUpReported = true;
            }
            return false;
        }
        _scene = engine->scene();

        _material = std::make_shared<WaterMaterial>(engine->graphicsDevice());
        for (MeshInstance* meshInstance : render->meshInstances()) {
            if (meshInstance) {
                _originalMaterials.emplace_back(meshInstance, meshInstance->material());
                meshInstance->setMaterial(_material.get());
            }
        }

        // The flat normal map used when none is given.
        TextureOptions options;
        options.name = "waterFlatNormalMap";
        options.width = 1;
        options.height = 1;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        _fallbackNormalMap = std::make_shared<Texture>(engine->graphicsDevice().get(), options);
        const uint8_t flat[4] = {128, 128, 255, 255};
        _fallbackNormalMap->setLevelData(0, flat, sizeof(flat));
        _fallbackNormalMap->upload();
        return true;
    }

    void Water::releaseAll()
    {
        requestDepthMap(false);
        for (const auto& [meshInstance, material] : _originalMaterials) {
            if (_material && meshInstance->material() == _material.get()) {
                meshInstance->setMaterial(material);
            }
        }
        _originalMaterials.clear();

        _reflectionWatch.reset();
        _refractionWatch.reset();
        delete _reflection.entity;
        _reflection = {};
        delete _refraction.entity;
        _refraction = {};

        if (auto scene = _scene.lock()) {
            if (scene->userShaderData().texture == _fallbackNormalMap.get() ||
                scene->userShaderData().texture == normalMap || scene->userShaderData().texture == causticsMap) {
                scene->setUserShaderTexture(nullptr);
            }
        }
        _scene.reset();
        _material.reset();
        _fallbackNormalMap.reset();
    }

    Water::PlanarCamera Water::createPlanarCamera(const char* name, const bool reflection)
    {
        auto* main = _cameraEntity->findComponent<CameraComponent>();
        const std::vector<int> waterLayers =
            entity()->findComponent<RenderComponent>() ? entity()->findComponent<RenderComponent>()->layers()
                                                        : std::vector<int>{};
        std::vector<int> layers;
        for (const int id : main->layers()) {
            if (std::find(waterLayers.begin(), waterLayers.end(), id) == waterLayers.end() && id != LAYERID_UI &&
                id != LAYERID_DEPTH) {
                layers.push_back(id);
            }
        }

        PlanarCamera result;
        result.entity = new Entity();
        result.entity->setName(entity()->name() + ":" + name);
        result.entity->setEngine(entity()->engine());
        auto* camera = static_cast<CameraComponent*>(result.entity->addComponent<CameraComponent>());
        camera->setLayers(layers);
        camera->setPriority(main->priority() - 1);
        result.entity->addComponent<ScriptComponent>();
        result.renderer = result.entity->script()->create<PlanarRenderer>();
        result.renderer->setSceneCameraEntity(_cameraEntity);
        result.renderer->setMode(reflection ? PlanarRenderer::Mode::Reflection : PlanarRenderer::Mode::Refraction);
        result.renderer->setScale(textureScale);
        result.renderer->setDepth(true);
        entity()->engine()->root()->addChild(result.entity);

        (reflection ? _reflectionWatch : _refractionWatch).watch(result.entity, [this, reflection]() {
            (reflection ? _reflection : _refraction) = {};
        });
        return result;
    }

    void Water::updatePlanarCamera(PlanarCamera& camera, const Vector3& planePoint, const Vector3& planeNormal)
    {
        auto* main = _cameraEntity->findComponent<CameraComponent>();
        if (auto* planar = camera.entity->findComponent<CameraComponent>()) {
            // Linear while the main camera's frame tone maps, as upstream's internal cameras
            // follow a camera whose own tone mapping is left linear there.
            planar->setToneMapping(main->onPostprocessing() ? TONEMAP_LINEAR : main->toneMapping());
        }
        camera.renderer->setPlane(planePoint, planeNormal);
        camera.renderer->setScale(textureScale);
        camera.renderer->setObliqueClipping(obliqueClipping);
        camera.renderer->setClipBias(clipBias);
    }

    void Water::enableCameras(const bool value)
    {
        if (_reflection.entity) {
            _reflection.entity->setEnabled(value);
        }
        if (_refraction.entity) {
            _refraction.entity->setEnabled(value);
        }
    }

    void Water::requestDepthMap(const bool value)
    {
        if (_depthMapRequested == value) {
            return;
        }
        _depthMapRequested = value;
        if (auto* camera = _cameraEntity ? _cameraEntity->findComponent<CameraComponent>() : nullptr) {
            camera->requestSceneDepthMap(value);
        }
    }

    void Water::postUpdate(const float dt)
    {
        if (!setUp()) {
            return;
        }
        auto scene = _scene.lock();
        auto* mainCamera = _cameraEntity ? _cameraEntity->findComponent<CameraComponent>() : nullptr;
        if (!_material || !scene || !mainCamera) {
            return;
        }
        _time += dt;

        const bool skyReflection = reflectionSource == WaterReflection::Sky && scene->envAtlas();
        const bool planarReflection = reflectionSource == WaterReflection::Planar;
        uint32_t switches = 0;
        switches |= planarReflection ? kReflectionPlanar : 0u;
        switches |= skyReflection ? kReflectionSky : 0u;
        switches |= refraction ? kRefraction : 0u;
        switches |= depthEffects ? kDepthEffects : 0u;
        switches |= foam && depthEffects ? kFoam : 0u;
        switches |= waves ? kWaves : 0u;
        _material->setSwitches(switches);

        // The scene depth for the depth effects.
        requestDepthMap(depthEffects);

        // The water plane: the entity's position and up axis.
        const Matrix4& transform = entity()->worldTransform();
        const Vector3 planePoint = Vector3(transform.getColumn(3));
        const Vector3 planeNormal = Vector3(transform.getColumn(1)).normalized();

        // With the camera under the water the internal cameras get the plane flipped: the mirrored
        // camera captures the underwater scene (what total internal reflection shows), the clipped
        // refraction camera the world above the surface.
        const Matrix4& cameraTransform = _cameraEntity->worldTransform();
        const Vector3 cameraPosition = Vector3(cameraTransform.getColumn(3));
        const bool submerged = planeNormal.dot(cameraPosition) < planeNormal.dot(planePoint);
        const Vector3 clipNormal = submerged ? planeNormal * -1.0f : planeNormal;

        if (planarReflection) {
            if (!_reflection.entity) {
                _reflection = createPlanarCamera("WaterReflection", true);
            }
            _reflection.entity->setEnabled(true);
            updatePlanarCamera(_reflection, planePoint, clipNormal);
            if (Texture* texture = _reflection.renderer->frameUpdate()) {
                _material->setBaseColorTexture(texture);
            }
        } else if (_reflection.entity) {
            _reflection.entity->setEnabled(false);
        }

        if (refraction) {
            if (!_refraction.entity) {
                _refraction = createPlanarCamera("WaterRefraction", false);
            }
            _refraction.entity->setEnabled(true);
            updatePlanarCamera(_refraction, planePoint, clipNormal);
            if (Texture* texture = _refraction.renderer->frameUpdate()) {
                _material->setEmissiveTexture(texture);
            }
        } else if (_refraction.entity) {
            _refraction.entity->setEnabled(false);
        }

        Texture* ripples = normalMap ? normalMap : _fallbackNormalMap.get();
        if (_material->normalTexture() != ripples) {
            _material->setNormalTexture(ripples);
        }

        WaterBlock& block = _material->block();
        linearColor(shallowColor, block.shallowColor);
        block.shallowColor[3] = rippleTiling;
        linearColor(deepColor, block.deepColor);
        block.deepColor[3] = rippleSpeed;
        block.lightDir[3] = bumpiness;
        block.lightColor[3] = distortion;
        store3(block.cameraPosition, cameraPosition);
        block.cameraPosition[3] = _time;
        store3(block.cameraForward, Vector3(cameraTransform.getColumn(2)).normalized() * -1.0f);
        block.cameraForward[3] = skyBlur;
        block.params0[0] = fresnelPower;
        block.params0[1] = reflectionStrength;
        block.params0[2] = depthFade;
        block.params0[3] = shoreSoftness;
        block.params1[0] = opacity;
        block.params1[1] = specularPower;
        block.params2[0] = waveFiltering;
        block.params2[2] = submerged ? 1.0f : 0.0f;
        block.params2[3] = snellWindow;

        // The distance fog of the surface seen from under the water: the scene fog when it has one.
        const FogParams& fog = scene->fog();
        const bool sceneFog = fog.enabled && fog.type != FogType::FOG_NONE;
        block.params2[1] = sceneFog ? fog.density : 0.06f;
        linearColor(sceneFog ? fog.color : deepColor, block.underwaterFogColor);

        // The directional light, travelling along the light entity's -Y.
        auto* light = _lightEntity ? _lightEntity->findComponent<LightComponent>() : nullptr;
        if (light) {
            const Vector3 up = Vector3(_lightEntity->worldTransform().getColumn(1)).normalized();
            store3(block.lightDir, up * -1.0f);
            linearColor(light->color(), block.lightColor, light->intensity());
            block.params1[2] = specularIntensity;
            block.params1[3] = diffuseIntensity;
        } else {
            store3(block.lightDir, Vector3(0.0f, -1.0f, 0.0f));
            store3(block.lightColor, Vector3(0.0f, 0.0f, 0.0f));
            block.params1[2] = 0.0f;
            block.params1[3] = 0.0f;
        }

        linearColor(foamColor, block.foam);
        block.foam[3] = foamDepth;

        block.waveParams[0] = waveAmplitude;
        block.waveParams[1] = 2.0f * std::numbers::pi_v<float> / std::max(waveLength, 0.01f);
        block.waveParams[2] = waveSpeed;
        block.waveParams[3] = waveSteepness;
        block.swellParams[0] = swellAmplitude;
        block.swellParams[1] = 2.0f * std::numbers::pi_v<float> / std::max(swellLength, 0.01f);
        block.swellParams[2] = swellSpeed;
        block.swellParams[3] = waveSteepness;
        block.waveDirs[0] = std::cos(waveDirection * kDegToRad);
        block.waveDirs[1] = std::sin(waveDirection * kDegToRad);
        block.waveDirs[2] = std::cos(swellDirection * kDegToRad);
        block.waveDirs[3] = std::sin(swellDirection * kDegToRad);

        const Camera* camera = mainCamera->camera();
        block.output[0] = camera->nearClip();
        block.output[1] = camera->farClip();
        block.output[3] = static_cast<float>(envAtlasEncodingOf(scene->envAtlas()));

        // The scene-wide values of the underwater chunk on other materials. Under the water the
        // fog's gate rises above the waterline, so the wave troughs and what lies just under the
        // surface are fogged too.
        const float waterLevel = planeNormal.dot(planePoint);
        const float waveMargin = waves ? waveAmplitude * 2.0f + swellAmplitude * 1.7f : 0.0f;
        scene->setUserShaderData(kUserLevels,
            Vector4(waterLevel, waterLevel + (submerged ? waveMargin + 0.5f : 0.0f), _time, 0.0f));
        scene->setUserShaderData(kUserCaustics, Vector4(causticsTiling, causticsSpeed, causticsStrength, causticsDepth));
        scene->setUserShaderData(kUserCausticsColor,
            Vector4(block.lightColor[0], block.lightColor[1], block.lightColor[2], 0.0f));
        scene->setUserShaderTexture(causticsMap ? causticsMap : ripples);
    }
}
