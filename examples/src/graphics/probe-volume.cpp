// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// Port of upstream graphics/probe-volume.
//
// A concrete pavilion lit by a probe volume, with three lighting presets loaded from a baked file
// (concrete-pavilion-probes.bin, upstream's bytes). Apart from the sun there are no lights: the
// glowing objects are emissive meshes (their emission scaled by 0.25), which light the scene only
// through the probes. The pavilion's meshes are batched by material on the world layer; four
// bitmoji characters (made non-metallic, as their glTF leaves metalness at 1) walk around the
// plinth on a layer of their own, lit by the probes and not captured by them. The dynamic sky of
// the camera frame follows the preset (sun elevation and rotation, sun intensity, luminance) and
// drives the sun light (PCF3, 2048, bias 0.2, normal offset 0.05, shadow bounds the pavilion's
// box); the volume's own intensity and the bloom are per preset too. Neutral tone mapping, the
// camera (fov 60, near 0.05, far 2000) at (-14.35, 9.04, 27.3) looking 25 units along
// (0.453, -0.239, -0.859). The probes' blend distance is 0.2. The sunset preset shows at start.
//
// Keys stand in for upstream's controls panel:
//   1 / 2 / 3  the preset: day, sunset, night
//   P          the probes on / off
//   H          the SH sharpening on / off
//   L          the leak reduction on / off
//   V          the visibility on / off
//   O          the reflection occlusion on / off
//   F          the probe format, RGBA16F / RGBA8
//   - / =      the normal bias -/+ 0.1
//   G          show the probes: each sphere shows its probe as stored, invalid probes red, a line
//              from each moved probe to where it was captured
//   C          the characters on / off
//   B          bake the lighting of the preset again from the captured geometry (capturing it
//              first if it was not), 2 bounces
//   K          capture the geometry again and bake every preset
//   S          the source of a bake, the captured geometry / the scene rendered from each probe
//   D          save the probes of every preset (upstream's Download) to concrete-pavilion-probes.bin
//              in the working directory
// VISUTWIN_PROBE_PRESET=n starts at preset n, VISUTWIN_PROBE_BAKE=1 bakes every preset at start
// instead of showing the loaded ones (as upstream does when the file cannot be loaded), with
// VISUTWIN_PROBE_SOURCE=scene from the scene rendered from each probe, VISUTWIN_PROBE_SAVE=<file>
// saves the probes when a bake ends, VISUTWIN_PROBES=0 starts with the probes off and
// VISUTWIN_PROBE_SHOW=1 with the probes shown.
//
// DEVIATIONS:
// - the volume's box gizmo, its probe preview, the probe spacing, face size and the other capture
//   settings of upstream's Baking panel are not ported; the box and the spacing are the file's.
// - the camera is the examples' orbit controls around upstream's focus point.
//
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "exampleApp.h"
#include "extras/probeVolume/probeVolume.h"
#include "extras/probeVolume/probeVolumeBaker.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/batching/batchGroup.h"
#include "framework/batching/batchManager.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/parsers/glbContainerResource.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/graphics/dynamicSkyEffect.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // The layer of the characters and the other dynamic objects, which the probes do not capture.
    constexpr int LAYERID_DYNAMIC = 100;

    constexpr int kPavilionBatchGroup = 1;

    // The emissive lights of the pavilion, dimmed so that the tone mapping keeps their colour
    // instead of showing them almost white. The probes captured them at this brightness.
    constexpr float kEmissiveScale = 0.25f;

    constexpr float kWalkSpeed = 1.0f;

    // A lighting preset, baked into a lighting of the volume: the dynamic sky's settings it is
    // baked with, and the probes' intensity and the bloom, which are not baked.
    struct Preset
    {
        float elevation;
        float rotation;
        float sunIntensity;
        float luminance;
        float intensity;
        float bloom;
    };

    std::shared_ptr<AnimTrack> firstAnimTrack(Asset* asset)
    {
        auto* glb = asset ? dynamic_cast<GlbContainerResource*>(asset->resourceAs<ContainerResource>()) : nullptr;
        if (!glb || glb->animTracks().empty()) {
            return nullptr;
        }
        return glb->animTracks().begin()->second;
    }

    std::vector<uint8_t> readFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
    }
}

class ProbeVolumeExample final: public ExampleApp
{
public:
    ProbeVolumeExample(): ExampleApp({}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        _pavilionAsset = std::make_unique<Asset>("concrete-pavilion", AssetType::CONTAINER,
            assetPath("models/concrete-pavilion.glb"));
        _bitmojiAsset = std::make_unique<Asset>("bitmoji", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _walkAsset = std::make_unique<Asset>("walk", AssetType::CONTAINER, assetPath("animations/bitmoji/walk.glb"));
        auto* pavilionContainer = _pavilionAsset->resourceAs<ContainerResource>();
        auto* bitmojiContainer = _bitmojiAsset->resourceAs<ContainerResource>();
        const auto walk = firstAnimTrack(_walkAsset.get());
        if (!pavilionContainer || !bitmojiContainer || !walk) {
            spdlog::error("Failed to load the pavilion, the character or its walk");
            return false;
        }

        if (const char* preset = std::getenv("VISUTWIN_PROBE_PRESET")) {
            _preset = std::clamp(std::atoi(preset), 0, 2);
        }
        if (const char* probes = std::getenv("VISUTWIN_PROBES"); probes && std::atoi(probes) == 0) {
            _probesEnabled = false;
        }
        if (const char* source = std::getenv("VISUTWIN_PROBE_SOURCE"); source && std::strcmp(source, "scene") == 0) {
            _source = ProbeSource::Scene;
        }
        if (const char* save = std::getenv("VISUTWIN_PROBE_SAVE")) {
            _savePath = save;
        }

        // The characters and the other dynamic objects on a layer of their own, between the
        // world's opaque and transparent sublayers.
        const auto layers = scene()->layers();
        const auto worldLayer = layers->getLayerById(LAYERID_WORLD);
        auto dynamicLayer = std::make_shared<Layer>("Dynamic", LAYERID_DYNAMIC);
        layers->insert(dynamicLayer, layers->getTransparentIndex(worldLayer));

        // The sun, whose direction, colour and intensity the dynamic sky sets.
        _sun = createDirectionalLight(Vector3(0.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        _sun->setName("Sun");
        auto* sunLight = _sun->findComponent<LightComponent>();
        sunLight->setShadowType(SHADOW_PCF3_32F);
        sunLight->setShadowResolution(2048);
        sunLight->setShadowBias(0.2f);
        sunLight->setShadowNormalBias(0.05f);
        sunLight->setLayers({LAYERID_WORLD, LAYERID_DYNAMIC});

        // The pavilion, its meshes batched by material, so that the probes capture it with few
        // draw calls.
        engine()->batcher()->addGroup(BatchGroup(kPavilionBatchGroup, "Pavilion", false, 100.0f, {LAYERID_WORLD}));
        Entity* pavilion = pavilionContainer->instantiateRenderEntity();
        std::set<StandardMaterial*> emissive;
        BoundingBox pavilionBox;
        bool boxEmpty = true;
        for (auto* render : pavilion->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
            render->setReceiveShadows(true);
            for (auto* meshInstance : render->meshInstances()) {
                auto* material = dynamic_cast<StandardMaterial*>(meshInstance->material());
                if (material && material->emissiveIntensity() > 1.0f) {
                    emissive.insert(material);
                }
                if (material) {
                    _probeLitMaterials.insert(material);
                }
            }
        }
        for (StandardMaterial* material : emissive) {
            material->setEmissiveIntensity(material->emissiveIntensity() * kEmissiveScale);
        }
        root()->addChild(pavilion);

        // The box of the volume: the pavilion, its planters and trees, a metre around and above
        // them, so that their outside faces have probes in front of them, but not the ground.
        for (auto* render : pavilion->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                if (render->entity()->name() != "Ground") {
                    if (boxEmpty) {
                        pavilionBox = meshInstance->aabb();
                        boxEmpty = false;
                    } else {
                        pavilionBox.add(meshInstance->aabb());
                    }
                }
            }
            render->setBatchGroupId(kPavilionBatchGroup);
        }
        const Vector3 boxMin = pavilionBox.center() - pavilionBox.halfExtents() - Vector3(1.0f, 0.0f, 1.0f);
        const Vector3 boxMax = pavilionBox.center() + pavilionBox.halfExtents() + Vector3(1.0f, 1.0f, 1.0f);
        _volumeBox = BoundingBox((boxMin + boxMax) * 0.5f, (boxMax - boxMin) * 0.5f);
        engine()->batcher()->generate(scene().get(), {kPavilionBatchGroup});

        // The sun's shadow covers the pavilion for any camera, so it does not fade with distance.
        sunLight->setShadowBounds(_volumeBox);

        // The camera.
        const Vector3 cameraPosition(-14.35f, 9.04f, 27.3f);
        Entity* camera = createCamera(cameraPosition);
        _cameraComp = camera->findComponent<CameraComponent>();
        _cameraComp->setLayers({LAYERID_WORLD, LAYERID_DYNAMIC, LAYERID_SKYBOX, LAYERID_IMMEDIATE, LAYERID_UI});
        _cameraComp->camera()->setFov(60.0f);
        _cameraComp->camera()->setNearClip(0.05f);
        _cameraComp->camera()->setFarClip(2000.0f);
        if (auto* controls = addOrbitControls(camera, Vector3(0.453f, -0.239f, -0.859f) * 25.0f + cameraPosition)) {
            controls->storeResetState();
        }

        // The dynamic sky, rendered by the camera frame: it keeps the sun light in step with the
        // sun and the moon, and generates the environment's lighting, which lights the scene
        // outside the volume and which the probes see when they are baked.
        _cameraComp->setToneMapping(TONEMAP_NEUTRAL);
        _sky = std::make_shared<DynamicSkyEffect>();
        _sky->setLight(sunLight);
        _sky->moonIntensity = 0.4f;
        _cameraComp->addFrameEffect(_sky);

        // The characters, walking along the path around the plinth, spread out along it.
        _path = {Vector3(-11.8f, 0.0f, -9.8f), Vector3(9.5f, 0.0f, -9.8f), Vector3(9.5f, 0.0f, 13.9f),
            Vector3(-11.8f, 0.0f, 13.9f), Vector3(-11.8f, 0.0f, -9.8f)};
        _pathDistances = {0.0f};
        for (size_t i = 1; i < _path.size(); ++i) {
            _pathDistances.push_back(_pathDistances.back() + (_path[i] - _path[i - 1]).length());
        }
        const float pathLength = _pathDistances.back();
        _dynamic = new Entity();
        _dynamic->setName("Dynamic");
        _dynamic->setEngine(engine());
        root()->addChild(_dynamic);
        for (int index = 0; index < 4; ++index) {
            Entity* entity = bitmojiContainer->instantiateRenderEntity();
            entity->setEngine(engine());
            for (auto* render : entity->findComponents<RenderComponent>()) {
                render->setCastShadows(true);
                render->setLayers({LAYERID_DYNAMIC});
                for (auto* meshInstance : render->meshInstances()) {
                    if (auto* material = dynamic_cast<StandardMaterial*>(meshInstance->material())) {
                        material->setMetalness(0.0f);
                        _probeLitMaterials.insert(material);
                    }
                }
            }
            auto* anim = static_cast<AnimComponent*>(entity->addComponent<AnimComponent>());
            anim->setActivate(true);
            anim->assignAnimation("Walk", walk);
            _dynamic->addChild(entity);
            _characters.push_back({entity, static_cast<float>(index) * pathLength / 4.0f});
        }

        // The probe spheres: each shows its probe as stored, the invalid probes red.
        _probeMaterial = std::make_shared<StandardMaterial>();
        _probeMaterial->setUseLighting(false);
        _probeMaterial->setUseSkybox(false);
        _probeMaterial->setUseMetalness(true);
        _probeMaterial->setMetalness(0.0f);
        _probeMaterial->setGloss(0.0f);
        _probeMaterial->setProbeVolumeDebug(true);
        _invalidProbeMaterial = std::make_shared<StandardMaterial>();
        _invalidProbeMaterial->setUseLighting(false);
        _invalidProbeMaterial->setUseSkybox(false);
        _invalidProbeMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _invalidProbeMaterial->setEmissive(Color(1.0f, 0.1f, 0.1f, 1.0f));
        _probeSpheres = new Entity();
        _probeSpheres->setName("ProbeSpheres");
        _probeSpheres->setEngine(engine());
        root()->addChild(_probeSpheres);
        for (StandardMaterial* material : {_probeMaterial.get(), _invalidProbeMaterial.get()}) {
            Entity* sphere = createPrimitive("sphere", material, Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f),
                {LAYERID_DYNAMIC});
            sphere->findComponent<RenderComponent>()->setCastShadows(false);
            _probeSpheres->addChild(root()->removeChild(sphere));
            _probeSphereEntities.push_back(sphere);
        }
        _probeSpheres->setEnabled(false);

        applyLighting(kPresets[_preset]);

        // The probes baked ahead of time, showing the initial preset, or baked now.
        const char* bake = std::getenv("VISUTWIN_PROBE_BAKE");
        const std::vector<uint8_t> bytes = readFile(assetPath("models/concrete-pavilion-probes.bin"));
        std::unique_ptr<ProbeVolume> loaded = bake && std::atoi(bake) != 0 ? nullptr
            : ProbeVolume::deserialize(device().get(), bytes, _format);
        if (loaded) {
            setVolume(std::move(loaded));
            _volume->setActiveLighting(static_cast<uint32_t>(_preset));
            spdlog::info("Probes: {} probes, {} presets loaded", _volume->probeCount(), _volume->lightingCount());
        } else {
            setVolume(std::make_unique<ProbeVolume>(device().get(), _volumeBox, resolutionFor(_volumeBox), _format));
            _bakeQueue = {0, 1, 2};
            _captureQueued = true;
        }

        if (const char* show = std::getenv("VISUTWIN_PROBE_SHOW"); show && std::atoi(show) != 0) {
            _showProbes = true;
            showProbeSpheres(true);
        }

        spdlog::info("Controls: 1/2/3 preset, P probes, H sharpening, L leak reduction, V visibility, "
                     "O reflection occlusion, F format, -/= normal bias, G show probes, C characters, "
                     "B bake preset, K capture and bake, S bake source");
        return true;
    }

    void destroy() override
    {
        _baker.reset();
        _volume.reset();
        _sky.reset();
    }

    void update(const float dt) override
    {
        const float pathLength = _pathDistances.back();
        for (auto& character : _characters) {
            character.distance += dt * kWalkSpeed;
            const float d = std::fmod(character.distance, pathLength);
            size_t i = 0;
            while (i < _path.size() - 2 && _pathDistances[i + 1] < d) {
                ++i;
            }
            const float t = (d - _pathDistances[i]) / (_pathDistances[i + 1] - _pathDistances[i]);
            const Vector3 position = _path[i] + (_path[i + 1] - _path[i]) * t;
            const Vector3 direction = _path[i + 1] - _path[i];
            character.entity->setPosition(position);
            character.entity->setRotation(Quaternion::fromEulerAngles(0.0f,
                std::atan2(direction.getX(), direction.getZ()) * 180.0f / 3.14159265358979f, 0.0f));
        }

        if (_probeSpheres->enabled() && !_probeLines.empty()) {
            for (size_t i = 0; i + 1 < _probeLines.size(); i += 2) {
                scene()->drawLine(_probeLines[i], _probeLines[i + 1], Color(1.0f, 0.9f, 0.1f, 1.0f), false);
            }
        }

        stepBake();
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_volume) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_1:
        case SDLK_2:
        case SDLK_3:
            _preset = static_cast<int>(event.key.key - SDLK_1);
            if (!baking()) {
                applyLighting(kPresets[_preset]);
            }
            if (static_cast<uint32_t>(_preset) < _volume->lightingCount()) {
                _volume->setActiveLighting(static_cast<uint32_t>(_preset));
            }
            _volume->setIntensity(kPresets[_preset].intensity);
            break;
        case SDLK_P: _probesEnabled = !_probesEnabled; _volume->setEnabled(_probesEnabled); break;
        case SDLK_H: _volume->setShSharpening(!_volume->shSharpening()); break;
        case SDLK_L: _volume->setLeakReduction(!_volume->leakReduction()); break;
        case SDLK_V: _volume->setVisibilityEnabled(!_volume->visibility()); break;
        case SDLK_O: _volume->setReflectionOcclusion(!_volume->reflectionOcclusion()); break;
        case SDLK_F:
            _format = _format == PixelFormat::PIXELFORMAT_RGBA16F ? PixelFormat::PIXELFORMAT_RGBA8
                                                                 : PixelFormat::PIXELFORMAT_RGBA16F;
            _volume->setFormat(_format);
            break;
        case SDLK_MINUS: _volume->setNormalBias(std::max(0.0f, _volume->normalBias() - 0.1f)); break;
        case SDLK_EQUALS: _volume->setNormalBias(_volume->normalBias() + 0.1f); break;
        case SDLK_G:
            _showProbes = !_showProbes;
            showProbeSpheres(_showProbes && !baking());
            break;
        case SDLK_C: _charactersEnabled = !_charactersEnabled; _dynamic->setEnabled(_charactersEnabled && !baking()); break;
        case SDLK_B: queueBake(false); break;
        case SDLK_K: queueBake(true); break;
        case SDLK_S:
            _source = _source == ProbeSource::GBuffer ? ProbeSource::Scene : ProbeSource::GBuffer;
            break;
        case SDLK_D: save("concrete-pavilion-probes.bin"); break;
        default:
            return false;
        }
        spdlog::info("preset {}, probes {}, sharpening {}, leak reduction {}, visibility {}, reflection occlusion {}, "
                     "format {}, normal bias {:.1f}, bake source {}", _preset, _probesEnabled ? "on" : "off",
            _volume->shSharpening() ? "on" : "off", _volume->leakReduction() ? "on" : "off",
            _volume->visibility() ? "on" : "off", _volume->reflectionOcclusion() ? "on" : "off",
            _format == PixelFormat::PIXELFORMAT_RGBA8 ? "rgba8" : "rgba16f", _volume->normalBias(),
            _source == ProbeSource::GBuffer ? "geometry" : "scene");
        return true;
    }

private:
    struct Character
    {
        Entity* entity;
        float distance;
    };

    static constexpr Preset kPresets[3] = {
        {60.0f, 35.0f, 2.0f, 1.0f, 1.0f, 0.0f},
        {6.0f, 250.0f, 2.0f, 1.0f, 1.0f, 0.015f},
        {-20.0f, 120.0f, 2.0f, 1.0f, 3.0f, 0.1f},
    };

    // The resolution of a volume over a box at the probe spacing of 2.
    static std::array<uint32_t, 3> resolutionFor(const BoundingBox& box)
    {
        const Vector3& half = box.halfExtents();
        const auto count = [](const float extent) {
            return static_cast<uint32_t>(std::max(1.0f, std::round(extent * 2.0f / 2.0f)));
        };
        return {count(half.getX()), count(half.getY()), count(half.getZ())};
    }

    // The real-time lighting of a preset: the dynamic sky's settings, and its bloom.
    void applyLighting(const Preset& preset)
    {
        _sky->elevation = preset.elevation;
        _sky->rotation = preset.rotation;
        _sky->sunIntensity = preset.sunIntensity;
        _sky->luminance = preset.luminance;
        auto rendering = _cameraComp->rendering();
        rendering.bloomIntensity = preset.bloom;
        _cameraComp->setRendering(rendering);
    }

    // The volume shown, with the display settings, applied to the materials.
    void setVolume(std::unique_ptr<ProbeVolume> volume)
    {
        _baker.reset();
        _volume = std::move(volume);
        _volume->setEnabled(_probesEnabled);
        _volume->setIntensity(kPresets[_preset].intensity);
        _volume->setBlendDistance(0.2f);
        _probeSpheresStale = true;
        for (StandardMaterial* material : _probeLitMaterials) {
            ProbeVolume::applyToMaterial(*material);
        }
        ProbeVolume::applyToMaterial(*_probeMaterial);
    }

    void updateProbeSpheres()
    {
        const Vector3 cell = _volume->cellSize();
        const float size = std::min({cell.getX(), cell.getY(), cell.getZ()}) * 0.2f;
        std::vector<Matrix4> matrices[2];
        _probeLines.clear();
        const auto& offsets = _volume->offsets();
        for (uint32_t i = 0; i < _volume->probeCount(); ++i) {
            const Vector3 position = _volume->probePosition(i);
            matrices[_volume->validity()[i] ? 0 : 1].push_back(
                Matrix4::trs(position, Quaternion(0.0f, 0.0f, 0.0f, 1.0f), Vector3(size)));
            if (!offsets.empty() && (offsets[i * 3] != 0.0f || offsets[i * 3 + 1] != 0.0f || offsets[i * 3 + 2] != 0.0f)) {
                _probeLines.push_back(position);
                _probeLines.push_back(position + Vector3(offsets[i * 3], offsets[i * 3 + 1], offsets[i * 3 + 2]));
            }
        }
        for (int index = 0; index < 2; ++index) {
            auto* render = _probeSphereEntities[index]->findComponent<RenderComponent>();
            MeshInstance* meshInstance = render->meshInstances()[0];
            const int count = static_cast<int>(matrices[index].size());
            if (count > 0) {
                VertexBufferOptions options;
                options.data.resize(matrices[index].size() * sizeof(Matrix4));
                std::memcpy(options.data.data(), matrices[index].data(), options.data.size());
                auto buffer = device()->createVertexBuffer(VertexFormat::defaultInstancingFormat(), count, options);
                meshInstance->setInstancing(buffer, count);
            }
            _probeSphereEntities[index]->setEnabled(count > 0);
        }
    }

    void showProbeSpheres(const bool show)
    {
        if (show && _probeSpheresStale) {
            updateProbeSpheres();
            _probeSpheresStale = false;
        }
        _probeSpheres->setEnabled(show);
    }

    bool baking() const { return _baker && _baker->baking(); }

    // The probes of every preset, as loaded at start.
    void save(const std::string& path) const
    {
        const std::vector<uint8_t> bytes = _volume->serialize();
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        spdlog::info("Probes: saved {} bytes to {}", bytes.size(), path);
    }

    // A bake of the preset shown, or with `capture`, a capture of the geometry and a bake of
    // every preset; one requested during a bake runs after it.
    void queueBake(const bool capture)
    {
        if (capture) {
            _captureQueued = true;
            _bakeQueue = {0, 1, 2};
        } else if (std::find(_bakeQueue.begin(), _bakeQueue.end(), _preset) == _bakeQueue.end()) {
            _bakeQueue.push_back(_preset);
        }
    }

    // Advances the bakes queued, one step a frame: the baker works over several frames, and
    // the sky follows the preset baked for a few frames before its bake starts.
    void stepBake()
    {
        if (_baker && _baker->baking()) {
            return;
        }
        if (!_captureQueued && _bakeQueue.empty()) {
            if (_bakeActive) {
                // The preset selected now, which may have changed during the bake.
                _bakeActive = false;
                applyLighting(kPresets[_preset]);
                if (static_cast<uint32_t>(_preset) < _volume->lightingCount()) {
                    _volume->setActiveLighting(static_cast<uint32_t>(_preset));
                }
                _dynamic->setEnabled(_charactersEnabled);
                _probeSpheresStale = true;
                showProbeSpheres(_showProbes);
                spdlog::info("Probes: baked");
                if (!_savePath.empty()) {
                    save(_savePath);
                }
            }
            return;
        }
        if (!ProbeVolumeBaker::isSupported(device().get())) {
            spdlog::warn("Probes: this device cannot bake");
            _captureQueued = false;
            _bakeQueue.clear();
            return;
        }
        if (!_baker) {
            ProbeVolumeBakerOptions options;
            options.emissive = true;
            options.layers = {LAYERID_WORLD, LAYERID_SKYBOX};
            _baker = std::make_unique<ProbeVolumeBaker>(engine(), _volume.get(), options);
        }
        // The dynamic objects are not part of the baked lighting, so they are hidden, which also
        // keeps them from casting shadows into it.
        _bakeActive = true;
        _dynamic->setEnabled(false);
        _probeSpheres->setEnabled(false);

        if (_captureQueued || !_baker->hasGeometry()) {
            _captureQueued = false;
            ProbeVolumeCaptureOptions options;
            options.onComplete = [](const ProbeVolumeGeometryResult& result) {
                spdlog::info("Probes: captured {} probes, {} invalid, {} moved", result.probeCount,
                    result.invalidProbes, result.movedProbes);
            };
            _baker->captureGeometry(options);
            return;
        }

        // Each preset baked into its lighting under its real-time lighting, a few frames after
        // the sky has followed it.
        const int index = _bakeQueue.front();
        if (_settleFrames == 0) {
            applyLighting(kPresets[index]);
            _settleFrames = 4;
            return;
        }
        if (--_settleFrames > 0) {
            return;
        }
        _bakeQueue.erase(_bakeQueue.begin());
        ProbeVolumeBakeOptions options;
        options.source = _source;
        options.index = static_cast<uint32_t>(index);
        options.bounces = 2;
        _baker->bakeLighting(options);
    }

    std::unique_ptr<Asset> _pavilionAsset;
    std::unique_ptr<Asset> _bitmojiAsset;
    std::unique_ptr<Asset> _walkAsset;
    Entity* _sun = nullptr;
    CameraComponent* _cameraComp = nullptr;
    std::shared_ptr<DynamicSkyEffect> _sky;
    std::set<StandardMaterial*> _probeLitMaterials;
    BoundingBox _volumeBox;

    std::vector<Vector3> _path;
    std::vector<float> _pathDistances;
    Entity* _dynamic = nullptr;
    std::vector<Character> _characters;

    std::shared_ptr<StandardMaterial> _probeMaterial;
    std::shared_ptr<StandardMaterial> _invalidProbeMaterial;
    Entity* _probeSpheres = nullptr;
    std::vector<Entity*> _probeSphereEntities;
    std::vector<Vector3> _probeLines;
    bool _probeSpheresStale = true;

    std::unique_ptr<ProbeVolume> _volume;
    std::unique_ptr<ProbeVolumeBaker> _baker;
    PixelFormat _format = PixelFormat::PIXELFORMAT_RGBA16F;
    ProbeSource _source = ProbeSource::GBuffer;
    int _preset = 1;
    bool _probesEnabled = true;
    bool _charactersEnabled = true;
    bool _showProbes = false;
    bool _captureQueued = false;
    bool _bakeActive = false;
    std::vector<int> _bakeQueue;
    std::string _savePath;
    int _settleFrames = 0;
};

VISUTWIN_EXAMPLE_MAIN(ProbeVolumeExample)
