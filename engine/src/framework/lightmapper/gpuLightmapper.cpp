// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "gpuLightmapper.h"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/components/componentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphNode.h"
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

        /// Upstream random.circlePointDeterministic — evenly spread points in a unit disc.
        void circlePointDeterministic(float& x, float& y, const int index, const int numPoints)
        {
            const float theta = static_cast<float>(index) * GOLDEN_ANGLE;
            const float r = std::sqrt(static_cast<float>(index) / static_cast<float>(std::max(numPoints, 1)));
            x = r * std::cos(theta);
            y = r * std::sin(theta);
        }

        /// Upstream random.spherePointDeterministic, covering the top `end` part of the sphere.
        Vector3 spherePointDeterministic(const int index, const int numPoints, const float end)
        {
            const float finish = 1.0f - 2.0f * end;
            const float t = static_cast<float>(index) / static_cast<float>(std::max(numPoints, 1));
            const float y = 1.0f + (finish - 1.0f) * t;
            const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const float theta = GOLDEN_ANGLE * static_cast<float>(index);
            return Vector3(std::cos(theta) * radius, y, std::sin(theta) * radius);
        }

        int nextPowerOfTwo(int value)
        {
            int result = 1;
            while (result < value) {
                result <<= 1;
            }
            return result;
        }

        /// Upstream calculateLightmapSize: the resolution follows the mesh's world-space
        /// bounds, so large surfaces get more texels than small ones. `maxSize` is the
        /// device's own texture limit — a bounds-derived resolution has no other ceiling,
        /// so a large enough mesh would otherwise ask for a texture the driver refuses.
        int lightmapSizeFor(MeshInstance* meshInstance, const GpuLightmapper::Options& options,
            const int maxSize)
        {
            if (options.sizeMultiplier <= 0.0f || !meshInstance) {
                return std::clamp(options.lightmapSize, 8, maxSize);
            }
            const BoundingBox aabb = meshInstance->aabb();
            const Vector3 half = aabb.halfExtents();
            const float totalArea = std::sqrt(
                half.getY() * half.getZ() + half.getX() * half.getZ() + half.getX() * half.getY());
            return std::clamp(nextPowerOfTwo(static_cast<int>(totalArea * options.sizeMultiplier)),
                8, std::clamp(options.maxResolution, 8, maxSize));
        }
    }

    GpuLightmapper::GpuLightmapper(Engine* engine) : _engine(engine)
    {
    }

    GpuLightmapper::~GpuLightmapper()
    {
        destroyBakeNodes();
    }

    namespace
    {
        // HDR: the bake stores LINEAR light, and the sun alone exceeds 1.0 before
        // exposure — an 8-bit target would clamp it to white and flatten the scene.
        // The accumulating virtual-light passes need the headroom too.
        std::shared_ptr<Texture> createLightmapTexture(GraphicsDevice* device, const int size)
        {
            TextureOptions texOptions;
            texOptions.width = static_cast<uint32_t>(size);
            texOptions.height = static_cast<uint32_t>(size);
            texOptions.format = PixelFormat::PIXELFORMAT_RGBA16F;
            texOptions.mipmaps = false;
            texOptions.name = "gpuLightmap";
            texOptions.profilerHint = TexHint::TEXHINT_LIGHTMAP;
            return std::make_shared<Texture>(device, texOptions);
        }

        std::shared_ptr<RenderTarget> createLightmapTarget(GraphicsDevice& device, Texture* texture)
        {
            RenderTargetOptions rtOptions;
            rtOptions.graphicsDevice = &device;
            rtOptions.colorBuffer = texture;
            rtOptions.depth = true;
            rtOptions.name = "gpuLightmapTarget";
            return device.createRenderTarget(rtOptions);
        }
    }

    void GpuLightmapper::bake(const std::vector<MeshInstance*>& targets, const Options& options)
    {
        if (!_engine || targets.empty()) {
            return;
        }
        destroyBakeNodes();

        _options = options;
        _targets = targets;
        _lightmaps.clear();
        _originalMasks.clear();

        for (size_t i = 0; i < _targets.size(); ++i) {
            prepareTarget(i);
        }
        widenLightsForBake();
        prepareDirectionalSamples();

        // Ambient occlusion: the scene's own ambient is suppressed for the bake and
        // re-introduced as virtual lights, so the ambient term carries visibility
        // instead of being flat. Sample -1 is the direct-light pass.
        _ambientSample = -1;
        _ambientNormalization = 0.0f;
        if (_options.ambientBake && _options.ambientBakeNumSamples > 0) {
            _savedAmbient = _engine->scene()->ambientLight();
            setupAmbientLight();
        }

        _pending = true;
        _framesWaited = 0;
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
        auto texture = createLightmapTexture(device.get(), size);
        auto renderTarget = createLightmapTarget(*device, texture.get());

        // A private layer per target: the bake camera renders exactly one mesh, so
        // its unwrap owns the whole target.
        auto layer = std::make_shared<Layer>("LightmapBake" + std::to_string(index),
            _options.baseLayerId + static_cast<int>(index));
        layer->addMeshInstances({meshInstance});
        _engine->scene()->layers()->pushOpaque(layer);

        // Upstream's mask scheme: the bake lights carry MASK_BAKE, so the mesh wears
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
        camera->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
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
            std::vector<int> layerIds = lightComponent->layers();
            layerIds.insert(layerIds.end(), bakeLayers.begin(), bakeLayers.end());
            lightComponent->setLayers(layerIds);

            // A light on MASK_BAKE reports castShadows() == false by design
            // (Light::castShadows excludes MASK_BAKE and MASK_NONE, mirroring upstream),
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
        _dirSample = -1;
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
            lightComponent->setEnabled(false);
        }
        if (!_directionalLights.empty()) {
            _dirSampleCount = _options.directionalBakeNumSamples;
        }
    }

    bool GpuLightmapper::update()
    {
        if (!_pending) {
            return false;
        }
        // One rendered frame is enough — the bake cameras drew their targets during it.
        if (++_framesWaited < 1) {
            return false;
        }

        // Soft directional shadows: one accumulated frame per virtual copy of the sun.
        if (_dirSample + 1 < _dirSampleCount) {
            ++_dirSample;
            prepareDirectionalSample(_dirSample);
            beginAccumulation();
            return false;
        }

        // Ambient occlusion: after the direct-light frame, run one more frame per virtual
        // light, each blended additively into the same lightmaps.
        if (_options.ambientBake && _ambientLight &&
            _ambientSample + 1 < _options.ambientBakeNumSamples) {
            ++_ambientSample;
            prepareAmbientSample(_ambientSample);

            beginAccumulation();
            return false;   // more frames to go
        }

        for (size_t i = 0; i < _targets.size(); ++i) {
            auto* meshInstance = _targets[i];
            if (!meshInstance || i >= _lightmaps.size() || !_lightmaps[i]) {
                continue;
            }
            meshInstance->setMask(MASK_AFFECT_LIGHTMAPPED);
            // The bake belongs to the MESH INSTANCE (upstream 0cd268478): meshes that
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

    void GpuLightmapper::setupAmbientLight()
    {
        _ambientLightEntity = new Entity();
        _ambientLightEntity->setName("LightmapAmbientBakeLight");
        _ambientLightEntity->setEngine(_engine);
        _engine->root()->addChild(_ambientLightEntity);

        _ambientLight = static_cast<LightComponent*>(
            _ambientLightEntity->addComponent<LightComponent>());
        _ambientLight->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        _ambientLight->setCastShadows(true);
        _ambientLight->setShadowBias(0.2f);
        _ambientLight->setShadowDistance(_options.bakeCameraDistance * 4.0f);
        _ambientLight->setMask(MASK_AFFECT_LIGHTMAPPED);
        _ambientLight->setEnabled(false);   // enabled once the direct pass is done

        _ambientLight->setLayers(bakeLayerIds());
    }

    void GpuLightmapper::beginAccumulation()
    {
        if (_accumulating) {
            return;
        }
        _accumulating = true;

        // Ambient is dropped by the shader for these passes (VT_FEATURE_LIGHTMAP_BAKE_ACCUM)
        // rather than by touching the scene — nulling Scene::envAtlas mid-bake stalls the
        // renderer, and the shader gate is both cheaper and reversible.
        // From here on the bake cameras add to the lightmap instead of clearing it. The
        // composition caches its render actions (and the resolved clear flags with them),
        // and camera clear state is not part of its dirty fingerprint — so this has to
        // mark it dirty or every pass would simply replace the last.
        for (auto* cameraEntity : _cameras) {
            if (!cameraEntity) {
                continue;
            }
            if (auto* cameraComponent = cameraEntity->findComponent<CameraComponent>()) {
                if (Camera* camera = cameraComponent->camera()) {
                    camera->setClearColorBuffer(false);
                    camera->setLightmapBakeAccumulate(true);
                }
            }
        }
        if (const auto& layers = _engine->scene()->layers()) {
            layers->markDirty();
        }
    }

    void GpuLightmapper::prepareDirectionalSample(const int index)
    {
        // Only the directional lights contribute to these passes — the local lights were
        // already baked in the direct frame.
        for (auto& [lightComponent, mask] : _lightMaskBackup) {
            if (lightComponent && lightComponent->type() != LightType::LIGHTTYPE_DIRECTIONAL) {
                lightComponent->setEnabled(false);
            }
        }

        for (auto& [lightComponent, rotation, intensity, luminance] : _directionalLights) {
            if (!lightComponent) {
                continue;
            }
            lightComponent->setEnabled(true);
            auto* node = lightComponent->entity();
            if (!node) {
                continue;
            }
            // Upstream BakeLightSimple: sample 0 keeps the authored direction, the rest are
            // rotated by a disc point scaled to half the bake area.
            node->setLocalRotation(rotation);
            if (index > 0) {
                float dx = 0.0f, dy = 0.0f;
                circlePointDeterministic(dx, dy, index, _dirSampleCount);
                const float half = _options.directionalBakeArea * 0.5f;
                node->rotateLocal(dx * half, 0.0f, dy * half);
            }
            // The lightmap accumulates in linear space, so the copies simply split the
            // authored intensity. (Upstream's pow-based split compensates for its own
            // gamma-space accumulation; doing that here would multiply the sun by N^0.55.)
            const float share = 1.0f / static_cast<float>(std::max(_dirSampleCount, 1));
            lightComponent->setIntensity(intensity * share);
            lightComponent->setLuminance(luminance * share);
        }
    }

    void GpuLightmapper::prepareAmbientSample(const int index)
    {
        if (!_ambientLight || !_ambientLightEntity) {
            return;
        }
        // Silence the scene's own lights for the ambient passes — their contribution is
        // already in the lightmap from the direct-light frame.
        for (auto& [lightComponent, mask] : _lightMaskBackup) {
            if (lightComponent && lightComponent != _ambientLight) {
                lightComponent->setEnabled(false);
            }
        }
        _ambientLight->setEnabled(true);

        // Upstream BakeLightAmbient: a point on the sphere part, the light aimed back
        // along it (lookAt(-point) then rotateLocal(90,0,0), since a light emits along
        // its node's -Y while lookAt aims -Z).
        const Vector3 point = spherePointDeterministic(index,
            _options.ambientBakeNumSamples, std::clamp(_options.ambientBakeSpherePart, 0.01f, 1.0f));
        _ambientLightEntity->setLocalPosition(0.0f, 0.0f, 0.0f);
        _ambientLightEntity->lookAt(point * -1.0f);
        _ambientLightEntity->rotateLocal(90.0f, 0.0f, 0.0f);

        // Intensity normalization. Upstream can divide by the sample count because its
        // bake accumulates VISIBILITY and multiplies by the ambient colour at the end
        // (bakeLmEnd); here each virtual light contributes radiance directly, so the set
        // has to be normalized such that a fully open, upward-facing surface receives
        // exactly the ambient colour it replaces: sum of N·L over the sample directions
        // for N = +Y, inverted.
        if (_ambientNormalization <= 0.0f) {
            float sum = 0.0f;
            for (int i = 0; i < _options.ambientBakeNumSamples; ++i) {
                const Vector3 dir = spherePointDeterministic(i, _options.ambientBakeNumSamples,
                    std::clamp(_options.ambientBakeSpherePart, 0.01f, 1.0f));
                sum += std::max(dir.getY(), 0.0f);
            }
            _ambientNormalization = (sum > 1e-4f) ? (1.0f / sum) : 1.0f;
        }
        _ambientLight->setIntensity(_ambientNormalization);
        _ambientLight->setColor(_savedAmbient);
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
        _accumulating = false;

        // Re-enable any scene lights the ambient passes silenced.
        for (auto& [lightComponent, mask] : _lightMaskBackup) {
            if (lightComponent && lightComponent != _ambientLight) {
                lightComponent->setEnabled(true);
            }
        }
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
    }
}
