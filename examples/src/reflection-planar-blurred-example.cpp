// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Blurred planar reflections: a glossy ground plane on its own layer reflects the
// scene through a mirrored camera pair — one rendering colour, one rendering
// distance-from-plane as a depth proxy — which the ground shader blurs by height.
// Keys retune blur, intensity, fade, angle fade and height range at runtime.
//
#include <algorithm>
#include <cmath>
#include <memory>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "core/math/matrix4.h"
#include "framework/assets/asset.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1200;
constexpr int WINDOW_HEIGHT = 800;

// Custom layer ID for the ground reflector (excluded from reflection camera).
constexpr int LAYERID_GROUND_REFLECTOR = 100;

class ReflectionPlanarBlurredExample final: public ExampleApp
{
public:
    ReflectionPlanarBlurredExample()
        : ExampleApp({.title = "Blurred Planar Reflection",
                      .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    bool create() override
    {
        scene()->setSkyboxMip(0);
        scene()->setSkyboxIntensity(2.0f);
        scene()->setExposure(1.5f);
        scene()->setToneMapping(TONEMAP_NEUTRAL);

        // Environment atlas for IBL (matches upstream reflection-planar-blurred).
        _envAtlas = std::make_unique<Asset>(
            "morning-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/morning-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _statueAsset = std::make_unique<Asset>(
            "sunglasses", AssetType::CONTAINER, assetPath("models/SunglassesKhronos.glb"));

        const auto envAtlasResource = _envAtlas->resource();
        if (!envAtlasResource) {
            spdlog::error("Failed to load environment atlas texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));

        // -----------------------------------------------------------------------
        // Layer composition: World + GroundReflector (excluded from reflection).
        // -----------------------------------------------------------------------
        auto groundLayer = std::make_shared<Layer>("GroundReflector", LAYERID_GROUND_REFLECTOR);
        scene()->layers()->pushOpaque(groundLayer);
        scene()->layers()->pushTransparent(groundLayer);

        // -----------------------------------------------------------------------
        // Scene objects — materials and primitives in the World layer.
        // -----------------------------------------------------------------------

        // Red metallic sphere.
        _redMaterial = std::make_shared<StandardMaterial>();
        _redMaterial->setDiffuse(Color(0.9f, 0.15f, 0.1f, 1.0f));
        _redMaterial->setMetalness(0.6f);
        _redMaterial->setGloss(0.8f);

        // Gold metallic box.
        _goldMaterial = std::make_shared<StandardMaterial>();
        _goldMaterial->setDiffuse(Color(1.0f, 0.84f, 0.0f, 1.0f));
        _goldMaterial->setMetalness(0.9f);
        _goldMaterial->setGloss(0.7f);

        // Blue dielectric cylinder.
        _blueMaterial = std::make_shared<StandardMaterial>();
        _blueMaterial->setDiffuse(Color(0.15f, 0.3f, 0.85f, 1.0f));
        _blueMaterial->setMetalness(0.1f);
        _blueMaterial->setGloss(0.9f);

        // White ceramic torus (cone as proxy).
        _whiteMaterial = std::make_shared<StandardMaterial>();
        _whiteMaterial->setDiffuse(Color(0.95f, 0.95f, 0.92f, 1.0f));
        _whiteMaterial->setMetalness(0.0f);
        _whiteMaterial->setGloss(0.85f);

        _sphere = createPrimitive("sphere", _redMaterial.get(),
            Vector3(-1.5f, 1.0f, -0.5f), Vector3(2.0f, 2.0f, 2.0f));
        _box = createPrimitive("box", _goldMaterial.get(),
            Vector3(1.8f, 0.8f, -1.0f), Vector3(1.5f, 1.6f, 1.5f));
        _cone = createPrimitive("cone", _blueMaterial.get(),
            Vector3(0.0f, 1.2f, 1.5f), Vector3(1.6f, 2.4f, 1.6f));
        createPrimitive("cone", _whiteMaterial.get(),
            Vector3(-3.0f, 0.6f, 2.0f), Vector3(1.0f, 1.2f, 1.0f));

        // Load GLB model if available.
        if (const auto statueResource = _statueAsset->resource()) {
            if (auto* modelEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity()) {
                // SunglassesKhronos is authored at real-world scale (~14 cm) — scale up
                // to be the visible hero object in this larger scene.
                modelEntity->setLocalScale(18.0f, 18.0f, 18.0f);
                modelEntity->setLocalPosition(0.0f, 1.6f, 0.0f);
                root()->addChild(modelEntity);
            }
        }

        // -----------------------------------------------------------------------
        // Ground reflector — render component in GroundReflector layer only.
        // -----------------------------------------------------------------------
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.85f, 0.85f, 0.85f, 1.0f));
        _groundMaterial->setMetalness(0.0f);
        _groundMaterial->setGloss(0.95f);   // High gloss for strong reflections.

        createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(20.0f, 1.0f, 20.0f), {LAYERID_GROUND_REFLECTOR});

        // -----------------------------------------------------------------------
        // Reflection depth render target and camera.
        //
        // The depth camera renders the scene (excluding ground) with a special
        // shader that outputs distance-from-reflection-plane as grayscale.
        // Created before color camera so it renders first.
        // -----------------------------------------------------------------------
        TextureOptions depthTexOpts;
        depthTexOpts.name = "ReflectionDepthRT";
        depthTexOpts.width = WINDOW_WIDTH;
        depthTexOpts.height = WINDOW_HEIGHT;
        depthTexOpts.format = PixelFormat::PIXELFORMAT_RGBA8;
        depthTexOpts.mipmaps = false;
        depthTexOpts.minFilter = FilterMode::FILTER_LINEAR;
        depthTexOpts.magFilter = FilterMode::FILTER_LINEAR;
        _reflectionDepthTexture = std::make_shared<Texture>(device().get(), depthTexOpts);

        RenderTargetOptions depthRtOpts;
        depthRtOpts.graphicsDevice = device().get();
        depthRtOpts.colorBuffer = _reflectionDepthTexture.get();
        depthRtOpts.depth = true;
        depthRtOpts.name = "ReflectionDepthRenderTarget";
        auto reflectionDepthRT = device()->createRenderTarget(depthRtOpts);

        // Depth camera: renders World + Skybox with depth pass shader.
        _depthCamEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _depthCamComp = _depthCamEntity->findComponent<CameraComponent>();
        _depthCamComp->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
        _depthCamComp->camera()->setRenderTarget(reflectionDepthRT);
        _depthCamComp->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));  // Black = zero distance
        _depthCamComp->camera()->setPlanarReflectionDepthPass(true);  // Enable depth pass shader

        // -----------------------------------------------------------------------
        // Reflection color render target and camera.
        // -----------------------------------------------------------------------
        TextureOptions reflTexOpts;
        reflTexOpts.name = "ReflectionRT";
        reflTexOpts.width = WINDOW_WIDTH;
        reflTexOpts.height = WINDOW_HEIGHT;
        reflTexOpts.format = PixelFormat::PIXELFORMAT_RGBA8;
        reflTexOpts.mipmaps = false;
        reflTexOpts.minFilter = FilterMode::FILTER_LINEAR;
        reflTexOpts.magFilter = FilterMode::FILTER_LINEAR;
        _reflectionTexture = std::make_shared<Texture>(device().get(), reflTexOpts);

        RenderTargetOptions reflRtOpts;
        reflRtOpts.graphicsDevice = device().get();
        reflRtOpts.colorBuffer = _reflectionTexture.get();
        reflRtOpts.depth = true;
        reflRtOpts.name = "ReflectionRenderTarget";
        auto reflectionRT = device()->createRenderTarget(reflRtOpts);

        // Reflection color camera: renders World + Skybox only (excludes ground layer).
        _reflCamEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _reflCamComp = _reflCamEntity->findComponent<CameraComponent>();
        _reflCamComp->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
        _reflCamComp->camera()->setRenderTarget(reflectionRT);
        _reflCamComp->camera()->setClearColor(Color(0.5f, 0.5f, 0.5f, 1.0f));

        // -----------------------------------------------------------------------
        // Main camera with orbit controls.
        // -----------------------------------------------------------------------
        _cameraEntity = createCamera(Vector3(-2.0f, 3.5f, 8.0f));
        _cameraComp = _cameraEntity->findComponent<CameraComponent>();

        if (_cameraComp && _cameraComp->camera()) {
            _cameraComp->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX,
                                    LAYERID_UI, LAYERID_IMMEDIATE, LAYERID_GROUND_REFLECTOR});
            _cameraComp->camera()->setFov(60.0f);
            _cameraComp->camera()->setNearClip(0.01f);
            _cameraComp->camera()->setFarClip(200.0f);
            _cameraComp->camera()->setClearColor(Color(0.7f, 0.7f, 0.75f, 1.0f));
        }
        _cameraEntity->lookAt(Vector3(0.0f, 0.5f, 0.0f));

        // Orbit camera controls.
        _controls = addOrbitControls(_cameraEntity, Vector3(0.0f, 0.5f, 0.0f));
        _controls->setPitchRange(Vector2(-85.0f, -3.0f));  // Keep above ground.
        _controls->setOrbitDistance(10.0f);
        _controls->setAutoFarClip(true, 10.0f, 200.0f);
        _controls->storeResetState();

        // -----------------------------------------------------------------------
        // Directional light with shadows.
        // -----------------------------------------------------------------------
        auto* light = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.2f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowResolution(2048);
            lightComp->setShadowDistance(50.0f);
            lightComp->setShadowBias(0.2f);
        }

        // -----------------------------------------------------------------------
        // Apply reflection textures to ground material and graphics device.
        // -----------------------------------------------------------------------
        _groundMaterial->setReflectionMap(_reflectionTexture.get());
        device()->setReflectionMap(_reflectionTexture.get());
        device()->setReflectionDepthMap(_reflectionDepthTexture.get());

        // Initialize blur parameters (defaults).
        resetBlurParams();
        device()->setReflectionBlurParams(_blurParams);

        spdlog::info("Controls: B/V blur +/-, I/O intensity +/-, F/G fade +/-, A/S angle +/-, H/J height +/-, M toggle, R reset");
        logBlurParams();

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        // Blur amount: B increase, V decrease.
        case SDLK_B:
            _blurParams.blurAmount = std::min(_blurParams.blurAmount + 0.1f, 2.0f);
            break;
        case SDLK_V:
            _blurParams.blurAmount = std::max(_blurParams.blurAmount - 0.1f, 0.0f);
            break;

        // Intensity: I increase, O decrease.
        case SDLK_I:
            _blurParams.intensity = std::min(_blurParams.intensity + 0.1f, 1.0f);
            break;
        case SDLK_O:
            _blurParams.intensity = std::max(_blurParams.intensity - 0.1f, 0.0f);
            break;

        // Fade strength: F increase, G decrease.
        case SDLK_F:
            _blurParams.fadeStrength = std::min(_blurParams.fadeStrength + 0.2f, 5.0f);
            break;
        case SDLK_G:
            _blurParams.fadeStrength = std::max(_blurParams.fadeStrength - 0.2f, 0.1f);
            break;

        // Angle fade: A increase, S decrease.
        case SDLK_A:
            _blurParams.angleFade = std::min(_blurParams.angleFade + 0.1f, 1.0f);
            break;
        case SDLK_S:
            _blurParams.angleFade = std::max(_blurParams.angleFade - 0.1f, 0.1f);
            break;

        // Height range: H increase, J decrease.
        case SDLK_H:
            _blurParams.heightRange = std::min(_blurParams.heightRange + 1.0f, 50.0f);
            break;
        case SDLK_J:
            _blurParams.heightRange = std::max(_blurParams.heightRange - 1.0f, 1.0f);
            break;

        // Reset to defaults.
        case SDLK_R:
            resetBlurParams();
            device()->setReflectionBlurParams(_blurParams);
            _controls->reset();
            spdlog::info("Parameters reset to defaults");
            logBlurParams();
            return true;

        // Toggle reflection on/off.
        case SDLK_M:
            _reflectionEnabled = !_reflectionEnabled;
            device()->setReflectionMap(_reflectionEnabled ? _reflectionTexture.get() : nullptr);
            device()->setReflectionDepthMap(_reflectionEnabled ? _reflectionDepthTexture.get() : nullptr);
            _groundMaterial->setReflectionMap(_reflectionEnabled ? _reflectionTexture.get() : nullptr);
            spdlog::info("Reflections {}", _reflectionEnabled ? "ON" : "OFF");
            return true;

        default:
            return false;
        }

        device()->setReflectionBlurParams(_blurParams);
        logBlurParams();
        return true;
    }

    void update(const float dt) override
    {
        _time += std::clamp(dt, 0.0f, 0.1f);
        const float time = _time;

        // Animate scene objects for visual interest.
        _sphere->setLocalPosition(-1.5f, 1.0f + 0.3f * std::sin(time * 1.5f), -0.5f);
        _box->setLocalEulerAngles(0.0f, time * 20.0f, 0.0f);
        _cone->setLocalEulerAngles(0.0f, time * -15.0f, 0.0f);

        // -----------------------------------------------------------------------
        // Mirror main camera across ground plane for both reflection cameras.
        //   _reflectionMatrix.setReflection(plane.normal, plane.distance);
        //   reflectionMatrix.transformPoint(mainCameraPos, reflectedPos);
        // -----------------------------------------------------------------------
        if (!_reflectionEnabled) {
            return;
        }

        constexpr float groundY = 0.0f;
        const float planeDistance = -groundY;  // d = -dot(normal, pointOnPlane)
        const Matrix4 reflMatrix = Matrix4::reflection(0.0f, 1.0f, 0.0f, planeDistance);

        const auto& camWorld = _cameraEntity->worldTransform();
        const Vector3 camForward = Vector3(camWorld.getColumn(2)) * -1.0f;
        const Vector3 camPos = _cameraEntity->position();
        const Vector3 camTarget = camPos + camForward;
        const Vector3 reflPos = reflMatrix.transformPoint(camPos);
        const Vector3 reflTarget = reflMatrix.transformPoint(camTarget);

        const Vector3 reflDir = (reflTarget - reflPos).normalized();
        const float pitch = std::asin(std::clamp(reflDir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
        const float yaw = std::atan2(-reflDir.getX(), -reflDir.getZ()) * RAD_TO_DEG;

        // Update color reflection camera.
        _reflCamEntity->setPosition(reflPos);
        _reflCamEntity->setLocalEulerAngles(pitch, yaw, 0.0f);
        _reflCamComp->camera()->setFov(_cameraComp->camera()->fov());
        _reflCamComp->camera()->setNearClip(_cameraComp->camera()->nearClip());
        _reflCamComp->camera()->setFarClip(_cameraComp->camera()->farClip() * 2.0f);
        _reflCamComp->camera()->setAspectRatio(_cameraComp->camera()->aspectRatio());
        _reflCamComp->camera()->setClearColor(_blurParams.fadeColor);

        // Update depth reflection camera (identical transform).
        _depthCamEntity->setPosition(reflPos);
        _depthCamEntity->setLocalEulerAngles(pitch, yaw, 0.0f);
        _depthCamComp->camera()->setFov(_cameraComp->camera()->fov());
        _depthCamComp->camera()->setNearClip(_cameraComp->camera()->nearClip());
        _depthCamComp->camera()->setFarClip(_cameraComp->camera()->farClip() * 2.0f);
        _depthCamComp->camera()->setAspectRatio(_cameraComp->camera()->aspectRatio());
    }

    void destroy() override
    {
        // Release reflection resources before engine destruction.
        device()->setReflectionMap(nullptr);
        device()->setReflectionDepthMap(nullptr);
        _groundMaterial->setReflectionMap(nullptr);
    }

private:
    void resetBlurParams()
    {
        _blurParams.intensity = 1.0f;
        _blurParams.blurAmount = 0.5f;
        _blurParams.fadeStrength = 0.8f;
        _blurParams.angleFade = 0.5f;
        _blurParams.fadeColor = Color(0.5f, 0.5f, 0.5f, 1.0f);
        _blurParams.planeDistance = 0.0f;   // Ground plane at Y = 0
        _blurParams.heightRange = 10.0f;    // Normalize heights to 0..1 over 10 world units
    }

    void logBlurParams() const
    {
        spdlog::info("Reflection: blur={:.2f} intensity={:.2f} fade={:.2f} angle={:.2f} height={:.1f}",
            _blurParams.blurAmount, _blurParams.intensity,
            _blurParams.fadeStrength, _blurParams.angleFade, _blurParams.heightRange);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _statueAsset;

    std::shared_ptr<StandardMaterial> _redMaterial;
    std::shared_ptr<StandardMaterial> _goldMaterial;
    std::shared_ptr<StandardMaterial> _blueMaterial;
    std::shared_ptr<StandardMaterial> _whiteMaterial;
    std::shared_ptr<StandardMaterial> _groundMaterial;

    std::shared_ptr<Texture> _reflectionTexture;
    std::shared_ptr<Texture> _reflectionDepthTexture;

    Entity* _sphere = nullptr;
    Entity* _box = nullptr;
    Entity* _cone = nullptr;
    Entity* _cameraEntity = nullptr;
    Entity* _reflCamEntity = nullptr;
    Entity* _depthCamEntity = nullptr;
    CameraComponent* _cameraComp = nullptr;
    CameraComponent* _reflCamComp = nullptr;
    CameraComponent* _depthCamComp = nullptr;
    CameraControls* _controls = nullptr;

    ReflectionBlurParams _blurParams;
    bool _reflectionEnabled = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ReflectionPlanarBlurredExample)
