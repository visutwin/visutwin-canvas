// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Multi-view — port of upstream graphics/multi-view. One chess board rendered by
// three cameras into three viewports of the same back buffer:
//   * TOP (full width, upper half) — perspective, World layer, directional light.
//   * BOTTOM-LEFT                  — perspective, World layer, orbiting the board.
//   * BOTTOM-RIGHT                 — ORTHOGRAPHIC top-down, on a private
//                                    SpotLightLayer, so it sees only the yellow
//                                    spot light and not the directional one.
// The board belongs to both lighting layers, which is what lets the same geometry
// be lit differently per camera. Press D to step the debug shader pass applied to
// the top and right viewports (upstream exposes the same thing as a HUD dropdown).
//
// Model: "Chess Board" by Idmental, CC BY 4.0
// https://sketchfab.com/3d-models/chess-board-901eeeca884f4622ac37b7e8f7cb82c3
//
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "core/shape/boundingBox.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "scene/composition/layerComposition.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

constexpr int LAYERID_SPOTLIGHT = 70;

void setRenderLayersRecursive(GraphNode* node, const std::vector<int>& layers)
{
    if (!node) {
        return;
    }
    if (auto* entity = dynamic_cast<Entity*>(node)) {
        if (auto* render = entity->findComponent<RenderComponent>()) {
            render->setLayers(layers);
        }
    }
    for (const auto& child : node->children()) {
        setRenderLayersRecursive(child.get(), layers);
    }
}

void setRenderShadowsRecursive(GraphNode* node, const bool cast, const bool receive)
{
    if (!node) {
        return;
    }
    if (auto* entity = dynamic_cast<Entity*>(node)) {
        if (auto* render = entity->findComponent<RenderComponent>()) {
            render->setCastShadows(cast);
            render->setReceiveShadows(receive);
        }
    }
    for (const auto& child : node->children()) {
        setRenderShadowsRecursive(child.get(), cast, receive);
    }
}

BoundingBox calcEntityAABB(Entity* entity)
{
    BoundingBox bbox;
    bbox.setCenter(0, 0, 0);
    bbox.setHalfExtents(0, 0, 0);
    if (!entity) return bbox;
    bool hasAny = false;
    for (auto* render : RenderComponent::instances()) {
        if (!render || !render->entity()) continue;
        auto* owner = render->entity();
        if (owner != entity && !owner->isDescendantOf(entity)) continue;
        for (auto* mi : render->meshInstances()) {
            if (!mi) continue;
            if (!hasAny) { bbox = mi->aabb(); hasAny = true; }
            else { bbox.add(mi->aabb()); }
        }
    }
    return bbox;
}

// Debug shader passes on the top viewport, matching where upstream demonstrates them.
// Switching costs no shader recompile — the mode is a runtime uniform.
struct DebugPassEntry
{
    DebugShaderPass pass;
    const char* name;
};

constexpr DebugPassEntry debugPasses[] = {
    {DebugShaderPass::DEBUGPASS_NONE, "none (forward)"},
    {DebugShaderPass::DEBUGPASS_ALBEDO, "albedo"},
    {DebugShaderPass::DEBUGPASS_WORLDNORMAL, "world normal"},
    {DebugShaderPass::DEBUGPASS_OPACITY, "opacity"},
    {DebugShaderPass::DEBUGPASS_SPECULARITY, "specularity"},
    {DebugShaderPass::DEBUGPASS_GLOSS, "gloss"},
    {DebugShaderPass::DEBUGPASS_METALNESS, "metalness"},
    {DebugShaderPass::DEBUGPASS_AO, "ao"},
    {DebugShaderPass::DEBUGPASS_EMISSION, "emission"},
    {DebugShaderPass::DEBUGPASS_LIGHTING, "lighting"},
    {DebugShaderPass::DEBUGPASS_UV0, "uv0"},
};

constexpr float debugPassInterval = 2.5f;

class MultiViewExample final: public ExampleApp
{
public:
    MultiViewExample()
        : ExampleApp({.title = "Multi-View Control Room", .width = 1200, .height = 760}) {}

protected:
    bool create() override
    {
        scene()->setSkyboxMip(1);

        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        const auto helipadResource = _helipad->resource();
        if (!helipadResource) {
            spdlog::error("Failed to load helipad env atlas");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        // Layer setup: world + dedicated spotlight layer + skybox.
        const auto defaultLayers = scene()->layers();
        const auto worldLayer = defaultLayers ? defaultLayers->getLayerById(LAYERID_WORLD) : nullptr;
        const auto skyboxLayer = defaultLayers ? defaultLayers->getLayerById(LAYERID_SKYBOX) : nullptr;
        const auto immediateLayer = defaultLayers ? defaultLayers->getLayerById(LAYERID_IMMEDIATE) : nullptr;
        const auto uiLayer = defaultLayers ? defaultLayers->getLayerById(LAYERID_UI) : nullptr;
        if (!worldLayer || !skyboxLayer) {
            spdlog::error("Failed to resolve required default layers");
            return false;
        }

        auto spotLightLayer = std::make_shared<Layer>("SpotLightLayer", LAYERID_SPOTLIGHT);
        auto composition = std::make_shared<LayerComposition>("multi-view");
        composition->pushOpaque(spotLightLayer);
        composition->pushOpaque(worldLayer);
        composition->pushOpaque(skyboxLayer);
        composition->pushTransparent(worldLayer);
        composition->pushTransparent(spotLightLayer);
        if (immediateLayer) {
            composition->pushOpaque(immediateLayer);
            composition->pushTransparent(immediateLayer);
        }
        if (uiLayer) {
            composition->pushTransparent(uiLayer);
        }
        scene()->setLayers(composition);

        // Load the chess-board GLB once. Like upstream, it belongs to BOTH lighting
        // layers, so the world (left/top) cameras light it with the directional light
        // and the spotlight (right) camera lights it with the spot light.
        _boardAsset = std::make_unique<Asset>(
            "chess-board", AssetType::CONTAINER, assetPath("models/chess-board.glb"));
        const auto boardResource = _boardAsset->resource();
        if (!boardResource || !std::holds_alternative<ContainerResource*>(*boardResource)) {
            spdlog::error("Failed to load chess-board.glb");
            return false;
        }

        auto* boardContainer = std::get<ContainerResource*>(*boardResource);
        auto* boardEntity = boardContainer ? boardContainer->instantiateRenderEntity() : nullptr;
        if (!boardEntity) {
            spdlog::error("Failed to instantiate chess-board.glb render entity");
            return false;
        }
        boardEntity->setEngine(engine());
        // Both lighting layers, so each camera lights the same geometry from its own
        // light. The board keeps its authored size (~337 units across) — the camera
        // distances and ortho heights below are all framed against that, so scaling it
        // down to a "tidy" 100 units, as this port used to, pushed every viewport out
        // to a distant wide shot instead of upstream's close-ups.
        setRenderLayersRecursive(boardEntity, {LAYERID_WORLD, LAYERID_SPOTLIGHT});
        setRenderShadowsRecursive(boardEntity, true, true);
        root()->addChild(boardEntity);

        // Left camera: perspective, bottom-left viewport, World layer.
        _leftCamEntity = createCamera(Vector3(100.0f, 35.0f, 100.0f));
        _leftCam = _leftCamEntity->findComponent<CameraComponent>();
        if (_leftCam && _leftCam->camera()) {
            _leftCam->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
            _leftCam->camera()->setRect(Vector4(0.0f, 0.0f, 0.5f, 0.5f));
            _leftCam->camera()->setScissorRect(Vector4(0.0f, 0.0f, 0.5f, 0.5f));
            _leftCam->camera()->setFarClip(500.0f);
            _leftCam->setToneMapping(TONEMAP_ACES);
        }
        _leftCamEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        // Right camera: orthographic top-down, bottom-right viewport, spot light layer
        // only — so this view is lit by the yellow spot and never by the directional.
        // The +X up vector is what turns the board square-on in the viewport.
        auto* rightCamEntity = createCamera(Vector3(0.0f, 150.0f, 0.0f));
        _rightCam = rightCamEntity->findComponent<CameraComponent>();
        if (_rightCam && _rightCam->camera()) {
            _rightCam->setLayers({LAYERID_SPOTLIGHT, LAYERID_SKYBOX});
            _rightCam->camera()->setRect(Vector4(0.5f, 0.0f, 0.5f, 0.5f));
            _rightCam->camera()->setScissorRect(Vector4(0.5f, 0.0f, 0.5f, 0.5f));
            _rightCam->camera()->setProjection(ProjectionType::Orthographic);
            _rightCam->camera()->setOrthoHeight(150.0f);
            _rightCam->camera()->setFarClip(500.0f);
            _rightCam->setToneMapping(TONEMAP_ACES);
            // DEVIATION: upstream lets every camera clear its own viewport. Here the
            // second and third cameras must not clear, or they wipe the viewports drawn
            // before them.
            _rightCam->camera()->setClearColorBuffer(false);
            _rightCam->camera()->setClearDepthBuffer(false);
            _rightCam->camera()->setClearStencilBuffer(false);
        }
        rightCamEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));

        // Top camera: perspective, full-width upper half, World layer.
        auto* topCamEntity = createCamera(Vector3(-100.0f, 75.0f, 100.0f));
        _topCam = topCamEntity->findComponent<CameraComponent>();
        if (_topCam && _topCam->camera()) {
            _topCam->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
            _topCam->camera()->setRect(Vector4(0.0f, 0.5f, 1.0f, 0.5f));
            _topCam->camera()->setScissorRect(Vector4(0.0f, 0.5f, 1.0f, 0.5f));
            _topCam->camera()->setFarClip(500.0f);
            _topCam->setToneMapping(TONEMAP_ACES);
            _topCam->camera()->setClearColorBuffer(false);
            _topCam->camera()->setClearDepthBuffer(false);
            _topCam->camera()->setClearStencilBuffer(false);
        }
        topCamEntity->lookAt(Vector3(0.0f, 7.0f, 0.0f));

        // Guard against unintended extra cameras rendering full-screen.
        for (auto* cameraComp : CameraComponent::instances()) {
            if (!cameraComp) {
                continue;
            }
            if (cameraComp != _leftCam && cameraComp != _rightCam && cameraComp != _topCam) {
                cameraComp->setEnabled(false);
                spdlog::warn("Disabled unintended camera component in multi-view example");
            }
        }

        logCameraRect("LeftCamera", _leftCam);
        logCameraRect("RightCamera", _rightCam);
        logCameraRect("TopCamera", _topCam);
        logRenderActions(composition);

        // Directional light affects only world-layer cameras.
        auto* dirLightEntity = new Entity();
        dirLightEntity->setEngine(engine());
        if (auto* dirLight = static_cast<LightComponent*>(dirLightEntity->addComponent<LightComponent>())) {
            dirLight->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            dirLight->setLayers({LAYERID_WORLD});
            dirLight->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            dirLight->setIntensity(5.0f);
            dirLight->setRange(500.0f);
            dirLight->setShadowDistance(500.0f);
            dirLight->setCastShadows(true);
            dirLight->setShadowBias(0.2f);
            dirLight->setShadowNormalBias(0.05f);
        }
        dirLightEntity->setLocalEulerAngles(45.0f, 0.0f, 30.0f);
        root()->addChild(dirLightEntity);

        // Spot light affects only right-camera layer.
        _spotLightEntity = new Entity();
        _spotLightEntity->setEngine(engine());
        if (auto* spotLight = static_cast<LightComponent*>(_spotLightEntity->addComponent<LightComponent>())) {
            spotLight->setType(LightType::LIGHTTYPE_SPOT);
            spotLight->setLayers({LAYERID_SPOTLIGHT});
            spotLight->setColor(Color(1.0f, 1.0f, 0.0f, 1.0f));
            spotLight->setIntensity(7.0f);
            spotLight->setInnerConeAngle(20.0f);
            spotLight->setOuterConeAngle(80.0f);
            spotLight->setRange(200.0f);
            spotLight->setShadowDistance(200.0f);
            spotLight->setCastShadows(true);
            spotLight->setShadowBias(0.2f);
            spotLight->setShadowNormalBias(0.05f);
        }
        // Left unrotated on purpose, as upstream does: the beam points straight down
        // its own -Y and the light simply slides around above the board.
        _spotLightEntity->setLocalPosition(40.0f, 60.0f, 40.0f);
        root()->addChild(_spotLightEntity);

        applyDebugPass();
        spdlog::info("Press D to step the top viewport's debug shader pass (stops auto-cycling)");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_D) {
            _debugPassAutoCycle = false;
            _debugPassIndex = (_debugPassIndex + 1) % std::size(debugPasses);
            applyDebugPass();
            return true;
        }
        return false;
    }

    void update(const float dt) override
    {
        _time += dt;
        const float time = _time;

        if (_debugPassAutoCycle) {
            _debugPassTimer += dt;
            if (_debugPassTimer >= debugPassInterval) {
                _debugPassTimer = 0.0f;
                _debugPassIndex = (_debugPassIndex + 1) % std::size(debugPasses);
                applyDebugPass();
            }
        }

        // Orbit the left camera, slide the spot light, and breathe the ortho view.
        _leftCamEntity->setLocalPosition(100.0f * std::sin(time * 0.2f), 35.0f, 100.0f * std::cos(time * 0.2f));
        _leftCamEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        _spotLightEntity->setLocalPosition(40.0f * std::sin(time * 0.5f), 60.0f, 40.0f * std::cos(time * 0.5f));

        if (_rightCam && _rightCam->camera()) {
            _rightCam->camera()->setOrthoHeight(90.0f + std::sin(time * 0.3f) * 60.0f);
        }
    }

private:
    static void logCameraRect(const char* name, CameraComponent* cameraComp)
    {
        if (!cameraComp || !cameraComp->camera()) {
            return;
        }
        const auto rect = cameraComp->camera()->rect();
        spdlog::info(
            "{} rect=({}, {}, {}, {})",
            name, rect.getX(), rect.getY(), rect.getZ(), rect.getW());
    }

    void logRenderActions(const std::shared_ptr<LayerComposition>& composition) const
    {
        if (!composition) {
            return;
        }
        const auto& actions = composition->renderActions();
        spdlog::info("Multi-view render actions: {}", actions.size());
        for (size_t i = 0; i < actions.size(); ++i) {
            const auto* action = actions[i];
            if (!action || !action->camera || !action->layer) {
                continue;
            }
            const char* cameraName = action->camera == _leftCam
                ? "LeftCamera"
                : (action->camera == _rightCam
                    ? "RightCamera"
                    : (action->camera == _topCam ? "TopCamera" : "OtherCamera"));
            spdlog::info(
                "  [{}] {} layer={} transparent={} enabled={}",
                i, cameraName, action->layer->id(),
                action->transparent ? "true" : "false",
                action->camera->enabled() ? "true" : "false");
        }
    }

    void applyDebugPass() const
    {
        // Upstream's HUD drives the top and right cameras together; the left one
        // stays on the forward pass as a reference.
        if (_topCam && _topCam->camera()) {
            _topCam->camera()->setDebugShaderPass(debugPasses[_debugPassIndex].pass);
        }
        if (_rightCam && _rightCam->camera()) {
            _rightCam->camera()->setDebugShaderPass(debugPasses[_debugPassIndex].pass);
        }
        spdlog::info("Top + right viewport debug pass: {}", debugPasses[_debugPassIndex].name);
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _boardAsset;

    CameraComponent* _leftCam = nullptr;
    CameraComponent* _rightCam = nullptr;
    CameraComponent* _topCam = nullptr;
    Entity* _leftCamEntity = nullptr;
    Entity* _spotLightEntity = nullptr;

    size_t _debugPassIndex = 0;
    bool _debugPassAutoCycle = true;
    float _debugPassTimer = 0.0f;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MultiViewExample)
