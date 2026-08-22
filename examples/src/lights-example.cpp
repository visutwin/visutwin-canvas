// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Lights example — port of upstream graphics/lights. A statue on a large grey
// ground box, lit by one of each local light type plus a directional key light,
// every one of them animated:
//   * SPOT (white)      — orbits high above, aimed at the scene, and projects the
//                         alpha channel of heart.png as a light COOKIE.
//   * OMNI (yellow)     — orbits low and fast, casts cubemap shadows, and projects
//                         a christmas cubemap COOKIE that spins with the light.
//   * DIRECTIONAL (cyan)— the key light, sweeping its yaw, casting cascaded shadows.
// Keys 1/2/3 toggle omni/spot/directional (upstream's key order); orbit camera.
//
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "core/math/quaternion.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// Omni light cookie: six faces assembled into a cubemap below. Face order is the
// engine's cube convention: +X, -X, +Y, -Y, +Z, -Z.
const std::array<const char*, 6> xmasFaceFiles = {
    "xmas_posx", "xmas_negx", "xmas_posy", "xmas_negy", "xmas_posz", "xmas_negz"
};

// Assemble a cubemap from six loaded 2D face textures. DEVIATION: upstream builds
// this through a 'cubemap' Asset that references six texture assets; this port has
// no cubemap asset type, so the faces are copied into one cubemap texture here.
std::shared_ptr<Texture> makeCubemapFromFaces(GraphicsDevice* device,
                                              const std::array<Texture*, 6>& faces,
                                              const std::string& name)
{
    const Texture* first = faces[0];
    if (!first) {
        return nullptr;
    }
    const uint32_t size = first->width();
    for (const Texture* face : faces) {
        if (!face || face->width() != size || face->height() != size) {
            spdlog::error("Cubemap '{}': faces must all be square and the same size", name);
            return nullptr;
        }
    }

    TextureOptions options;
    options.name = name;
    options.width = size;
    options.height = size;
    options.format = PixelFormat::PIXELFORMAT_RGBA8;
    options.cubemap = true;
    options.mipmaps = true;
    options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
    options.magFilter = FilterMode::FILTER_LINEAR;
    auto cubemap = std::make_shared<Texture>(device, options);

    for (uint32_t face = 0; face < 6; ++face) {
        const auto* pixels = static_cast<const uint8_t*>(faces[face]->getLevel(0));
        const size_t dataSize = faces[face]->getLevelDataSize(0);
        if (!pixels || dataSize == 0) {
            spdlog::error("Cubemap '{}': face {} has no CPU-side pixel data", name, face);
            return nullptr;
        }
        cubemap->setLevelData(0, pixels, dataSize, face);
    }
    // mipmaps = true makes upload() generate the roughness chain on the GPU.
    cubemap->upload();
    return cubemap;
}

class LightsExample final: public ExampleApp
{
public:
    LightsExample(): ExampleApp({.title = "Lights Example", .width = 1100, .height = 750}) {}

protected:
    bool create() override
    {
        spdlog::info("*** Lights Example Started ***");

        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // -----------------------------------------------------------------------
        // Cookie textures.
        // -----------------------------------------------------------------------
        // Spot light cookie: the heart's ALPHA channel masks the beam.
        _heartAsset = std::make_unique<Asset>(
            "heart", AssetType::TEXTURE, assetPath("textures/heart.png"),
            AssetData{.mipmaps = true});

        Texture* heartCookie = nullptr;
        if (const auto heartResource = _heartAsset->resource()) {
            heartCookie = std::get<Texture*>(*heartResource);
        } else {
            spdlog::warn("heart.png failed to load — the spot light keeps a plain beam");
        }

        // Load the six cubemap faces, then assemble them. The face assets stay alive
        // for the run; only their CPU-side level 0 is read, at assembly time.
        std::array<Texture*, 6> xmasFaces{};
        bool allFacesLoaded = true;
        for (size_t i = 0; i < xmasFaceFiles.size(); ++i) {
            auto asset = std::make_unique<Asset>(
                xmasFaceFiles[i],
                AssetType::TEXTURE,
                assetPath("cubemaps/xmas_faces/" + std::string(xmasFaceFiles[i]) + ".png")
            );
            if (const auto resource = asset->resource()) {
                xmasFaces[i] = std::get<Texture*>(*resource);
            } else {
                allFacesLoaded = false;
            }
            _xmasFaceAssets.push_back(std::move(asset));
        }
        if (allFacesLoaded) {
            _xmasCookie = makeCubemapFromFaces(device().get(), xmasFaces, "xmas_cubemap");
        }
        if (!_xmasCookie) {
            spdlog::warn("xmas cubemap failed to build — the omni light keeps a plain falloff");
        }

        // -----------------------------------------------------------------------
        // Statue.
        // -----------------------------------------------------------------------
        _statueAsset = std::make_unique<Asset>(
            "statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        const auto statueResource = _statueAsset->resource();
        if (!statueResource || !std::holds_alternative<ContainerResource*>(*statueResource)) {
            spdlog::error("statue.glb failed to load");
            return false;
        }
        auto* statueContainer = std::get<ContainerResource*>(*statueResource);
        auto* statue = statueContainer ? statueContainer->instantiateRenderEntity() : nullptr;
        if (!statue) {
            spdlog::error("statue.glb instantiate failed");
            return false;
        }
        statue->setEngine(engine());
        root()->addChild(statue);

        // -----------------------------------------------------------------------
        // Camera. Upstream authors it at (0, 15, 35) and its orbit script pivots on
        // the AABB centre of everything renderable at script-init time — which is the
        // statue alone, since the ground is added after the camera. That AABB centre
        // (verified against the running reference) sits at (0.17, 7.52, 0.02), which
        // is what puts the statue where the reference frames it; orbiting the origin
        // instead tilts the whole scene up the frame.
        // -----------------------------------------------------------------------
        auto* camera = createCamera(Vector3(0.0f, 15.0f, 35.0f));
        if (auto* cameraComp = camera->findComponent<CameraComponent>();
            cameraComp && cameraComp->camera()) {
            cameraComp->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        }

        _controls = addOrbitControls(camera, kStatueCenter);
        _controls->setZoomRange(Vector2(1.0f, 500.0f));
        _controls->storeResetState();

        // -----------------------------------------------------------------------
        // Ground.
        // -----------------------------------------------------------------------
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setName("ground");
        _groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.5f, 1.0f));
        _groundMaterial->setUseMetalness(true);
        _groundMaterial->setMetalness(0.5f);
        _groundMaterial->setGloss(0.5f);

        auto* ground = createPrimitive("box", _groundMaterial.get(), Vector3(0.0f, -0.5f, 0.0f),
            Vector3(70.0f, 1.0f, 70.0f));
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setCastShadows(true);
            render->setReceiveShadows(true);
        }

        // -----------------------------------------------------------------------
        // 1. SPOT — white, heart-alpha cookie, 2D shadows.
        // -----------------------------------------------------------------------
        _spotLight = new Entity();
        _spotLight->setEngine(engine());
        _spotComp = static_cast<LightComponent*>(_spotLight->addComponent<LightComponent>());
        if (_spotComp) {
            _spotComp->setType(LightType::LIGHTTYPE_SPOT);
            _spotComp->setColor(Color(1.0f, 1.0f, 1.0f));
            _spotComp->setIntensity(0.8f);
            _spotComp->setInnerConeAngle(30.0f);
            _spotComp->setOuterConeAngle(31.0f);
            _spotComp->setRange(100.0f);
            _spotComp->setCastShadows(true);
            _spotComp->setShadowBias(0.05f);
            _spotComp->setShadowNormalBias(0.03f);
            _spotComp->setShadowResolution(2048);
            _spotComp->setCookie(heartCookie);
            _spotComp->setCookieChannel(CookieChannel::COOKIE_CHANNEL_A);
            _spotComp->setCookieIntensity(1.0f);
        }
        root()->addChild(_spotLight);

        // Emissive cone marking the light itself.
        _coneMaterial = std::make_shared<StandardMaterial>();
        _coneMaterial->setName("spot-marker");
        _coneMaterial->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        auto* cone = new Entity();
        cone->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(cone->addComponent<RenderComponent>())) {
            render->setType("cone");
            render->setMaterial(_coneMaterial.get());
            render->setCastShadows(false);
        }
        _spotLight->addChild(cone);

        // -----------------------------------------------------------------------
        // 2. OMNI — yellow, christmas cubemap cookie, cubemap shadows.
        // -----------------------------------------------------------------------
        _omniLight = new Entity();
        _omniLight->setEngine(engine());
        _omniComp = static_cast<LightComponent*>(_omniLight->addComponent<LightComponent>());
        if (_omniComp) {
            _omniComp->setType(LightType::LIGHTTYPE_OMNI);
            _omniComp->setColor(Color(1.0f, 1.0f, 0.0f));
            _omniComp->setIntensity(0.8f);
            _omniComp->setRange(111.0f);
            _omniComp->setCastShadows(true);
            _omniComp->setShadowBias(0.05f);
            _omniComp->setShadowNormalBias(0.03f);
            _omniComp->setShadowType(SHADOW_PCF3_32F);
            _omniComp->setShadowResolution(256);
            _omniComp->setCookie(_xmasCookie.get());
            _omniComp->setCookieChannel(CookieChannel::COOKIE_CHANNEL_RGB);
            _omniComp->setCookieIntensity(1.0f);
        }
        // Upstream puts the marker sphere on the light entity itself.
        _omniMarkerMaterial = std::make_shared<StandardMaterial>();
        _omniMarkerMaterial->setName("omni-marker");
        _omniMarkerMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _omniMarkerMaterial->setEmissive(Color(1.0f, 1.0f, 0.0f, 1.0f));
        if (auto* render = static_cast<RenderComponent*>(_omniLight->addComponent<RenderComponent>())) {
            render->setType("sphere");
            render->setMaterial(_omniMarkerMaterial.get());
            render->setCastShadows(false);
        }
        root()->addChild(_omniLight);

        // -----------------------------------------------------------------------
        // 3. DIRECTIONAL — cyan key light with cascaded shadows.
        // -----------------------------------------------------------------------
        _dirLight = createDirectionalLight(Vector3(0.0f, 0.0f, 0.0f), Color(0.0f, 1.0f, 1.0f), 0.8f, true);
        _dirComp = _dirLight->findComponent<LightComponent>();
        if (_dirComp) {
            _dirComp->setRange(100.0f);
            _dirComp->setShadowDistance(50.0f);
            _dirComp->setShadowBias(0.1f);
            _dirComp->setShadowNormalBias(0.2f);
        }

        spdlog::info("Keys: 1=OMNI(yellow, cubemap cookie)  2=SPOT(white, heart cookie)  3=DIRECTIONAL(cyan)");
        spdlog::info("      F focus | R reset | Esc quit | LMB/RMB orbit, Shift/MMB pan, Wheel zoom");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_1:
            toggle(_omniComp, "OMNI");
            return true;
        case SDLK_2:
            toggle(_spotComp, "SPOT");
            return true;
        case SDLK_3:
            toggle(_dirComp, "DIRECTIONAL");
            return true;
        case SDLK_F:
            if (_controls) {
                _controls->focus(kStatueCenter, 35.772f);
            }
            return true;
        default:
            return false;
        }
    }

    void update(const float dt) override
    {
        _angleRad += 0.3f * dt;

        // Spot: aim at (0, -5, 0), then roll the node so its -Y (the emission
        // axis) points down the view direction, then move it. Upstream aims
        // before it moves, so the aim trails the position by one frame — kept
        // as-is, since the lag is what the reference animation shows.
        _spotLight->lookAt(Vector3(0.0f, -5.0f, 0.0f));
        _spotLight->rotateLocal(90.0f, 0.0f, 0.0f);
        _spotLight->setLocalPosition(15.0f * std::sin(_angleRad), 25.0f, 15.0f * std::cos(_angleRad));

        // Omni: a faster counter-rotating orbit, spinning about its own Y so the
        // projected cubemap cookie sweeps across the scene.
        _omniLight->setLocalPosition(5.0f * std::sin(-2.0f * _angleRad), 10.0f,
                                     5.0f * std::cos(-2.0f * _angleRad));
        // Upstream uses the world-space rotate(); the light is a root child with
        // no parent rotation, so a local rotation is the same thing.
        _omniLight->rotateLocal(0.0f, 50.0f * dt, 0.0f);

        _dirLight->setLocalEulerAngles(45.0f, -60.0f * _angleRad, 0.0f);
    }

    void destroy() override
    {
        spdlog::info("*** Lights Example Finished ***");
    }

private:
    static void toggle(LightComponent* component, const char* name)
    {
        if (!component) {
            return;
        }
        component->setEnabled(!component->enabled());
        spdlog::info("{}: {}", name, component->enabled() ? "ON" : "OFF");
    }

    std::unique_ptr<Asset> _statueAsset;
    std::unique_ptr<Asset> _heartAsset;
    std::vector<std::unique_ptr<Asset>> _xmasFaceAssets;
    std::shared_ptr<Texture> _xmasCookie;

    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::shared_ptr<StandardMaterial> _coneMaterial;
    std::shared_ptr<StandardMaterial> _omniMarkerMaterial;

    Entity* _spotLight = nullptr;
    Entity* _omniLight = nullptr;
    Entity* _dirLight = nullptr;
    LightComponent* _spotComp = nullptr;
    LightComponent* _omniComp = nullptr;
    LightComponent* _dirComp = nullptr;
    CameraControls* _controls = nullptr;

    const Vector3 kStatueCenter{0.173f, 7.523f, 0.018f};
    float _angleRad = 1.0f;
};

VISUTWIN_EXAMPLE_MAIN(LightsExample)
