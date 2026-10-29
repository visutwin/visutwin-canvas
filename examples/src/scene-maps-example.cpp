// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// Port of upstream compute/scene-maps.
//
// A compute shader reads the scene depth and colour maps of the camera to draw a scanner
// pulse sweeping through the scene: from the depth it reconstructs the world position of each
// pixel, and the pulse lighting up the surfaces it passes, with a fading world-space grid
// behind it, takes on the hue of the scene colour under it. The laboratory is lit by the
// helipad env atlas, its torches and a shadow-casting sun, with eight characters walking
// around it in both directions. The kernel runs in a ComputePass among the camera's after
// passes, so it reads the maps of the frame the camera just rendered, and draws into a
// half-resolution overlay that a second camera adds over the final image. The camera orbits
// the laboratory's bounds centre (the origin) from (-233.33, 116.67, 233.33), out to 350.
//
// The camera renders its scene maps either through the forward renderer, which renders them
// on request (the depth copied from the depth buffer, the colour gamma-encoded), or through a
// camera frame (the colour linear; the depth from a prepass when the depth map is asked for, or
// written by the scene pass when TAA needs it). The kernel reads them the same way in all of
// these.
//
// Keys (upstream's control panel):
//   1          renderer: forward / camera frame
//   2          depth (camera frame): prepass / scene pass (TAA)
//   3          MSAA (camera frame)
//   Up / Down  scanner speed (10 - 150)
//   [ / ]      band width (0.5 - 10)
//   , / .      grid size (1 - 16)
//
// Asset credit (laboratory.glb): "Laboratory" from Sketchfab, CC BY 4.0 — see
// assets/models/laboratory.txt.
//
// DEVIATIONS:
// - The scanner is Slang (one source for both backends), compiled at run time
//   (getOrCreateSlangShaderFromSource); it imports the engine's `vtscenemaps` module where
//   upstream's WGSL includes the sceneDepthCS and sceneColorCS chunks. Its uniforms are one
//   block (Compute::setUniformBlock), as this port binds compute resources by name order.
// - The control panel is the keys above, and upstream's scene format choice (RG11B10F,
//   RGBA16F, RGBA32F) is left out: the camera frame's scene target is always RGBA16F.
// - Upstream's camera frame is enabled with nothing else on; here that is
//   RenderingSettings::useCameraFrame, and the frame's scene maps come from the camera's
//   requests (requestSceneColorMap / requestSceneDepthMap), as everything else's do.
// - The back buffer is one pixel per point, as every example here; upstream caps the pixel
//   ratio at 2.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/computePass.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int LAYERID_SCANNER_OVERLAY = 23;

    // A world scanner: a pulse expands through the scene from an origin, lighting up the
    // surfaces it passes and leaving a fading grid of world-space lines behind it. The world
    // position of each pixel is reconstructed from the scene depth map, and the scene colour
    // map tints the pulse with the colour of the surfaces it sweeps over.
    //
    // Bindings in Compute's name order: the texture "overlay" (Metal texture 0, Vulkan binding
    // 0) and the uniform block after it (Metal buffer 0, Vulkan binding 1). The scene maps are
    // the module's, at their own slots.
    constexpr const char* kScannerSlang = R"slang(
import vtscenemaps;

struct ScannerParams
{
    float4 originRadius;        // xyz: where the pulse is emitted, w: its radius
    float4 scanColorBandWidth;  // xyz: the colour of the pulse, w: the width of its wavefront
    float trailLength;
    float gridSize;
    // the world size of an overlay pixel at the distance of one unit from the camera
    float pixelSize;
    float pad;
};

// the overlay the scanner draws into, added over the final image
[[vk::binding(0, 0)]] [format("rgba8")] RWTexture2D<float4> overlay : register(u0);
[[vk::binding(1, 0)]] ConstantBuffer<ScannerParams> params : register(b0);

[shader("compute")]
[numthreads(8, 8, 1)]
void scannerMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    overlay.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) {
        return;
    }

    // the overlay can be of a lower resolution than the scene maps
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    const int2 texel = int2(uv * float2(sceneDepthSize()));

    // nothing to scan where the sky is
    const float depth = sceneDepthLinear(texel);
    if (depth > sceneDepthFarClip() * 0.99) {
        overlay[id.xy] = float4(0.0);
        return;
    }

    const float3 position = sceneDepthWorldPosition(texel);

    // positive behind the wavefront, where the pulse has already passed
    const float behind = params.originRadius.w - distance(position, params.originRadius.xyz);

    // the bright wavefront, and the trail fading out behind it
    const float front = exp(-abs(behind) / params.scanColorBandWidth.w);
    const float trail = behind > 0.0 ? exp(-behind / params.trailLength) : 0.0;

    // lines where the surface crosses the planes of a world-space grid, at least a pixel and a
    // half wide on the screen
    const float3 cell = abs(frac(position / params.gridSize + 0.5) - 0.5) * params.gridSize;
    const float lineWidth = max(params.gridSize * 0.03, depth * params.pixelSize * 1.5);
    const float line = 1.0 - smoothstep(0.0, lineWidth, min(cell.x, min(cell.y, cell.z)));

    // the pulse takes on the hue of the surfaces it sweeps over
    const float3 surface = sceneColorToLinear(sceneColorSample(uv, 0.0).rgb);
    const float3 hue = surface / max(max(surface.r, max(surface.g, surface.b)), 0.001);
    const float3 tint = params.scanColorBandWidth.xyz * lerp(float3(1.0), hue, 0.6);

    const float3 glow = tint * (front * 1.6 + trail * (line * 0.9 + 0.06));
    overlay[id.xy] = float4(glow, 1.0);
}
)slang";

    // The overlay: the plane primitive stretched over the whole screen, sampling the texture
    // the scanner drew (bound as the material's base colour map).
    constexpr const char* kOverlaySlang = R"slang(
#include "bindings.slang"

VT_DECLARE_baseColorMap_AS(Sampler2D, overlayTexture)

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Varyings overlayVertex(VertexIn input)
{
    Varyings output;
    // the plane spans -0.5 to 0.5 on the x and z axes, stretched over the whole screen
    output.position = float4(input.position.x * 2.0, -input.position.z * 2.0, 0.5, 1.0);
    output.uv = input.position.xz + 0.5;
    return output;
}

[shader("fragment")]
float4 overlayFragment(Varyings input) : SV_Target0
{
    return overlayTexture.Sample(input.uv);
}
)slang";

    // The scanner's uniform block (ScannerParams above).
    struct ScannerParams
    {
        float originRadius[4];
        float scanColorBandWidth[4];
        float trailLength;
        float gridSize;
        float pixelSize;
        float pad;
    };
    static_assert(sizeof(ScannerParams) == 48);

    struct Walker
    {
        Entity* entity = nullptr;
        AnimComponent* anim = nullptr;
        float angle = 0.0f;
        float direction = 1.0f;
        float radius = 0.0f;
    };
}

class SceneMapsExample final : public ExampleApp
{
public:
    SceneMapsExample()
        : ExampleApp({.title = "Compute Scene Maps", .width = 1200, .height = 800,
            .miniStatsPresets = {"compute"}}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        spdlog::info("*** Compute Scene Maps Example ***");
        if (!device()->supportsCompute()) {
            spdlog::error("scene-maps: the device does not support compute");
            return false;
        }

        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _laboratory = std::make_unique<Asset>("laboratory", AssetType::CONTAINER, assetPath("models/laboratory.glb"));
        _bitmoji = std::make_unique<Asset>("bitmoji", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _walk = std::make_unique<Asset>("walk", AssetType::CONTAINER, assetPath("animations/bitmoji/walk.glb"));

        // Setup skydome
        Texture* envAtlas = _envAtlas->resourceAs<Texture>();
        auto* labContainer = _laboratory->resourceAs<ContainerResource>();
        auto* bitmojiContainer = _bitmoji->resourceAs<ContainerResource>();
        auto* walkContainer = dynamic_cast<GlbContainerResource*>(_walk->resourceAs<ContainerResource>());
        if (!envAtlas || !labContainer || !bitmojiContainer || !walkContainer || walkContainer->animTracks().empty()) {
            spdlog::error("scene-maps: an asset failed to load");
            return false;
        }
        scene()->setEnvAtlas(envAtlas);
        scene()->setSkyboxMip(2);
        scene()->setExposure(2.5f);

        // ------ The scene ------

        auto* laboratory = labContainer->instantiateRenderEntity();
        laboratory->setLocalScale(100.0f, 100.0f, 100.0f);
        root()->addChild(laboratory);
        for (auto* render : laboratory->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
            render->setReceiveShadows(true);
        }

        // Warm lights at the torches of the laboratory
        for (auto* torch : laboratory->find([](GraphNode* node) {
                 return node->name().find("Fackel") != std::string::npos; })) {
            auto* light = new Entity();
            light->setEngine(engine());
            light->setName("Torch");
            if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
                lightComponent->setType(LightType::LIGHTTYPE_OMNI);
                lightComponent->setColor(Color(1.0f, 0.75f, 0.0f));
                lightComponent->setIntensity(3.0f);
                lightComponent->setRange(100.0f);
            }
            if (!torch->children().empty()) {
                auto* childRender = static_cast<Entity*>(torch->children()[0].get())->findComponent<RenderComponent>();
                if (childRender && !childRender->meshInstances().empty()) {
                    light->setLocalPosition(childRender->meshInstances()[0]->aabb().center());
                }
            }
            root()->addChild(light);
        }

        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.2f, 0.2f, 0.2f));
        auto* ground = createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, -40.0f, 0.0f),
            Vector3(400.0f, 1.0f, 400.0f));
        ground->setName("Ground");

        auto* sun = createDirectionalLight(Vector3(35.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* sunLight = sun->findComponent<LightComponent>()) {
            sunLight->setShadowResolution(2048);
            sunLight->setShadowBias(0.4f);
            sunLight->setShadowNormalBias(0.06f);
            sunLight->setShadowDistance(600.0f);
        }

        // The pulse is emitted from the floor in the middle of the laboratory, and travels past
        // its walls
        const BoundingBox bounds = entityBounds(laboratory);
        _origin = Vector3(bounds.center().getX(), bounds.center().getY() - bounds.halfExtents().getY(),
            bounds.center().getZ());
        _maxRadius = bounds.halfExtents().length() * 2.5f;

        // Characters walking around the laboratory in both directions. They render into the
        // scene maps like the rest of the scene, so the pulse sweeps over them as they move.
        constexpr int walkerCount = 8;
        _walkerHeight = ground->position().getY();
        for (int i = 0; i < walkerCount; ++i) {
            auto* walker = bitmojiContainer->instantiateRenderEntity();
            walker->setEngine(engine());
            walker->setLocalScale(20.0f, 20.0f, 20.0f);
            for (auto* render : walker->findComponents<RenderComponent>()) {
                render->setCastShadows(true);
                render->setReceiveShadows(true);
            }
            // DEVIATION: the anim component is state-graph driven, so the single looping clip
            // is a one-state graph rather than a bare assignAnimation call.
            if (auto* legacyAnim = walker->findComponent<AnimationComponent>()) {
                legacyAnim->setPlaying(false);
                legacyAnim->setEnabled(false);
            }
            AnimStateGraph stateGraph;
            auto& animLayer = stateGraph.addLayer("locomotion");
            animLayer.states.push_back(AnimStateDesc{"Walk"});
            animLayer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Walk"});
            auto* anim = static_cast<AnimComponent*>(walker->addComponent<AnimComponent>());
            anim->loadStateGraph(stateGraph);
            anim->assignAnimation("Walk", walkContainer->animTracks().begin()->second);
            anim->setSpeed(0.9f + static_cast<float>(i % 3) * 0.1f);
            root()->addChild(walker);

            _walkers.push_back({walker, anim,
                static_cast<float>(i) / walkerCount * 2.0f * std::numbers::pi_v<float>,
                (i % 2) ? 1.0f : -1.0f, 125.0f + static_cast<float>(i % 2) * 20.0f});
        }

        auto* cameraEntity = createCamera(Vector3(-60.0f, 30.0f, 60.0f));
        cameraEntity->setName("Camera");
        _camera = cameraEntity->findComponent<CameraComponent>();
        _camera->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        _camera->camera()->setNearClip(1.0f);
        _camera->camera()->setFarClip(600.0f);
        _camera->setToneMapping(TONEMAP_NEUTRAL);

        // The camera frames the laboratory: 1.5 x its largest half extent over sin(fov / 2),
        // clamped to 350.
        const Vector3 focus = bounds.center();
        const auto& he = bounds.halfExtents();
        const float fovRad = _camera->camera()->fov() * (std::numbers::pi_v<float> / 180.0f);
        const float distance = std::min(std::max({he.getX(), he.getY(), he.getZ()}) * 1.5f /
            std::sin(0.5f * fovRad), 350.0f);
        if (auto* controls = addOrbitControls(cameraEntity, focus)) {
            const float sceneRadius = std::max(he.length(), 1.0f);
            controls->setMoveSpeed(2 * sceneRadius);
            controls->setMoveFastSpeed(4 * sceneRadius);
            controls->setMoveSlowSpeed(sceneRadius);
            controls->setOrbitDistance(distance);
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.01f, 350.0f));
            controls->storeResetState();
        }

        // ------ The scanner ------

        // A compute shader reconstructing the world position of each pixel from the scene depth
        // map, and drawing the scanner pulse into an overlay texture, tinted by the scene colour
        // map.
        auto shader = getOrCreateSlangShaderFromSource(device().get(), "scene-maps-scanner", kScannerSlang);
        if (!shader) {
            spdlog::error("scene-maps: the scanner did not compile");
            return false;
        }
        _compute = std::make_unique<Compute>(device().get(), shader, "Scanner");

        // The scene maps of the camera are attached once - each dispatch then reads the maps
        // the camera rendered most recently, decoded however the camera stored them
        _compute->setSceneDepthMap(_camera->sceneDepthMapHandle());
        _compute->setSceneColorMap(_camera->sceneColorMapHandle());

        // The scanner runs in a compute pass after the main camera renders, so it scans the
        // scene maps of the frame the camera rendered, and the overlay drawn after it matches
        // the scene
        _scannerPass = std::make_shared<ComputePass>(device(), std::vector<Compute*>{_compute.get()});
        _scannerPass->setName("ScannerPass");
        _camera->afterPasses().push_back(_scannerPass);

        // The overlay is added over the final image by a camera rendering after the main one,
        // so that it is drawn the same way whether the main camera uses the camera frame or not
        _overlayLayer = std::make_shared<Layer>("ScannerOverlay", LAYERID_SCANNER_OVERLAY);
        scene()->layers()->pushOpaque(_overlayLayer);
        scene()->layers()->pushTransparent(_overlayLayer);

        auto overlayShader = getOrCreateSlangShaderFromSource(device().get(), "scene-maps-overlay", kOverlaySlang);
        if (!overlayShader) {
            spdlog::error("scene-maps: the overlay shader did not compile");
            return false;
        }
        _overlayMaterial = std::make_shared<ShaderMaterial>("ScannerOverlay", overlayShader);
        // upstream's BLEND_ADDITIVE: one times the source plus one times the destination
        auto additive = std::make_shared<BlendState>();
        additive->setEnabled(true);
        additive->setColorOp(BLENDEQUATION_ADD);
        additive->setColorSrcFactor(BLENDMODE_ONE);
        additive->setColorDstFactor(BLENDMODE_ONE);
        additive->setAlphaOp(BLENDEQUATION_ADD);
        additive->setAlphaSrcFactor(BLENDMODE_ONE);
        additive->setAlphaDstFactor(BLENDMODE_ONE);
        _overlayMaterial->setBlendState(additive);
        _overlayMaterial->setTransparent(true);
        auto noDepth = std::make_shared<DepthState>(DepthState::noWrite());
        noDepth->setDepthTest(false);
        _overlayMaterial->setDepthState(noDepth);
        _overlayMaterial->setCullMode(CullMode::CULLFACE_NONE);

        auto* overlayQuad = createPrimitive("plane", _overlayMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(1.0f, 1.0f, 1.0f), {LAYERID_SCANNER_OVERLAY});
        overlayQuad->setName("ScannerOverlay");
        if (auto* render = overlayQuad->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
            render->setReceiveShadows(false);
            for (auto* meshInstance : render->meshInstances()) {
                meshInstance->setCull(false);
            }
        }

        auto* overlayCameraEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        overlayCameraEntity->setName("OverlayCamera");
        auto* overlayCamera = overlayCameraEntity->findComponent<CameraComponent>();
        overlayCamera->camera()->setClearColorBuffer(false);
        overlayCamera->camera()->setClearDepthBuffer(false);
        overlayCamera->camera()->setClearStencilBuffer(false);
        overlayCamera->setPriority(1);
        overlayCamera->setLayers({LAYERID_SCANNER_OVERLAY});

        updateOverlayTexture();

        _camera->requestSceneColorMap(true);
        applyRendering();

        spdlog::info("Keys: 1 renderer (forward / camera frame), 2 depth source (prepass / scene pass TAA), "
            "3 MSAA, Up/Down speed, [/] band width, ,/. grid size");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_1:
            _forward = !_forward;
            break;
        case SDLK_2:
            _scenePassDepth = !_scenePassDepth;
            break;
        case SDLK_3:
            _msaa = !_msaa;
            break;
        case SDLK_UP:
            _speed = std::min(_speed + 10.0f, 150.0f);
            break;
        case SDLK_DOWN:
            _speed = std::max(_speed - 10.0f, 10.0f);
            break;
        case SDLK_RIGHTBRACKET:
            _bandWidth = std::min(_bandWidth + 0.5f, 10.0f);
            break;
        case SDLK_LEFTBRACKET:
            _bandWidth = std::max(_bandWidth - 0.5f, 0.5f);
            break;
        case SDLK_PERIOD:
            _gridSize = std::min(_gridSize + 1.0f, 16.0f);
            break;
        case SDLK_COMMA:
            _gridSize = std::max(_gridSize - 1.0f, 1.0f);
            break;
        default:
            return false;
        }
        applyRendering();
        spdlog::info("scene-maps: renderer {}, depth {}, MSAA {}, speed {}, band width {}, grid size {}",
            _forward ? "forward" : "camera frame", _scenePassDepth ? "scene pass (TAA)" : "prepass",
            _msaa ? "on" : "off", _speed, _bandWidth, _gridSize);
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        updateOverlayTexture();

        // the pulse expands from the origin, and is emitted again once it has passed the
        // laboratory
        const float radius = std::fmod(_time * _speed, _maxRadius);

        ScannerParams params{};
        params.originRadius[0] = _origin.getX();
        params.originRadius[1] = _origin.getY();
        params.originRadius[2] = _origin.getZ();
        params.originRadius[3] = radius;
        params.scanColorBandWidth[0] = 0.2f;
        params.scanColorBandWidth[1] = 0.75f;
        params.scanColorBandWidth[2] = 1.0f;
        params.scanColorBandWidth[3] = _bandWidth;
        params.trailLength = _maxRadius * 0.15f;
        params.gridSize = _gridSize;

        // the world size of an overlay pixel one unit from the camera, to keep the grid lines
        // visible
        const float fov = _camera->camera()->fov() * (std::numbers::pi_v<float> / 180.0f);
        params.pixelSize = 2.0f * std::tan(fov * 0.5f) / static_cast<float>(_overlayTexture->height());
        _compute->setUniformBlock(&params, sizeof(params));

        // the size of the dispatch, a thread for each overlay pixel, which the scanner pass uses
        // after the camera renders
        _compute->setupDispatchThreads(_overlayTexture->width(), _overlayTexture->height());

        for (auto& walker : _walkers) {
            // walk along a circle around the laboratory, at a speed roughly matching the animation
            const float angularSpeed = 14.0f * walker.anim->speed() / walker.radius;
            walker.angle += walker.direction * angularSpeed * dt;
            const Vector3 position(_origin.getX() + std::sin(walker.angle) * walker.radius, _walkerHeight,
                _origin.getZ() + std::cos(walker.angle) * walker.radius);
            walker.entity->setPosition(position);

            // the model faces +Z, and lookAt points -Z at the target, so look at the point behind
            const float behind = walker.angle - walker.direction * 0.1f;
            walker.entity->lookAt(Vector3(_origin.getX() + std::sin(behind) * walker.radius, position.getY(),
                _origin.getZ() + std::cos(behind) * walker.radius));
        }
    }

    void destroy() override
    {
        // The pass holds the compute by a raw pointer.
        if (_camera) {
            std::erase(_camera->afterPasses(), _scannerPass);
        }
        _scannerPass.reset();
        _compute.reset();
        spdlog::info("*** Compute Scene Maps Example Finished ***");
    }

private:
    // The overlay is rendered at half the resolution of the screen, and upscaled when drawn
    void updateOverlayTexture()
    {
        const auto [deviceWidth, deviceHeight] = device()->size();
        const auto width = static_cast<uint32_t>(std::max(1, deviceWidth / 2));
        const auto height = static_cast<uint32_t>(std::max(1, deviceHeight / 2));
        if (_overlayTexture && _overlayTexture->width() == width && _overlayTexture->height() == height) {
            return;
        }
        TextureOptions options;
        options.name = "ScannerOverlay";
        options.width = width;
        options.height = height;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        options.storage = true;
        _overlayTexture = std::make_shared<Texture>(device().get(), options);
        _overlayTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _overlayTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _overlayTexture->upload();
        _compute->setParameter("overlay", _overlayTexture.get());
        _overlayMaterial->setBaseColorTexture(_overlayTexture.get());
    }

    // The way the camera renders its scene maps
    void applyRendering()
    {
        // The forward renderer renders the scene maps on request: the depth by copying the
        // depth buffer's values, and the colour gamma-encoded. The camera frame renders the
        // colour linear; it renders the depth in a prepass when asked for the depth map, and as
        // part of the scene pass when an effect after it needs the depth, here TAA.
        const bool depthMap = _forward || !_scenePassDepth;
        if (depthMap != _depthMapRequested) {
            _camera->requestSceneDepthMap(depthMap);
            _depthMapRequested = depthMap;
        }
        auto rendering = _camera->rendering();
        rendering.useCameraFrame = !_forward;
        rendering.samples = !_forward && _msaa ? 4 : 1;
        _camera->setRendering(rendering);
        _camera->setTaaEnabled(!_forward && _scenePassDepth);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _laboratory;
    std::unique_ptr<Asset> _bitmoji;
    std::unique_ptr<Asset> _walk;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::shared_ptr<ShaderMaterial> _overlayMaterial;
    std::shared_ptr<Layer> _overlayLayer;
    std::shared_ptr<Texture> _overlayTexture;
    std::unique_ptr<Compute> _compute;
    std::shared_ptr<ComputePass> _scannerPass;
    CameraComponent* _camera = nullptr;
    std::vector<Walker> _walkers;

    Vector3 _origin;
    float _maxRadius = 1.0f;
    float _walkerHeight = 0.0f;
    float _time = 0.0f;

    bool _forward = true;
    bool _scenePassDepth = false;
    bool _msaa = true;
    bool _depthMapRequested = false;
    float _speed = 60.0f;
    float _bandWidth = 3.0f;
    float _gridSize = 2.0f;
};

VISUTWIN_EXAMPLE_MAIN(SceneMapsExample)
