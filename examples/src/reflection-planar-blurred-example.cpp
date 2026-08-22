// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of the upstream "graphics/reflection-planar-blurred" example.
//
// The Khronos sunglasses model sits at its authored real-world size (~14 cm) on a
// 4-unit reflective quad, lit by the morning environment atlas alone. A mirrored
// camera pair renders the scene from below the ground plane — one pass for colour,
// one writing per-pixel distance from the plane — and the ground shader uses that
// distance to blur the reflection with height, so contact points stay sharp while
// the raised parts of the frame smear out.
//
// Keys stand in for upstream's slider panel, over the same parameters and ranges:
//   B / V  blur amount     (0 .. 1)
//   I / O  intensity       (0 .. 1)
//   F / G  fade strength   (0.1 .. 5)
//   A / S  angle fade      (0.1 .. 1)
//   H / J  height range    (0.001 .. 1)
//   M      toggle reflections   R  reset to defaults   Esc  quit
//
// DEVIATIONS:
//  - upstream's BlurredPlanarReflection script REPLACES the ground plane's material
//    with a dedicated ShaderMaterial reflection quad. This engine implements planar
//    reflection as a StandardMaterial feature instead (VT_FEATURE bits 33/34, driven
//    by setReflectionMap plus GraphicsDevice::setReflectionBlurParams), so the ground
//    here stays a StandardMaterial. Same technique, different plumbing.
//  - upstream inserts the excluded layer's opaque and transparent sublayers at two
//    different positions; LayerComposition::insert places both at one index. The
//    excluded layer holds only the opaque ground, so the orders coincide.
//  - upstream's `resolution` slider rescales the reflection render targets live; here
//    they are created once at the device's backbuffer size (upstream's default 1.0).
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

// Layer holding the ground reflector, excluded from the reflection cameras
// (upstream calls it "Excluded").
constexpr int LAYERID_EXCLUDED = 100;

class ReflectionPlanarBlurredExample final: public ExampleApp
{
public:
    ReflectionPlanarBlurredExample()
        : ExampleApp({.title = "Blurred Planar Reflection", .width = 1200, .height = 800}) {}

protected:
    bool create() override
    {
        // Upstream sets only the environment atlas and skybox intensity; tone mapping
        // is a CAMERA setting there, and exposure/skyboxMip stay at their defaults.
        scene()->setSkyboxIntensity(2.0f);

        _envAtlas = std::make_unique<Asset>(
            "morning-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/morning-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _sunglassesAsset = std::make_unique<Asset>(
            "sunglasses", AssetType::CONTAINER, assetPath("models/SunglassesKhronos.glb"));

        const auto envAtlasResource = _envAtlas->resource();
        if (!envAtlasResource) {
            spdlog::error("Failed to load environment atlas texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));

        // Layer order upstream asks for:
        //   World(opaque) -> Excluded(opaque) -> Depth -> World(transp) -> Excluded(transp)
        const auto layers = scene()->layers();
        const auto worldLayer = layers->getLayerById(LAYERID_WORLD);
        if (!worldLayer) {
            spdlog::error("World layer missing from the default composition");
            return false;
        }
        auto excludedLayer = std::make_shared<Layer>("Excluded", LAYERID_EXCLUDED);
        layers->insert(excludedLayer, layers->getOpaqueIndex(worldLayer) + 1);

        // -----------------------------------------------------------------------
        // The hero model, at its authored real-world scale — upstream neither scales
        // nor moves it, and the whole shot is a sub-metre close-up because of that.
        // -----------------------------------------------------------------------
        const auto sunglassesResource = _sunglassesAsset->resource();
        if (!sunglassesResource) {
            spdlog::error("Failed to load models/SunglassesKhronos.glb");
            return false;
        }
        auto* sunglasses = std::get<ContainerResource*>(*sunglassesResource)->instantiateRenderEntity();
        sunglasses->setEngine(engine());
        root()->addChild(sunglasses);

        // -----------------------------------------------------------------------
        // Ground reflector — a 4-unit plane alone on the excluded layer.
        // -----------------------------------------------------------------------
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _groundMaterial->setMetalness(0.0f);
        _groundMaterial->setGloss(0.95f);

        auto* ground = createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(4.0f, 1.0f, 4.0f), {LAYERID_EXCLUDED});
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }

        // -----------------------------------------------------------------------
        // Reflection render targets, sized to the backbuffer (upstream resolution 1.0).
        // Both reflection cameras are created BEFORE the main camera: this engine has
        // no camera priority (upstream uses -2 / -1 / 0), and layer composition renders
        // cameras in construction order — built after, the main camera would sample the
        // previous frame's reflection maps.
        // -----------------------------------------------------------------------
        const auto [deviceWidth, deviceHeight] = device()->size();
        const int rtWidth = std::max(1, deviceWidth);
        const int rtHeight = std::max(1, deviceHeight);

        _reflectionDepthTexture = createReflectionTexture("ReflectionDepthRT", rtWidth, rtHeight);
        auto reflectionDepthRT = createReflectionTarget("ReflectionDepthRenderTarget",
            _reflectionDepthTexture.get());

        // Depth camera: renders distance-from-plane as greyscale for the blur radius.
        _depthCamEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _depthCamComp = _depthCamEntity->findComponent<CameraComponent>();
        _depthCamComp->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_UI});
        _depthCamComp->camera()->setRenderTarget(reflectionDepthRT);
        _depthCamComp->camera()->setPlanarReflectionDepthPass(true);

        _reflectionTexture = createReflectionTexture("ReflectionRT", rtWidth, rtHeight);
        auto reflectionRT = createReflectionTarget("ReflectionRenderTarget", _reflectionTexture.get());

        // Colour camera: the main camera's layers minus the excluded ground and minus
        // the skybox — upstream clears to the fade colour instead of drawing the sky.
        _reflCamEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _reflCamComp = _reflCamEntity->findComponent<CameraComponent>();
        _reflCamComp->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_UI});
        _reflCamComp->camera()->setRenderTarget(reflectionRT);

        // -----------------------------------------------------------------------
        // Main camera. Upstream: fov 60, nearClip 0.01, white clear, NEUTRAL tone
        // mapping, and no Skybox layer — the background is the clear colour.
        // -----------------------------------------------------------------------
        _cameraEntity = createCamera(Vector3(-0.2f, 0.1f, 0.2f));
        _cameraComp = _cameraEntity->findComponent<CameraComponent>();
        if (_cameraComp && _cameraComp->camera()) {
            _cameraComp->setLayers({LAYERID_WORLD, LAYERID_EXCLUDED, LAYERID_DEPTH, LAYERID_UI});
            _cameraComp->camera()->setFov(60.0f);
            _cameraComp->camera()->setNearClip(0.01f);
            _cameraComp->camera()->setClearColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            _cameraComp->setToneMapping(TONEMAP_NEUTRAL);

            // The sunglasses' lenses use transmission, which reads the mid-frame
            // scene colour grab — without this the glass renders wrong.
            _cameraComp->requestSceneColorMap(true);
        }

        _controls = addOrbitControls(_cameraEntity, Vector3(0.0f, 0.02f, 0.0f));
        _controls->setPitchRange(Vector2(-85.0f, -4.0f));   // keep the camera above ground
        _controls->setZoomRange(Vector2(0.1f, 1.0f));
        _controls->storeResetState();

        // -----------------------------------------------------------------------
        // Hand the reflection maps to the ground material and the device.
        // -----------------------------------------------------------------------
        _groundMaterial->setReflectionMap(_reflectionTexture.get());
        device()->setReflectionMap(_reflectionTexture.get());
        device()->setReflectionDepthMap(_reflectionDepthTexture.get());

        resetBlurParams();
        device()->setReflectionBlurParams(_blurParams);

        spdlog::info("Blurred planar reflection (upstream graphics/reflection-planar-blurred).");
        spdlog::info("Keys: B/V blur, I/O intensity, F/G fade, A/S angle, H/J height, M toggle, R reset");
        logBlurParams();

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        // Steps and clamps mirror upstream's slider ranges.
        switch (event.key.key) {
        case SDLK_B:
            _blurParams.blurAmount = std::min(_blurParams.blurAmount + 0.05f, 1.0f);
            break;
        case SDLK_V:
            _blurParams.blurAmount = std::max(_blurParams.blurAmount - 0.05f, 0.0f);
            break;
        case SDLK_I:
            _blurParams.intensity = std::min(_blurParams.intensity + 0.05f, 1.0f);
            break;
        case SDLK_O:
            _blurParams.intensity = std::max(_blurParams.intensity - 0.05f, 0.0f);
            break;
        case SDLK_F:
            _blurParams.fadeStrength = std::min(_blurParams.fadeStrength + 0.2f, 5.0f);
            break;
        case SDLK_G:
            _blurParams.fadeStrength = std::max(_blurParams.fadeStrength - 0.2f, 0.1f);
            break;
        case SDLK_A:
            _blurParams.angleFade = std::min(_blurParams.angleFade + 0.05f, 1.0f);
            break;
        case SDLK_S:
            _blurParams.angleFade = std::max(_blurParams.angleFade - 0.05f, 0.1f);
            break;
        case SDLK_H:
            _blurParams.heightRange = std::min(_blurParams.heightRange + 0.01f, 1.0f);
            break;
        case SDLK_J:
            _blurParams.heightRange = std::max(_blurParams.heightRange - 0.01f, 0.001f);
            break;

        case SDLK_R:
            resetBlurParams();
            device()->setReflectionBlurParams(_blurParams);
            _controls->reset();
            spdlog::info("Parameters reset to defaults");
            logBlurParams();
            return true;

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

    void update(float) override
    {
        if (!_reflectionEnabled) {
            return;
        }

        // Mirror the main camera across the ground plane for both reflection cameras
        // (upstream BlurredPlanarReflection::postUpdate):
        //   _reflectionMatrix.setReflection(plane.normal, plane.distance);
        //   reflectionMatrix.transformPoint(mainCameraPos, reflectedPos);
        constexpr float groundY = 0.0f;
        const float planeDistance = -groundY;   // d = -dot(normal, pointOnPlane)
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

        // Upstream runs the same _updateReflectionCamera over both cameras, so both
        // track the main camera's projection and both clear to the fade colour.
        syncReflectionCamera(_reflCamEntity, _reflCamComp, reflPos, pitch, yaw);
        syncReflectionCamera(_depthCamEntity, _depthCamComp, reflPos, pitch, yaw);
    }

    void destroy() override
    {
        // Release reflection resources before the engine goes.
        device()->setReflectionMap(nullptr);
        device()->setReflectionDepthMap(nullptr);
        _groundMaterial->setReflectionMap(nullptr);
    }

private:
    std::shared_ptr<Texture> createReflectionTexture(const char* name, int width, int height) const
    {
        TextureOptions options;
        options.name = name;
        options.width = width;
        options.height = height;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        return std::make_shared<Texture>(device().get(), options);
    }

    std::shared_ptr<RenderTarget> createReflectionTarget(const char* name, Texture* colorBuffer) const
    {
        RenderTargetOptions options;
        options.graphicsDevice = device().get();
        options.colorBuffer = colorBuffer;
        options.depth = true;
        options.name = name;
        return device()->createRenderTarget(options);
    }

    // Both reflection cameras track the main camera's projection; the colour one
    // doubles the far clip, as upstream does.
    void syncReflectionCamera(Entity* entity, CameraComponent* component,
        const Vector3& position, const float pitch, const float yaw) const
    {
        entity->setPosition(position);
        entity->setLocalEulerAngles(pitch, yaw, 0.0f);
        component->camera()->setFov(_cameraComp->camera()->fov());
        component->camera()->setNearClip(_cameraComp->camera()->nearClip());
        component->camera()->setFarClip(_cameraComp->camera()->farClip() * 2.0f);
        component->camera()->setAspectRatio(_cameraComp->camera()->aspectRatio());
        component->camera()->setClearColor(_blurParams.fadeColor);
    }

    // Upstream's initial script values.
    void resetBlurParams()
    {
        _blurParams.intensity = 1.0f;
        _blurParams.blurAmount = 0.5f;
        _blurParams.fadeStrength = 0.8f;
        _blurParams.angleFade = 0.5f;
        _blurParams.fadeColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
        _blurParams.planeDistance = 0.0f;   // ground plane at y = 0
        _blurParams.heightRange = 0.07f;    // metres — the model is only ~14 cm tall
    }

    void logBlurParams() const
    {
        spdlog::info("Reflection: blur={:.2f} intensity={:.2f} fade={:.2f} angle={:.2f} height={:.3f}",
            _blurParams.blurAmount, _blurParams.intensity,
            _blurParams.fadeStrength, _blurParams.angleFade, _blurParams.heightRange);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _sunglassesAsset;

    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::shared_ptr<Texture> _reflectionTexture;
    std::shared_ptr<Texture> _reflectionDepthTexture;

    Entity* _cameraEntity = nullptr;
    Entity* _reflCamEntity = nullptr;
    Entity* _depthCamEntity = nullptr;
    CameraComponent* _cameraComp = nullptr;
    CameraComponent* _reflCamComp = nullptr;
    CameraComponent* _depthCamComp = nullptr;
    CameraControls* _controls = nullptr;

    ReflectionBlurParams _blurParams;
    bool _reflectionEnabled = true;
};

VISUTWIN_EXAMPLE_MAIN(ReflectionPlanarBlurredExample)
