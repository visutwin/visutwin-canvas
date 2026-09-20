// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// SSAO showcase (mirrors upstream graphics/ambient-occlusion): the laboratory
// interior lit by the helipad env atlas, torch omni lights and a shadow-casting
// directional light, with screen-space ambient occlusion applied through the
// camera frame. Every SSAO parameter is bound to a key so its effect can be
// isolated at runtime.
//
#include <algorithm>
#include <cmath>
#include <numbers>
#include <memory>
#include <cstdlib>
#include <map>
#include <vector>
#include <algorithm>
#include "platform/graphics/gpuProfiler.h"

#include <core/shape/boundingBox.h>
#include <framework/assets/asset.h>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "platform/graphics/depthState.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassConstants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class AmbientOcclusionExample final: public ExampleApp
{
public:
    AmbientOcclusionExample(): ExampleApp({.title = "Ambient Occlusion Example"}) {}

protected:
    bool create() override
    {
        spdlog::info("*** Ambient Occlusion Example Started ***");

        // setup skydome
        scene()->setSkyboxMip(2);
        scene()->setExposure(2.5f);
        scene()->setToneMapping(TONEMAP_NEUTRAL);

        // Assets matching upstream AO example
        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _laboratory = std::make_unique<Asset>(
            "laboratory", AssetType::CONTAINER, assetPath("models/laboratory.glb"));

        const auto envAtlasResource = _envAtlas->resource();
        if (!envAtlasResource) {
            spdlog::error("Failed to load environment atlas texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));

        // create laboratory entity (scale 100)
        const auto labResource = _laboratory->resource();
        if (!labResource) {
            spdlog::error("Failed to load laboratory model");
            return false;
        }
        auto* labEntity = std::get<ContainerResource*>(*labResource)->instantiateRenderEntity();
        labEntity->setLocalScale(100, 100, 100);
        root()->addChild(labEntity);

        // set up materials — enable shadows, disable baked AO, disable blending
        for (auto* render : RenderComponent::instances()) {
            if (!render || !render->entity()) continue;
            auto* owner = render->entity();
            if (owner != labEntity && !owner->isDescendantOf(labEntity)) continue;

            render->setCastShadows(true);
            render->setReceiveShadows(true);

            for (auto* mi : render->meshInstances()) {
                if (!mi || !mi->material()) continue;
                auto* mat = dynamic_cast<StandardMaterial*>(mi->material());
                if (!mat) continue;
                // disable baked AO map — we want SSAO only
                mat->setAoMap(nullptr);
                // disable blending / enable depth writes
                mat->setTransparent(false);
                mat->setDepthState(std::make_shared<DepthState>());
            }
        }

        // add lights to the torches
        {
            auto torches = labEntity->find([](GraphNode* node) {
                return node->name().find("Fackel") != std::string::npos;
            });
            spdlog::info("Found {} torch nodes", torches.size());
            for (auto* torch : torches) {
                auto* torchLight = new Entity();
                torchLight->setEngine(engine());
                if (auto* torchLightComp = static_cast<LightComponent*>(torchLight->addComponent<LightComponent>())) {
                    torchLightComp->setType(LightType::LIGHTTYPE_OMNI);
                    torchLightComp->setColor(Color(1.0f, 0.75f, 0.0f));
                    torchLightComp->setIntensity(3.0f);
                    torchLightComp->setRange(100.0f);
                    torchLightComp->setCastShadows(true);
                    torchLightComp->setShadowBias(0.2f);
                    torchLightComp->setShadowNormalBias(0.2f);
                    // Static torches in a static room: render each cubemap once. Measured
                    // on Metal, this and the directional light's mode below take the
                    // frame from 852 draw calls to about 124, which is upstream's figure.
                    torchLightComp->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_THISFRAME);
                }
                // Position at the torch's first child mesh center
                if (!torch->children().empty()) {
                    // GraphNode owns its children by unique_ptr in this engine.
                    auto* firstChild = torch->children()[0].get();
                    auto* childRender = static_cast<Entity*>(firstChild)->findComponent<RenderComponent>();
                    if (childRender && !childRender->meshInstances().empty()) {
                        torchLight->setLocalPosition(childRender->meshInstances()[0]->aabb().center());
                    } else {
                        torchLight->setLocalPosition(static_cast<Entity*>(torch)->position());
                    }
                } else {
                    torchLight->setLocalPosition(static_cast<Entity*>(torch)->position());
                }
                root()->addChild(torchLight);
            }
        }

        // add a ground plane
        _planeMaterial = std::make_shared<StandardMaterial>();
        _planeMaterial->setDiffuse(Color(0.2f, 0.2f, 0.2f));
        createPrimitive("plane", _planeMaterial.get(), Vector3(0.0f, -40.0f, 0.0f),
            Vector3(400.0f, 1.0f, 400.0f));

        // add shadow casting directional light
        auto* light = createDirectionalLight(Vector3(35.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowResolution(4096);
            lightComp->setShadowDistance(600.0f);
            lightComp->setShadowBias(0.4f);
            lightComp->setShadowNormalBias(0.06f);
            lightComp->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_THISFRAME);
        }

        // create camera entity — nearClip=1, farClip=600
        auto* camera = createCamera(Vector3(-60.0f, 30.0f, 60.0f));
        _cameraComp = camera->findComponent<CameraComponent>();

        if (_cameraComp && _cameraComp->camera()) {
            _cameraComp->camera()->setNearClip(1.0f);
            _cameraComp->camera()->setFarClip(600.0f);
        }

        // enable SSAO
        if (_cameraComp) {
            auto ssao = _cameraComp->ssao();
            ssao.enabled = true;
            // Upstream's example opens on SSAOTYPE_LIGHTING: the occlusion is folded
            // into the ambient term as the scene shades, rather than multiplied over
            // the finished image by compose. The two look different — lighting mode
            // cannot darken what was never lit by ambient light, so an emissive or
            // fully lit surface keeps its brightness — and only lighting mode needs
            // the depth prepass, which is why the type is worth switching at runtime.
            ssao.type = SSAOTYPE_LIGHTING;
            ssao.blurEnabled = true;
            ssao.radius = 30.0f;
            ssao.samples = 12;
            ssao.intensity = 0.4f;
            ssao.power = 6.0f;
            ssao.minAngle = 10.0f;
            ssao.scale = 1.0f;
            ssao.randomize = false;
            _cameraComp->setSsao(ssao);

            // tone mapping, and upstream's 4x MSAA on the scene target (it disables
            // MSAA only when TAA is on, which this port does not offer). The camera
            // frame's target is already RGBA16F, upstream's `renderFormats` choice.
            auto rendering = _cameraComp->rendering();
            rendering.toneMapping = TONEMAP_NEUTRAL;
            rendering.samples = 4;
            _cameraComp->setRendering(rendering);
        }

        // Setup orbit camera controls focused on laboratory
        const auto labBbox = entityBounds(labEntity);
        _focusPoint = labBbox.center();
        const float sceneRadius = std::max(labBbox.halfExtents().length(), 1.0f);
        // Upstream's orbit script frames the focus entity at
        // 1.5 * (largest half extent) / sin(fov / 2), clamped to its distanceMax of 350,
        // which is where the clamp lands for this model.
        const auto& he = labBbox.halfExtents();
        const float frameRadius = std::max({he.getX(), he.getY(), he.getZ()});
        const float fovDeg = (_cameraComp && _cameraComp->camera()) ? _cameraComp->camera()->fov() : 45.0f;
        const float fovRad = fovDeg * (std::numbers::pi_v<float> / 180.0f);
        _orbitDistance = std::min(frameRadius * 1.5f / std::sin(0.5f * fovRad), 350.0f);

        _controls = addOrbitControls(camera, _focusPoint);
        _controls->setMoveSpeed(2 * sceneRadius);
        _controls->setMoveFastSpeed(4 * sceneRadius);
        _controls->setMoveSlowSpeed(sceneRadius);
        _controls->setOrbitDistance(_orbitDistance);
        _controls->storeResetState();

        spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
        spdlog::info("SSAO controls: O toggle SSAO, T lighting/combine, B toggle blur, Z toggle randomize");
        spdlog::info("  +/- adjust intensity, [/] adjust radius, ,/. adjust samples, ;/' adjust power");
        logSsaoState("init");

        return true;
    }

    void update(float) override
    {
        ++_probeFrame;
        if (_probeFrame == 20) if (const auto& gd = engine()->graphicsDevice()) if (const auto& prof = gd->gpuProfiler()) prof->setEnabled(true);
        if (_probeFrame < 60 || _probeFrame > 360) return;
        const auto& gd = engine()->graphicsDevice(); const auto& prof = gd ? gd->gpuProfiler() : nullptr; if (!prof) return;
        size_t i = 0; for (const auto& pt : prof->passTimings()) { const std::string key = std::to_string(i++) + " " + pt.name; if (!_t.count(key)) _order.push_back(key); _t[key].push_back(pt.milliseconds); }
        _frameMs.push_back(prof->frameMilliseconds());
        if (_probeFrame == 360) {
            auto med = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0.0 : v[v.size() / 2]; };
            std::string line = "PROBE ours frame " + std::to_string(med(_frameMs)).substr(0, 5);
            for (const auto& k : _order) line += " | " + k + " " + std::to_string(med(_t[k])).substr(0, 5);
            spdlog::info("{}", line);
        }
    }
    int _probeFrame = 0; std::map<std::string, std::vector<double>> _t; std::vector<std::string> _order; std::vector<double> _frameMs;

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }

        auto ssao = _cameraComp->ssao();
        switch (event.key.key) {
        case SDLK_O:
            ssao.enabled = !ssao.enabled;
            _cameraComp->setSsao(ssao);
            logSsaoState("toggle");
            return true;
        case SDLK_B:
            ssao.blurEnabled = !ssao.blurEnabled;
            _cameraComp->setSsao(ssao);
            logSsaoState("blur");
            return true;
        case SDLK_Z:
            ssao.randomize = !ssao.randomize;
            _cameraComp->setSsao(ssao);
            logSsaoState("randomize");
            return true;
        case SDLK_T:
            ssao.type = (ssao.type == SSAOTYPE_LIGHTING) ? SSAOTYPE_COMBINE : SSAOTYPE_LIGHTING;
            _cameraComp->setSsao(ssao);
            logSsaoState("type");
            return true;
        case SDLK_EQUALS:
            ssao.intensity = std::min(1.0f, ssao.intensity + 0.05f);
            _cameraComp->setSsao(ssao);
            logSsaoState("intensity+");
            return true;
        case SDLK_MINUS:
            ssao.intensity = std::max(0.0f, ssao.intensity - 0.05f);
            _cameraComp->setSsao(ssao);
            logSsaoState("intensity-");
            return true;
        case SDLK_RIGHTBRACKET:
            ssao.radius = std::min(100.0f, ssao.radius + 5.0f);
            _cameraComp->setSsao(ssao);
            logSsaoState("radius+");
            return true;
        case SDLK_LEFTBRACKET:
            ssao.radius = std::max(1.0f, ssao.radius - 5.0f);
            _cameraComp->setSsao(ssao);
            logSsaoState("radius-");
            return true;
        case SDLK_PERIOD:
            ssao.samples = std::min(32, ssao.samples + 2);
            _cameraComp->setSsao(ssao);
            logSsaoState("samples+");
            return true;
        case SDLK_COMMA:
            ssao.samples = std::max(2, ssao.samples - 2);
            _cameraComp->setSsao(ssao);
            logSsaoState("samples-");
            return true;
        case SDLK_APOSTROPHE:
            ssao.power = std::min(16.0f, ssao.power + 1.0f);
            _cameraComp->setSsao(ssao);
            logSsaoState("power+");
            return true;
        case SDLK_SEMICOLON:
            ssao.power = std::max(0.5f, ssao.power - 1.0f);
            _cameraComp->setSsao(ssao);
            logSsaoState("power-");
            return true;
        case SDLK_F:
            if (_controls) {
                _controls->focus(_focusPoint, _orbitDistance);
            }
            return true;
        default:
            return false;
        }
    }

    void destroy() override
    {
        spdlog::info("*** Ambient Occlusion Example Finished ***");
    }

private:
    void logSsaoState(const char* reason) const
    {
        if (!_cameraComp) {
            return;
        }
        const auto& ssao = _cameraComp->ssao();
        spdlog::info("SSAO {}: enabled={}, type={}, blur={}, intensity={:.2f}, power={:.1f}, radius={:.1f}, samples={}, minAngle={:.1f}, scale={:.2f}, randomize={}",
            reason,
            ssao.enabled ? "ON" : "OFF",
            std::string(ssao.type),
            ssao.blurEnabled ? "ON" : "OFF",
            ssao.intensity,
            ssao.power,
            ssao.radius,
            ssao.samples,
            ssao.minAngle,
            ssao.scale,
            ssao.randomize ? "ON" : "OFF");
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _laboratory;
    std::shared_ptr<StandardMaterial> _planeMaterial;

    CameraComponent* _cameraComp = nullptr;
    CameraControls* _controls = nullptr;
    Vector3 _focusPoint;
    float _orbitDistance = 200.0f;
};

VISUTWIN_EXAMPLE_MAIN(AmbientOcclusionExample)
