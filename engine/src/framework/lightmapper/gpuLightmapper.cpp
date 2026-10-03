// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.08.2026
//
#include "gpuLightmapper.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

#include <spdlog/spdlog.h>

#include "lightmapFilters.h"
#include "lightmapFilterShaders.h"
#include "scene/graphics/quadShader.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/components/componentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/lightRadiance.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphNode.h"
#include "scene/graphics/quadRender.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float GOLDEN_ANGLE = 2.399963229728653f;

        /// Evenly spread points in a unit disc.
        void circlePointDeterministic(float& x, float& y, const int index, const int numPoints)
        {
            const float theta = static_cast<float>(index) * GOLDEN_ANGLE;
            const float r = std::sqrt(static_cast<float>(index) / static_cast<float>(std::max(numPoints, 1)));
            x = r * std::cos(theta);
            y = r * std::sin(theta);
        }

        int nextPowerOfTwo(int value)
        {
            int result = 1;
            while (result < value) {
                result <<= 1;
            }
            return result;
        }

        /// The resolution follows the mesh's world-space
        /// bounds, so large surfaces get more texels than small ones. `maxSize` is the
        /// device's own texture limit — a bounds-derived resolution has no other ceiling,
        /// so a large enough mesh would otherwise ask for a texture the driver refuses.
        int lightmapSizeFor(MeshInstance* meshInstance, const GpuLightmapper::Options& options,
            const int maxSize)
        {
            if (options.lightmapSizeMultiplier <= 0.0f || !meshInstance) {
                return std::clamp(options.lightmapSize, 8, maxSize);
            }
            const BoundingBox aabb = meshInstance->aabb();
            const Vector3 half = aabb.halfExtents();
            const float totalArea = std::sqrt(
                half.getY() * half.getZ() + half.getX() * half.getZ() + half.getX() * half.getY());
            return std::clamp(nextPowerOfTwo(static_cast<int>(totalArea * options.lightmapSizeMultiplier)),
                8, std::clamp(options.lightmapMaxResolution, 8, maxSize));
        }

        // HDR: the bake stores LINEAR light, and the sun alone exceeds 1.0 before
        // exposure — an 8-bit target would clamp it to white and flatten the scene.
        // The accumulating virtual-light passes need the headroom too.
        std::shared_ptr<Texture> createLightmapTexture(GraphicsDevice* device, const int size,
            const char* name)
        {
            TextureOptions texOptions;
            texOptions.width = static_cast<uint32_t>(size);
            texOptions.height = static_cast<uint32_t>(size);
            texOptions.format = PixelFormat::PIXELFORMAT_RGBA16F;
            texOptions.mipmaps = false;
            texOptions.name = name;
            texOptions.profilerHint = TexHint::TEXHINT_LIGHTMAP;
            return std::make_shared<Texture>(device, texOptions);
        }

        std::shared_ptr<RenderTarget> createLightmapTarget(GraphicsDevice& device, Texture* texture,
            const bool depth)
        {
            RenderTargetOptions rtOptions;
            rtOptions.graphicsDevice = &device;
            rtOptions.colorBuffer = texture;
            rtOptions.depth = depth;
            rtOptions.samples = 1;
            rtOptions.name = "gpuLightmapTarget";
            return device.createRenderTarget(rtOptions);
        }

        // A quad pass over one lightmap-sized target, outside the frame graph. The device
        // handle is non-owning for the same reason as the environment bakes' pass: it lives
        // for one draw inside an offline scope the device outlives.
        struct LightmapFilterPass final : RenderPass
        {
            LightmapFilterPass(GraphicsDevice* device, const char* name)
                : RenderPass(std::shared_ptr<GraphicsDevice>(device, [](GraphicsDevice*) {}))
            {
                _name = name;
            }
        };

        enum class FilterKind { Dilate, Denoise, AmbientOcclusion, Copy };

        std::shared_ptr<Shader> filterShader(GraphicsDevice* device, const FilterKind kind)
        {
            const char* cacheKey = "lightmap-copy";
            const char* define = "#define LM_COPY 1\n";
            switch (kind) {
            case FilterKind::Dilate:
                cacheKey = "lightmap-dilate";
                define = "#define LM_DILATE 1\n";
                break;
            case FilterKind::Denoise:
                cacheKey = "lightmap-denoise";
                define = "#define LM_DENOISE 1\n";
                break;
            case FilterKind::AmbientOcclusion:
                cacheKey = "lightmap-ambient-ao";
                define = "#define LM_AMBIENT_AO 1\n";
                break;
            case FilterKind::Copy:
                break;
            }
            // GLSL needs its #version line first, so the pass switch follows it.
            return getOrCreateQuadShader(device, cacheKey, "lightmapFilterVertex", "lightmapFilterFragment",
                [define](const bool glsl) {
                    return glsl
                        ? std::string("#version 450\n") + define + lightmap_filter_shaders::LIGHTMAP_FILTER_GLSL
                        : std::string(define) + lightmap_filter_shaders::LIGHTMAP_FILTER_MSL;
                });
        }

        /// One full-target quad: `sources` on slots 0.., writing `target`. A pass draws
        /// outside the frame graph, so it sets every piece of state the pipeline reads.
        void drawFilter(GraphicsDevice* device, const std::shared_ptr<Shader>& shader,
            const std::shared_ptr<RenderTarget>& target, std::initializer_list<Texture*> sources,
            const lightmap_filters::LightmapFilterUniforms& uniforms, const char* name)
        {
            if (!shader || !target) {
                return;
            }
            QuadRender quad(shader);
            size_t slot = 0;
            for (auto* source : sources) {
                quad.setTexture(slot++, source);
            }
            quad.setUniforms(uniforms);

            LightmapFilterPass pass(device, name);
            pass.init(target);
            device->startRenderPass(&pass);
            device->setBlendState(BlendState::noBlend());
            device->setDepthState(DepthState::noDepth());
            device->setCullMode(CullMode::CULLFACE_NONE);
            device->setStencilState();
            quad.render();
            device->endRenderPass(&pass);
        }
    }


    GpuLightmapper::GpuLightmapper(Engine* engine) : _engine(engine)
    {
    }

    GpuLightmapper::~GpuLightmapper()
    {
        destroyBakeNodes();
    }

    void GpuLightmapper::bake(const std::vector<MeshInstance*>& targets, const Options& options)
    {
        if (!_engine || targets.empty()) {
            return;
        }
        destroyBakeNodes();

        _options = options;
        // Clamp these to their valid ranges.
        _options.ambientBakeNumSamples = std::clamp(_options.ambientBakeNumSamples, 1, 255);
        _options.ambientBakeSpherePart = std::clamp(_options.ambientBakeSpherePart, 0.001f, 1.0f);
        _options.lightmapFilterRange = std::max(_options.lightmapFilterRange, 0.001f);
        _targets = targets;
        _lightmaps.clear();
        _originalMasks.clear();

        for (size_t i = 0; i < _targets.size(); ++i) {
            prepareTarget(i);
        }
        widenLightsForBake();
        prepareDirectionalSamples();
        if (_options.ambientBake) {
            setupAmbientLight();
        }

        // The ambient light bakes first, from a black lightmap, so its occlusion
        // curve shapes the ambient alone; the scene lights add on top afterwards.
        startPhase(_options.ambientBake ? Phase::AmbientLight : Phase::Direct, 0);

        _pending = true;
        spdlog::info("GpuLightmapper: baking {} mesh(es) in UV space", _targets.size());
    }

    void GpuLightmapper::prepareTarget(const size_t index)
    {
        auto* meshInstance = _targets[index];
        if (!meshInstance || !meshInstance->mesh()) {
            _lightmaps.push_back(nullptr);   // keeps lightmaps() aligned with the targets
            return;
        }

        const auto& device = _engine->graphicsDevice();
        const int size = lightmapSizeFor(meshInstance, _options, device->maxTextureSize());
        auto texture = createLightmapTexture(device.get(), size, "gpuLightmap");
        auto renderTarget = createLightmapTarget(*device, texture.get(), true);

        // The ambient bake's visibility accumulates apart from the lightmap, so its curve
        // and the multiply by the ambient light can be applied to it alone.
        if (_options.ambientBake) {
            auto occlusion = createLightmapTexture(device.get(), size, "gpuLightmapOcclusion");
            _occlusionRT.push_back(createLightmapTarget(*device, occlusion.get(), true));
            _occlusionTextures.push_back(std::move(occlusion));
        }

        // A private layer per target: the bake camera renders exactly one mesh, so
        // its unwrap owns the whole target.
        auto layer = std::make_shared<Layer>("LightmapBake" + std::to_string(index),
            _options.baseLayerId + static_cast<int>(index));
        layer->addMeshInstances({meshInstance});
        _engine->scene()->layers()->pushOpaque(layer);

        // Mask scheme: the bake lights carry MASK_BAKE, so the mesh wears
        // MASK_BAKE while it is being baked (those lights reach it) and switches to
        // MASK_AFFECT_LIGHTMAPPED afterwards (they no longer do, and the bake is not
        // applied twice). Remember what it had so a failed bake can restore it.
        // The mesh is lit through MASK_AFFECT_LIGHTMAPPED during the bake — see the
        // light-mask note in widenLightsForBake for why not MASK_BAKE — and keeps that
        // mask after, which is what stops the bake lights (back on MASK_BAKE) from
        // lighting it a second time at runtime.
        _originalMasks.push_back(meshInstance->mask());
        meshInstance->setMask(MASK_AFFECT_LIGHTMAPPED);

        _cameras.push_back(createBakeCamera(*layer, renderTarget));
        _lightmaps.push_back(std::move(texture));
        _targetsRT.push_back(std::move(renderTarget));
        _layers.push_back(std::move(layer));
    }

    Entity* GpuLightmapper::createBakeCamera(const Layer& layer, const std::shared_ptr<RenderTarget>& renderTarget)
    {
        auto* cameraEntity = new Entity();
        cameraEntity->setName("LightmapBakeCamera");
        cameraEntity->setEngine(_engine);
        _engine->root()->addChild(cameraEntity);

        auto* cameraComponent = static_cast<CameraComponent*>(
            cameraEntity->addComponent<CameraComponent>());
        cameraComponent->setLayers({layer.id()});

        Camera* camera = cameraComponent->camera();
        camera->setRenderTarget(renderTarget);
        camera->setLightmapBakePass(true);
        // Alpha 0: a texel the unwrap never covers stays at alpha 0, and the dilate and
        // denoise read alpha as "baked" (lightmapFilterShaders.h).
        camera->setClearColor(Color(0.0f, 0.0f, 0.0f, 0.0f));
        // The UV-space vertex stage ignores this transform; it exists only so the
        // directional shadow cascades are fitted to the scene rather than to a
        // degenerate frustum. Place it back from the scene and look at its centre.
        camera->setFarClip(std::max(_options.bakeCameraDistance * 4.0f, 100.0f));
        cameraEntity->setLocalPosition(
            _options.bakeCameraTarget + Vector3(1.0f, 1.0f, 1.0f).normalized() *
                _options.bakeCameraDistance);
        cameraEntity->lookAt(_options.bakeCameraTarget);
        return cameraEntity;
    }

    std::vector<int> GpuLightmapper::bakeLayerIds() const
    {
        std::vector<int> layerIds;
        layerIds.reserve(_layers.size());
        for (const auto& layer : _layers) {
            if (layer) {
                layerIds.push_back(layer->id());
            }
        }
        return layerIds;
    }

    void GpuLightmapper::widenLightsForBake()
    {
        // Lights are filtered per layer, so every scene light has to be told about the
        // private bake layers or the bake would only pick up ambient. Their original
        // layer lists are restored once the bake is collected.
        const std::vector<int> bakeLayers = bakeLayerIds();
        // Every instance, including inactive ones: this only widens and then
        // restores each light's layer list, and a light disabled during the bake
        // still has to get its own list back.
        for (auto* lightComponent : LightComponent::instances()) {
            if (!lightComponent) {
                continue;
            }
            _lightLayerBackup.emplace_back(lightComponent, lightComponent->layers());
            _lightEnabledBackup.emplace_back(lightComponent, lightComponent->enabled());
            std::vector<int> layerIds = lightComponent->layers();
            layerIds.insert(layerIds.end(), bakeLayers.begin(), bakeLayers.end());
            lightComponent->setLayers(layerIds);

            // A light on MASK_BAKE reports castShadows() == false by design
            // (Light::castShadows excludes MASK_BAKE and MASK_NONE),
            // which suppresses its shadow map — so a bake light would light the texels
            // but cast nothing. Lift such lights to MASK_AFFECT_LIGHTMAPPED for the bake
            // and restore their authored mask afterwards.
            _lightMaskBackup.emplace_back(lightComponent, lightComponent->mask());
            if (lightComponent->mask() == MASK_BAKE) {
                lightComponent->setMask(MASK_AFFECT_LIGHTMAPPED);
            }
        }
    }

    void GpuLightmapper::prepareDirectionalSamples()
    {
        // Directional lights bake as N virtual copies when soft shadows are asked for, so
        // they sit out the direct-light frame and contribute one accumulated pass each.
        _directionalLights.clear();
        _dirSampleCount = 0;
        if (!(_options.directionalBakeNumSamples > 1 && _options.directionalBakeArea > 0.0f)) {
            return;
        }
        for (auto* lightComponent : LightComponent::instances()) {
            if (!lightComponent || !lightComponent->active() ||
                lightComponent->type() != LightType::LIGHTTYPE_DIRECTIONAL) {
                continue;
            }
            auto* node = lightComponent->entity();
            _directionalLights.emplace_back(lightComponent,
                node ? node->localRotation() : Quaternion(),
                lightComponent->intensity(), lightComponent->luminance());
        }
        if (!_directionalLights.empty()) {
            _dirSampleCount = _options.directionalBakeNumSamples;
        }
    }

    void GpuLightmapper::setupAmbientLight()
    {
        // The ambient bake light: a white directional caster with PCF3 shadows
        // at 2048. DEVIATION: shadow bias 0.05 rather than upstream's 0.2 — the authoring
        // value here feeds a polygon offset, and 0.2 pushes small casters out of their own
        // shadows (the same conversion the lightmap examples make for their lights).
        _ambientLightEntity = new Entity();
        _ambientLightEntity->setName("AmbientLight");
        _ambientLightEntity->setEngine(_engine);
        _engine->root()->addChild(_ambientLightEntity);

        _ambientLight = static_cast<LightComponent*>(
            _ambientLightEntity->addComponent<LightComponent>());
        _ambientLight->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        _ambientLight->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _ambientLight->setCastShadows(true);
        _ambientLight->setShadowType(ShadowType::SHADOW_PCF3_32F);
        _ambientLight->setShadowResolution(2048);
        _ambientLight->setShadowBias(0.05f);
        _ambientLight->setShadowNormalBias(0.05f);
        _ambientLight->setNumCascades(1);
        _ambientLight->setShadowDistance(_options.bakeCameraDistance * 4.0f);
        _ambientLight->setMask(MASK_AFFECT_LIGHTMAPPED);
        _ambientLight->setLayers(bakeLayerIds());
        _ambientLight->setEnabled(false);   // enabled for the occlusion frames only
    }

    template <typename Predicate>
    void GpuLightmapper::enableSceneLights(Predicate keep)
    {
        for (auto& [lightComponent, enabled] : _lightEnabledBackup) {
            if (lightComponent) {
                lightComponent->setEnabled(enabled && keep(*lightComponent));
            }
        }
    }

    void GpuLightmapper::configureCameras(const bool occlusionTargets, const bool clear,
        const bool accumulate)
    {
        for (size_t i = 0; i < _cameras.size(); ++i) {
            auto* cameraEntity = _cameras[i];
            auto* cameraComponent = cameraEntity ? cameraEntity->findComponent<CameraComponent>() : nullptr;
            Camera* camera = cameraComponent ? cameraComponent->camera() : nullptr;
            if (!camera) {
                continue;
            }
            const auto& target = (occlusionTargets && i < _occlusionRT.size()) ? _occlusionRT[i] : _targetsRT[i];
            camera->setRenderTarget(target);
            camera->setClearColorBuffer(clear);
            // An accumulating frame adds its own light to the target with additive
            // blending (VT_FEATURE_LIGHTMAP_BAKE_ACCUM drops the ambient from it).
            camera->setLightmapBakeAccumulate(accumulate);
        }
        // The composition caches its render actions with the clear flags and the target;
        // both are in its fingerprint, but say so anyway — this is a one-off per frame.
        if (const auto& layers = _engine->scene()->layers()) {
            layers->markDirty();
        }
    }

    void GpuLightmapper::startPhase(const Phase phase, const int sample)
    {
        _phase = phase;
        _sample = sample;
        if (_ambientLight) {
            _ambientLight->setEnabled(phase == Phase::AmbientOcclusion);
        }
        const bool softDirectional = _dirSampleCount > 0;

        switch (phase) {
        case Phase::AmbientLight:
            // No light at all: what the frame writes is the ambient irradiance per texel —
            // the env atlas or the flat ambient, whichever the scene has — the term
            // the occlusion multiplies.
            enableSceneLights([](const LightComponent&) { return false; });
            configureCameras(false, true, false);
            break;
        case Phase::AmbientOcclusion:
            enableSceneLights([](const LightComponent&) { return false; });
            prepareAmbientSample(sample);
            configureCameras(true, sample == 0, true);
            break;
        case Phase::Direct:
            // The soft directional lights come in their own frames.
            enableSceneLights([softDirectional](const LightComponent& light) {
                return !(softDirectional && light.type() == LightType::LIGHTTYPE_DIRECTIONAL);
            });
            // Without the ambient bake this is the first frame and writes the unoccluded
            // ambient with the lights; with it, it adds the lights to the ambient.
            configureCameras(false, !_options.ambientBake, _options.ambientBake);
            break;
        case Phase::DirectionalSample:
            enableSceneLights([](const LightComponent& light) {
                return light.type() == LightType::LIGHTTYPE_DIRECTIONAL;
            });
            prepareDirectionalSample(sample);
            configureCameras(false, false, true);
            break;
        case Phase::Done:
            break;
        }
    }

    bool GpuLightmapper::update()
    {
        if (!_pending) {
            return false;
        }

        // The frame just rendered did the current phase's work; set up the next one.
        switch (_phase) {
        case Phase::AmbientLight:
            startPhase(Phase::AmbientOcclusion, 0);
            return false;
        case Phase::AmbientOcclusion:
            if (_sample + 1 < _options.ambientBakeNumSamples) {
                startPhase(Phase::AmbientOcclusion, _sample + 1);
                return false;
            }
            applyAmbientOcclusion();
            startPhase(Phase::Direct, 0);
            return false;
        case Phase::Direct:
            if (_dirSampleCount > 0) {
                startPhase(Phase::DirectionalSample, 0);
                return false;
            }
            break;
        case Phase::DirectionalSample:
            if (_sample + 1 < _dirSampleCount) {
                startPhase(Phase::DirectionalSample, _sample + 1);
                return false;
            }
            break;
        case Phase::Done:
            break;
        }
        _phase = Phase::Done;

        postprocessLightmaps();

        for (size_t i = 0; i < _targets.size(); ++i) {
            auto* meshInstance = _targets[i];
            if (!meshInstance || i >= _lightmaps.size() || !_lightmaps[i]) {
                continue;
            }
            meshInstance->setMask(MASK_AFFECT_LIGHTMAPPED);
            // The bake belongs to the MESH INSTANCE: meshes that
            // share one material each keep their own, and the material's lightMap is
            // left alone. Written into a shared material, every mesh using it would show
            // whichever target was baked last.
            meshInstance->setLightMap(_lightmaps[i]);
        }

        // The bake is one-shot: drop the cameras and layers so the scene renders normally.
        destroyBakeNodes();
        _pending = false;
        spdlog::info("GpuLightmapper: bake complete, {} lightmap(s) applied", _lightmaps.size());
        return true;
    }

    void GpuLightmapper::setLightmapsEnabled(const bool enabled)
    {
        for (size_t i = 0; i < _targets.size() && i < _lightmaps.size(); ++i) {
            if (auto* meshInstance = _targets[i]) {
                meshInstance->setLightMap(enabled ? _lightmaps[i] : nullptr);
            }
        }
    }

    void GpuLightmapper::prepareDirectionalSample(const int index)
    {
        for (auto& [lightComponent, rotation, intensity, luminance] : _directionalLights) {
            if (!lightComponent) {
                continue;
            }
            auto* node = lightComponent->entity();
            if (!node) {
                continue;
            }
            // Sample 0 keeps the authored direction, the rest are
            // rotated by a disc point scaled to half the bake area.
            node->setLocalRotation(rotation);
            if (index > 0) {
                float dx = 0.0f, dy = 0.0f;
                circlePointDeterministic(dx, dy, index, _dirSampleCount);
                const float half = _options.directionalBakeArea * 0.5f;
                node->rotateLocal(dx * half, 0.0f, dy * half);
            }
            // The lightmap accumulates in linear space, so the copies split the LINEAR
            // scale the light shades with, and the N of them sum to the live light. A
            // light under an intensity of 1 shades at its 2.2 power (lightRadiance), so
            // the split goes through that curve and back rather than dividing the
            // authored intensity, which would make each copy far too dark. A directional
            // light's unit conversion is 1, so its luminance splits the same way.
            const float share = 1.0f / static_cast<float>(std::max(_dirSampleCount, 1));
            lightComponent->setIntensity(intensityForLinearScale(linearScaleForIntensity(intensity) * share));
            lightComponent->setLuminance(intensityForLinearScale(linearScaleForIntensity(luminance) * share));
        }
    }

    void GpuLightmapper::prepareAmbientSample(const int index)
    {
        if (!_ambientLight || !_ambientLightEntity) {
            return;
        }
        // Virtual ambient light: a point on the sphere part, the light aimed back along it
        // (lookAt(-point) then rotateLocal(90,0,0), since a light emits along its node's -Y
        // while lookAt aims -Z), at the intensity ambientVirtualLightIntensity derives.
        const int numSamples = _options.ambientBakeNumSamples;
        const Vector3 point = lightmap_filters::spherePointDeterministic(index, numSamples,
            _options.ambientBakeSpherePart);
        _ambientLightEntity->setLocalPosition(0.0f, 0.0f, 0.0f);
        _ambientLightEntity->lookAt(point * -1.0f);
        _ambientLightEntity->rotateLocal(90.0f, 0.0f, 0.0f);
        _ambientLight->setIntensity(lightmap_filters::ambientVirtualLightIntensity(numSamples,
            _options.ambientBakeSpherePart));
    }

    const std::shared_ptr<RenderTarget>& GpuLightmapper::tempTarget(const int size)
    {
        auto& entry = _tempTargets[size];
        if (!entry.second) {
            GraphicsDevice* device = _engine->graphicsDevice().get();
            entry.first = createLightmapTexture(device, size, "gpuLightmapTemp");
            entry.first->upload();
            entry.second = createLightmapTarget(*device, entry.first.get(), false);
        }
        return entry.second;
    }

    void GpuLightmapper::applyAmbientOcclusion()
    {
        GraphicsDevice* device = _engine->graphicsDevice().get();
        const auto aoShader = filterShader(device, FilterKind::AmbientOcclusion);
        const auto copyShader = filterShader(device, FilterKind::Copy);
        if (!aoShader || !copyShader) {
            spdlog::error("GpuLightmapper: no ambient-occlusion shader for this device");
            return;
        }

        lightmap_filters::LightmapFilterUniforms uniforms;
        uniforms.occlusionContrast = _options.ambientBakeOcclusionContrast;
        uniforms.occlusionBrightness = _options.ambientBakeOcclusionBrightness;

        // Targets first: building one inside the offline scope would queue its image's
        // transition behind the work that uses it.
        for (const auto& rt : _targetsRT) {
            if (rt && rt->colorBuffer()) {
                tempTarget(static_cast<int>(rt->colorBuffer()->width()));
            }
        }

        device->beginOfflineWork();
        for (size_t i = 0; i < _targetsRT.size() && i < _occlusionTextures.size(); ++i) {
            const auto& lightmapRT = _targetsRT[i];
            Texture* lightmap = lightmapRT ? lightmapRT->colorBuffer() : nullptr;
            if (!lightmap || !_occlusionTextures[i]) {
                continue;
            }
            const auto& temp = tempTarget(static_cast<int>(lightmap->width()));
            // The last ambient pass.
            drawFilter(device, aoShader, temp, {_occlusionTextures[i].get(), lightmap},
                uniforms, "LightmapAmbientOcclusion");
            drawFilter(device, copyShader, lightmapRT, {temp->colorBuffer()}, uniforms,
                "LightmapAmbientCopy");
        }
        device->endOfflineWork();

        // The occlusion targets are done with. A texture still referenced by work in
        // flight is kept by the backend (Metal's command buffer retains it, Vulkan defers
        // the destruction), so they can go now.
        _occlusionRT.clear();
        _occlusionTextures.clear();
    }

    void GpuLightmapper::postprocessLightmaps()
    {
        GraphicsDevice* device = _engine->graphicsDevice().get();
        const auto dilateShader = filterShader(device, FilterKind::Dilate);
        const bool filter = _options.lightmapFilterEnabled;
        const auto denoiseShader = filter ? filterShader(device, FilterKind::Denoise) : nullptr;
        if (!dilateShader || (filter && !denoiseShader)) {
            spdlog::error("GpuLightmapper: no lightmap filter shaders for this device");
            return;
        }

        lightmap_filters::LightmapFilterUniforms uniforms;
        if (filter) {
            lightmap_filters::prepareDenoise(uniforms, _options.lightmapFilterRange,
                _options.lightmapFilterSmoothness);
        }

        for (const auto& rt : _targetsRT) {
            if (rt && rt->colorBuffer()) {
                tempTarget(static_cast<int>(rt->colorBuffer()->width()));
            }
        }

        // Post-processing, one colour pass (BAKE_COLOR): the first of the two
        // draws is the denoise when the filter is on and a dilate otherwise, the second a
        // dilate back into the lightmap — so the lightmap is dilated once or twice.
        device->beginOfflineWork();
        for (const auto& lightmapRT : _targetsRT) {
            Texture* lightmap = lightmapRT ? lightmapRT->colorBuffer() : nullptr;
            if (!lightmap) {
                continue;
            }
            const auto& temp = tempTarget(static_cast<int>(lightmap->width()));
            lightmap_filters::prepare(uniforms, static_cast<int>(lightmap->width()),
                static_cast<int>(lightmap->height()));
            drawFilter(device, filter ? denoiseShader : dilateShader, temp, {lightmap}, uniforms,
                filter ? "LightmapDenoise" : "LightmapDilate");
            drawFilter(device, dilateShader, lightmapRT, {temp->colorBuffer()}, uniforms,
                "LightmapDilate");
        }
        device->endOfflineWork();
    }

    void GpuLightmapper::destroyBakeNodes()
    {
        // Restore the directional lights the virtual copies borrowed.
        for (auto& [lightComponent, rotation, intensity, luminance] : _directionalLights) {
            if (!lightComponent) {
                continue;
            }
            lightComponent->setIntensity(intensity);
            lightComponent->setLuminance(luminance);
            if (auto* node = lightComponent->entity()) {
                node->setLocalRotation(rotation);
            }
        }
        _directionalLights.clear();
        _dirSampleCount = 0;

        // Every scene light back to the enabled state it had before the bake.
        enableSceneLights([](const LightComponent&) { return true; });
        _lightEnabledBackup.clear();

        delete _ambientLightEntity;
        _ambientLightEntity = nullptr;
        _ambientLight = nullptr;

        for (auto& [lightComponent, layerIds] : _lightLayerBackup) {
            if (lightComponent) {
                lightComponent->setLayers(layerIds);
            }
        }
        _lightLayerBackup.clear();

        for (auto& [lightComponent, mask] : _lightMaskBackup) {
            if (lightComponent) {
                lightComponent->setMask(mask);
            }
        }
        _lightMaskBackup.clear();

        for (auto* cameraEntity : _cameras) {
            delete cameraEntity;   // detaches from the parent in ~GraphNode
        }
        _cameras.clear();

        if (_engine) {
            if (const auto& layers = _engine->scene()->layers()) {
                for (const auto& layer : _layers) {
                    if (layer) {
                        layer->setEnabled(false);
                    }
                }
                layers->markDirty();
            }
        }
        _layers.clear();
        _targetsRT.clear();
        _occlusionRT.clear();
        _occlusionTextures.clear();
        _tempTargets.clear();
        _phase = Phase::Done;
    }
}
