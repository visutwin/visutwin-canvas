// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream graphics/render-to-texture.
//
// Three layers: World (objects rendered by both cameras), Excluded (rendered by
// the main camera only) and Skybox (both). A texture camera orbits the scene and
// renders World + Skybox into a 512x256 render target; the main camera renders
// World + Excluded + Skybox to the screen. A "tv" plane on the Excluded layer
// shows the rendered texture as its emissive map, and the texture is also drawn
// as a small preview in the bottom-right corner. Every 5 seconds the texture camera
// switches between perspective and orthographic projection. A red-blue-yellow
// sphere, cone and box sit on a checkerboard ground, a particle system sprays
// above the box, and a white sphere marks the texture camera's position.
//
// DEVIATIONS from upstream:
//  - the render texture is PIXELFORMAT_RGBA8, not SRGBA8: the engine has no sRGB
//    pixel formats. The forward shader decodes every emissive texture as sRGB,
//    and the target is written gamma-encoded, so the result is the same.
//    RENDERTARGET_ORIGIN_TOP and the transient-attachment hints have no
//    counterpart and are omitted.
//  - `textures.draw(texture, 0.725, 0.725, 0.25, 0.25)` has no immediate
//    equivalent (drawing to the back buffer after Engine::render is forbidden).
//    The preview is a RenderPassDownsample appended to the frame graph with the
//    same viewport-fraction rectangle, so it draws over everything rather than
//    straight after the skybox, and does not follow a window resize.
//  - the particle system has no velocity curves. Upstream's local velocity
//    ramps to a random +/-8 per axis at half life and its world velocity rises to
//    6 and falls to -48 in y; this is approximated with a random +/-8 spread, an
//    upward initial velocity of 6 and gravity of -54.
//  - orbitCamera is CameraControls. Upstream's orbit camera keeps the camera's
//    lookAt(1, 4, 0) direction and places it at its current distance from the
//    focus entity (the plane, at the origin); the camera here is placed at that
//    resulting pose. distanceMax 20 becomes the zoom range; CameraControls has
//    no inertia factor.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <variant>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "core/math/curve.h"
#include "core/math/curveSet.h"
#include "framework/assets/asset.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassDownsample.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// Upstream allocates the Excluded layer's id automatically; here it only has to be
// unique and outside the built-in range.
constexpr int LAYERID_EXCLUDED = 20;

class RenderToTextureExample final: public ExampleApp
{
public:
    RenderToTextureExample(): ExampleApp({.title = "Render to Texture"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ParticleSystemComponentSystem>();
    }

    bool create() override
    {
        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _checkerboard = std::make_unique<Asset>(
            "checkerboard", AssetType::TEXTURE, assetPath("textures/checkboard.png"));

        const auto helipadResource = _helipad->resource();
        const auto checkerResource = _checkerboard->resource();
        if (!helipadResource || !checkerResource) {
            spdlog::error("Failed to load required textures");
            return false;
        }

        // Create texture and render target for rendering into, including depth buffer
        TextureOptions textureOptions;
        textureOptions.name = "RT";
        textureOptions.width = 512;
        textureOptions.height = 256;
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = true;
        _texture = std::make_shared<Texture>(device().get(), textureOptions);
        _texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        _texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions rtOptions;
        rtOptions.graphicsDevice = device().get();
        rtOptions.name = "RT";
        rtOptions.colorBuffer = _texture.get();
        rtOptions.depth = true;
        rtOptions.samples = 2;
        rtOptions.autoResolve = true;
        const auto renderTarget = device()->createRenderTarget(rtOptions);

        // Create a layer for object that do not render into texture, add it right after the world layer
        const auto layers = scene()->layers();
        layers->insert(std::make_shared<Layer>("Excluded", LAYERID_EXCLUDED), 1);

        // Create ground plane and 3 primitives, visible in world layer
        _planeMaterial = std::make_shared<StandardMaterial>();
        _planeMaterial->setDiffuse(Color(3.0f, 4.0f, 2.0f, 1.0f));
        _planeMaterial->setDiffuseMap(std::get<Texture*>(*checkerResource));
        _planeMaterial->setDiffuseMapTiling(Vector2(10.0f, 10.0f));
        createPrimitive("plane", _planeMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(20.0f, 20.0f, 20.0f), {LAYERID_WORLD});

        createColoredPrimitive("sphere", Vector3(-2.0f, 1.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f),
            Color::RED, LAYERID_WORLD);
        createColoredPrimitive("cone", Vector3(0.0f, 1.0f, -2.0f), Vector3(2.0f, 2.0f, 2.0f),
            Color::CYAN, LAYERID_WORLD);
        createColoredPrimitive("box", Vector3(2.0f, 1.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f),
            Color::YELLOW, LAYERID_WORLD);

        // Particle system
        createParticleSystem(Vector3(2.0f, 3.0f, 0.0f));

        // Create main camera, which renders entities in world, excluded and skybox layers.
        // Upstream: translate(0, 9, 15), lookAt(1, 4, 0), then the orbit camera keeps that
        // direction and re-seats the camera at the same distance from the plane's centre.
        const Vector3 initialPosition(0.0f, 9.0f, 15.0f);
        const Vector3 lookDirection = (Vector3(1.0f, 4.0f, 0.0f) - initialPosition).normalized();
        const float orbitDistance = initialPosition.length();
        auto* camera = createCamera(lookDirection * -orbitDistance);
        camera->setName("Camera");
        auto* cameraComp = camera->findComponent<CameraComponent>();
        cameraComp->camera()->setFov(100.0f);
        cameraComp->setLayers({LAYERID_WORLD, LAYERID_EXCLUDED, LAYERID_SKYBOX, LAYERID_IMMEDIATE, LAYERID_UI});
        cameraComp->setToneMapping(TONEMAP_ACES);
        camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setZoomRange(Vector2(0.0f, 20.0f));
        }

        // Create texture camera, which renders entities in world and skybox layers into the texture
        _textureCamera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _textureCamera->setName("TextureCamera");
        _textureCameraComp = _textureCamera->findComponent<CameraComponent>();
        _textureCameraComp->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
        _textureCameraComp->setToneMapping(TONEMAP_ACES);
        // Rendered before the main camera (default priority 0) each frame
        _textureCameraComp->setPriority(-1);
        _textureCameraComp->camera()->setRenderTarget(renderTarget);

        // Add sphere at the position of this camera to see it in the world
        _defaultMaterial = std::make_shared<StandardMaterial>();
        if (auto* render = static_cast<RenderComponent*>(_textureCamera->addComponent<RenderComponent>())) {
            render->setMaterial(_defaultMaterial.get());
            render->setType("sphere");
        }

        // Create an Entity with a omni light component and add it to world layer (and so used by both cameras)
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lightComp->setType(LightType::LIGHTTYPE_OMNI);
            lightComp->setColor(Color::WHITE);
            lightComp->setRange(200.0f);
            lightComp->setCastShadows(true);
            lightComp->setLayers({LAYERID_WORLD});
        }
        light->setLocalPosition(0.0f, 2.0f, 5.0f);
        root()->addChild(light);

        // Create a plane called tv which we use to display rendered texture
        // This is only added to excluded Layer, so it does not render into texture
        _tvMaterial = std::make_shared<StandardMaterial>();
        _tvMaterial->setDiffuse(Color::BLACK);
        _tvMaterial->setEmissiveMap(_texture.get());   // assign the rendered texture as an emissive texture
        _tvMaterial->setEmissive(Color::WHITE);
        auto* tv = createPrimitive("plane", _tvMaterial.get(), Vector3(6.0f, 8.0f, -5.0f),
            Vector3(20.0f, 10.0f, 10.0f), {LAYERID_EXCLUDED});
        tv->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        if (auto* render = tv->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
            render->setReceiveShadows(false);
        }

        // Setup skydome, use top mipmap level of cubemap (full resolution)
        scene()->setSkyboxMip(0);
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        // Display the texture in the bottom-right corner, in the main frame only
        addTexturePreview(0.725f, 0.725f, 0.25f, 0.25f);

        return true;
    }

    void update(const float dt) override
    {
        // Rotate texture camera around the objects
        _time += dt;
        _textureCamera->setLocalPosition(12.0f * std::sin(_time), 3.0f, 12.0f * std::cos(_time));
        _textureCamera->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        // Every 5 seconds switch texture camera between perspective and orthographic projection
        _switchTime += dt;
        if (_switchTime > 5.0f) {
            _switchTime = 0.0f;
            auto* cam = _textureCameraComp->camera();
            if (cam->projection() == ProjectionType::Orthographic) {
                cam->setProjection(ProjectionType::Perspective);
            } else {
                cam->setProjection(ProjectionType::Orthographic);
                cam->setOrthoHeight(5.0f);
            }
        }
    }

    void destroy() override
    {
        // The preview pass is registered with the renderer, so it goes while the engine is alive.
        if (_previewPass) {
            engine()->renderer()->removeAppendPass(_previewPass);
        }
        _previewPass.reset();
    }

private:
    // helper function to create a primitive with shape type, position, scale, color and layer
    Entity* createColoredPrimitive(const char* type, const Vector3& position, const Vector3& scale,
        const Color& color, const int layer)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        _materials.push_back(material);
        return createPrimitive(type, material.get(), position, scale, {layer});
    }

    // helper function to create a basic particle system
    void createParticleSystem(const Vector3& position)
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        root()->addChild(entity);
        entity->setLocalPosition(position);

        auto* particles = static_cast<ParticleSystemComponent*>(
            entity->addComponent<ParticleSystemComponent>());
        if (!particles) {
            return;
        }
        auto& o = particles->options();
        o.numParticles = 200;
        o.lifetime = 1.0f;
        o.lifetime2 = 1.0f;
        o.rate = 0.01f;
        o.scaleGraph = Curve(std::vector<float>{0.0f, 0.5f});
        o.blendType = ParticleBlendType::BLEND_NORMAL;
        // DEVIATION: stands in for the local/world velocity curves, see the header
        o.initialVelocity = Vector3(0.0f, 6.0f, 0.0f);
        o.velocitySpread = Vector3(8.0f, 8.0f, 8.0f);
        o.gravity = Vector3(0.0f, -54.0f, 0.0f);
        particles->apply();
        particles->play();
    }

    // Upstream's TextureRenderer::draw(x, y, w, h) takes the left and top edge and the size,
    // each as a fraction of the viewport, and shows texture row 0 at the top.
    void addTexturePreview(const float x, const float y, const float w, const float h)
    {
        const auto [deviceWidth, deviceHeight] = device()->size();
        const auto screenW = static_cast<float>(std::max(1, deviceWidth));
        const auto screenH = static_cast<float>(std::max(1, deviceHeight));

        _previewPass = std::make_shared<RenderPassDownsample>(device(), _texture.get());
        _previewPass->init(nullptr);
        _previewPass->setRequiresCubemaps(false);

        const Vector4 viewport(x * screenW, y * screenH, w * screenW, h * screenH);
        _previewPass->setViewport(viewport);
        _previewPass->setScissor(viewport);

        engine()->renderer()->addAppendPass(_previewPass);
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _checkerboard;

    std::shared_ptr<Texture> _texture;
    std::shared_ptr<StandardMaterial> _planeMaterial;
    std::shared_ptr<StandardMaterial> _tvMaterial;
    std::shared_ptr<StandardMaterial> _defaultMaterial;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<RenderPassDownsample> _previewPass;

    Entity* _textureCamera = nullptr;
    CameraComponent* _textureCameraComp = nullptr;

    float _time = 0.0f;
    float _switchTime = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(RenderToTextureExample)
