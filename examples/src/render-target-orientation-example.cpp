// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream test/render-target-orientation (a hidden test example).
//
// A visual test of how images are oriented through every path that produces a texture. Each
// tile is a plane sampling its texture with the plane's own UVs; the 'upload' tile (the channels
// texture as loaded) is the control, so a tile that differs from it does so because of the path it
// exercises. Here a texture's row 0 is the top of the image on both backends, for loaded images
// and render targets alike, so every tile has ONE expectation, the same on Metal and Vulkan, which
// its label states:
//   upload               upright (the control)
//   camera RT            upright: a camera renders a plane showing the texture into a target
//   cube face +X         upright: a camera renders into face +X of a cubemap, shown through the
//                        standard face mapping (s from -z, t from -y)
//   quad copy            upright: a quad pass copies the camera target with the engine's quad uv
//   rect blit            cell at bottom: a quad pass draws the texture into the viewport rect
//                        (64, 128, 128, 128), measured from the target's top row
//   uv-space write       upright: a plane drawn in its UV space (clip y = 1 - 2 v) into a target
//   readback camera RT   yellow: texel (32, 32) read back from the camera target, where the
//                        yellow marker at the source's top-left lands with row 0 at the top
// The camera targets are 256 x 256 and clear to (0.1, 0.1, 0.25); the main camera is orthographic
// (height 6.6) and clears to (0.13, 0.13, 0.13).
//
// DEVIATIONS:
// - There is one origin, so the tiles of render targets with a bottom origin, of uploads with
//   flipY and of raw-uv quads, which upstream shows to differ between WebGL2 and WebGPU, have no
//   counterpart; nor does the grid's second group.
// - The cube face is first sampled into a 2D texture by a quad pass (the face mapping above), which
//   the tile shows, where upstream samples the cube in the tile's material.
// - The labels are laid out at font size 64 and scaled to upstream's 0.22 units.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/layer.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int LAYERID_RTT = 20;
    constexpr int LAYERID_UV = 21;
    constexpr uint32_t kRtSize = 256;
    constexpr int kLabelFontSize = 64;

    // A texture copied through the engine's quad (uv v = 0 at the top), into the pass's viewport.
    constexpr const char* kCopySlang = R"slang(
import vtquad;
#include "bindings.slang"
VT_DECLARE_QUAD_INPUT0(source)

[shader("vertex")]
QuadVarying copyVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 copyFragment(QuadVarying input) : SV_Target0
{
    return source.SampleLevel(input.uv, 0.0);
}
)slang";

    // Face +X of a cubemap through the standard face mapping: s from -z, t from -y.
    constexpr const char* kCubeFaceSlang = R"slang(
import vtquad;
#include "bindings.slang"
VT_DECLARE_QUAD_CUBE_INPUT0(cube)

[shader("vertex")]
QuadVarying cubeFaceVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 cubeFaceFragment(QuadVarying input) : SV_Target0
{
    const float2 st = input.uv * 2.0 - 1.0;
    return cube.SampleLevel(float3(1.0, -st.y, -st.x), 0.0);
}
)slang";

    // A mesh drawn in its UV space: the clip position is its uv, v = 0 at the top row.
    constexpr const char* kUvWriteSlang = R"slang(
#include "bindings.slang"
VT_DECLARE_BINDLESS_TABLE
VT_DECLARE_MAP_LANES
struct VertexIn
{
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 uv0 : TEXCOORD0;
};

[shader("vertex")]
Varyings uvWriteVertex(VertexIn input)
{
    Varyings output;
    output.uv0 = input.uv0;
    output.position = float4(input.uv0.x * 2.0 - 1.0, 1.0 - input.uv0.y * 2.0, 0.5, 1.0);
    return output;
}

[shader("fragment")]
float4 uvWriteFragment(Varyings input) : SV_Target0
{
    return VT_MAP_SAMPLE(VT_MAP_LANE_baseColorMap, input.uv0);
}
)slang";

    class QuadPass final : public RenderPassShaderQuad
    {
    public:
        QuadPass(const std::shared_ptr<GraphicsDevice>& device, const char* name, const char* source, Texture* input)
            : RenderPassShaderQuad(device), _name(name), _source(source), _input(input) {}

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource(_name, _source);
            }
        }

        void execute() override
        {
            prepareShaders();
            setQuadTextureBinding(0, _input);
            RenderPassShaderQuad::execute();
        }

    private:
        const char* _name;
        const char* _source;
        Texture* _input;
    };
}

class RenderTargetOrientationExample final : public ExampleApp
{
public:
    RenderTargetOrientationExample() : ExampleApp({.title = "Render Target Orientation"}) {}

protected:
    void configure(AppOptions& options) override
    {
        // The labels are world-space text elements, on no screen.
        registerUi(options, {.screen = false});
    }

    bool create() override
    {
        _channelsAsset = std::make_unique<Asset>("channels", AssetType::TEXTURE, assetPath("textures/channels.png"));
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _channels = _channelsAsset->resourceAs<Texture>();
        _font = _fontAsset->resourceAs<FontResource>();
        if (!_channels || !_font) {
            spdlog::error("render-target-orientation needs textures/channels.png and the Roboto font");
            return false;
        }

        // Layers for the scenes rendered into textures, not rendered by the main camera
        scene()->layers()->pushOpaque(std::make_shared<Layer>("RTTSource", LAYERID_RTT));
        scene()->layers()->pushOpaque(std::make_shared<Layer>("UVBake", LAYERID_UV));

        Entity* camera = createCamera(Vector3(0.0f, 0.0f, 20.0f));
        camera->setName("MainCamera");
        Camera* cam = camera->findComponent<CameraComponent>()->camera();
        cam->setProjection(ProjectionType::Orthographic);
        cam->setOrthoHeight(6.6f);
        cam->setClearColor(Color(0.13f, 0.13f, 0.13f, 1.0f));

        // ---------- the source scene, rendered into textures by the camera target tiles ----------
        Entity* source = createPrimitive("plane", tileMaterial(_channels), Vector3(100.0f, 0.0f, 0.0f),
            Vector3(2.0f, 1.0f, 2.0f), {LAYERID_RTT});
        source->setName("RTTSourcePlane");
        source->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        auto markerMaterial = std::make_shared<StandardMaterial>();
        markerMaterial->setEmissive(Color(1.0f, 1.0f, 0.0f, 1.0f));
        markerMaterial->setUseLighting(false);
        _materials.push_back(markerMaterial);
        Entity* marker = createPrimitive("box", markerMaterial.get(), Vector3(100.0f - 0.75f, 0.75f, 0.2f),
            Vector3(0.2f, 0.2f, 0.05f), {LAYERID_RTT});
        marker->setName("Marker");

        _texCamera = createTexture("RT-camera");
        createRtCamera("CameraRT", createTarget(_texCamera.get()), false);
        _texCube = createTexture("RT-cube", 128, true);
        RenderTargetOptions cubeOptions;
        cubeOptions.graphicsDevice = device().get();
        cubeOptions.name = "RT-cube";
        cubeOptions.colorBuffer = _texCube.get();
        cubeOptions.face = 0;   // +X
        cubeOptions.depth = true;
        createRtCamera("CameraCube", device()->createRenderTarget(cubeOptions), true);

        // ---------- quad passes: the cube face, a copy of the camera target and a rect blit ----------
        _texCubeFace = createTexture("RT-cube-face");
        addQuadPass(std::make_shared<QuadPass>(device(), "orientation-cube-face", kCubeFaceSlang, _texCube.get()),
            _texCubeFace.get(), std::nullopt);
        _texCopy = createTexture("RT-copy");
        addQuadPass(std::make_shared<QuadPass>(device(), "orientation-copy", kCopySlang, _texCamera.get()),
            _texCopy.get(), std::nullopt);
        _texRect = createTexture("RT-rect");
        addQuadPass(std::make_shared<QuadPass>(device(), "orientation-copy", kCopySlang, _channels),
            _texRect.get(), Vector4(64.0f, 128.0f, 128.0f, 128.0f));

        // ---------- uv-space write ----------
        _texUv = createTexture("RT-uv");
        if (auto shader = getOrCreateSlangShaderFromSource(device().get(), "orientation-uv-write", kUvWriteSlang)) {
            _uvMaterial = std::make_shared<ShaderMaterial>("OrientationUVWrite", shader);
            _uvMaterial->setBaseColorTexture(_channels);
            _uvMaterial->setCullMode(CullMode::CULLFACE_NONE);
            Entity* plane = createPrimitive("plane", _uvMaterial.get(), Vector3(200.0f, 0.0f, 0.0f),
                Vector3(1.0f, 1.0f, 1.0f), {LAYERID_UV});
            plane->setName("UVWrite-plane");
            plane->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
            Entity* uvCamera = createCamera(Vector3(200.0f, 0.0f, 5.0f));
            uvCamera->setName("UVWrite-camera");
            auto* uvComponent = uvCamera->findComponent<CameraComponent>();
            uvComponent->setLayers({LAYERID_UV});
            uvComponent->setPriority(-1);
            Camera* uvCam = uvComponent->camera();
            uvCam->setProjection(ProjectionType::Orthographic);
            uvCam->setOrthoHeight(2.0f);
            uvCam->setClearColor(Color(0.25f, 0.1f, 0.1f, 1.0f));
            uvCam->setClearDepthBuffer(false);
            uvCam->setRenderTarget(createTarget(_texUv.get()));
        }

        // ---------- the readback tile ----------
        _readbackMaterial = std::make_shared<StandardMaterial>();
        _readbackMaterial->setEmissive(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _readbackMaterial->setUseLighting(false);

        // ---------- the grid of result tiles ----------
        createLabel("ONE ORIGIN: EVERY TILE MATCHES ACROSS METAL / VULKAN", 0.0f, 4.6f, 0.26f);
        addTile(0, 0, tileMaterial(_channels), "upload", "upright");
        addTile(1, 0, tileMaterial(_texCamera.get()), "camera RT", "upright");
        addTile(2, 0, tileMaterial(_texCubeFace.get()), "cube face +X", "upright");
        addTile(3, 0, tileMaterial(_texCopy.get()), "quad copy", "upright");
        addTile(0, 1, tileMaterial(_texRect.get()), "rect blit", "cell at bottom");
        addTile(1, 1, tileMaterial(_texUv.get()), "uv-space write", "upright");
        addTile(2, 1, _readbackMaterial.get(), "readback camera RT", "yellow");
        return true;
    }

    void update(float /*dt*/) override
    {
        // One read back of texel (32, 32) of the camera target, once it has rendered
        if (++_frame == 5) {
            std::vector<uint8_t> texel;
            if (_texCamera->read(texel, 32, 32, 1, 1) && texel.size() >= 4) {
                _readbackMaterial->setEmissive(Color(texel[0] / 255.0f, texel[1] / 255.0f, texel[2] / 255.0f, 1.0f));
                spdlog::info("readback camera RT: texel (32, 32) = {}, {}, {}", texel[0], texel[1], texel[2]);
            } else {
                spdlog::error("readback camera RT: the read failed");
            }
        }
    }

    void destroy() override
    {
        // The passes are registered with the renderer, so they go while the engine is alive.
        if (engine() && engine()->renderer()) {
            for (const auto& pass : _quadPasses) {
                engine()->renderer()->removePrependPass(pass);
            }
        }
        _quadPasses.clear();
        if (_uvMaterial) {
            _uvMaterial->setBaseColorTexture(nullptr);
        }
    }

private:
    std::shared_ptr<Texture> createTexture(const char* name, const uint32_t size = kRtSize, const bool cubemap = false)
    {
        TextureOptions options;
        options.name = name;
        options.width = size;
        options.height = size;
        options.cubemap = cubemap;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_shared<Texture>(device().get(), options);
        texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        return texture;
    }

    std::shared_ptr<RenderTarget> createTarget(Texture* texture, const bool depth = true)
    {
        RenderTargetOptions options;
        options.graphicsDevice = device().get();
        options.name = texture->name();
        options.colorBuffer = texture;
        options.depth = depth;
        auto target = device()->createRenderTarget(options);
        _targets.push_back(target);
        return target;
    }

    // An emissive-only material showing a texture
    StandardMaterial* tileMaterial(Texture* texture)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setEmissiveMap(texture);
        material->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        material->setUseLighting(false);
        _materials.push_back(material);
        return material.get();
    }

    // A camera rendering the source scene into a target
    void createRtCamera(const char* name, const std::shared_ptr<RenderTarget>& target, const bool perspective)
    {
        _targets.push_back(target);
        Entity* camera = createCamera(Vector3(100.0f, 0.0f, perspective ? 1.0f : 5.0f));
        camera->setName(name);
        auto* component = camera->findComponent<CameraComponent>();
        component->setLayers({LAYERID_RTT});
        component->setPriority(-1);
        Camera* cam = component->camera();
        cam->setClearColor(Color(0.1f, 0.1f, 0.25f, 1.0f));
        if (perspective) {
            cam->setFov(90.0f);
        } else {
            cam->setProjection(ProjectionType::Orthographic);
            cam->setOrthoHeight(1.0f);
        }
        cam->setRenderTarget(target);
    }

    // A quad pass into a texture, run at the start of every frame, clearing it to the targets'
    // colour first and drawing into the viewport rect when one is given
    void addQuadPass(const std::shared_ptr<QuadPass>& pass, Texture* target, const std::optional<Vector4>& rect)
    {
        pass->init(createTarget(target, false));
        const Color clear(0.1f, 0.1f, 0.25f, 1.0f);
        pass->setClearColor(&clear);
        if (rect) {
            pass->setViewport(rect);
            pass->setScissor(rect);
        }
        engine()->renderer()->addPrependPass(pass);
        _quadPasses.push_back(pass);
    }

    // A text label: laid out at font size 64 and scaled to `size` units
    void createLabel(const std::string& message, const float x, const float y, const float size = 0.22f)
    {
        ElementComponent* element = createElement(engine(), root(), {
            .type = ElementType::Text,
            .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
            .pivot = Vector2(0.5f, 0.5f),
            .font = _font,
            .text = message,
            .fontSize = kLabelFontSize
        });
        const float scale = size / static_cast<float>(kLabelFontSize);
        element->entity()->setLocalPosition(x, y, 0.0f);
        element->entity()->setLocalScale(scale, scale, scale);
    }

    // A result tile (a plane displaying a material) with a label under it, at a grid position
    void addTile(const int col, const int row, Material* material, const std::string& label, const std::string& expect)
    {
        const float x = (static_cast<float>(col) - 1.5f) * 2.7f;
        const float y = (0.5f - static_cast<float>(row)) * 3.2f + 0.6f;
        Entity* tile = createPrimitive("plane", material, Vector3(x, y, 0.0f), Vector3(2.0f, 1.0f, 2.0f));
        tile->setName("Tile-" + label);
        tile->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        createLabel(label + "\nexpect: " + expect, x, y - 1.4f);
    }

    std::unique_ptr<Asset> _channelsAsset;
    std::unique_ptr<Asset> _fontAsset;
    Texture* _channels = nullptr;
    FontResource* _font = nullptr;
    std::vector<std::shared_ptr<Material>> _materials;
    std::vector<std::shared_ptr<RenderTarget>> _targets;
    std::vector<std::shared_ptr<QuadPass>> _quadPasses;
    std::shared_ptr<Texture> _texCamera;
    std::shared_ptr<Texture> _texCube;
    std::shared_ptr<Texture> _texCubeFace;
    std::shared_ptr<Texture> _texCopy;
    std::shared_ptr<Texture> _texRect;
    std::shared_ptr<Texture> _texUv;
    std::shared_ptr<ShaderMaterial> _uvMaterial;
    std::shared_ptr<StandardMaterial> _readbackMaterial;
    int _frame = 0;
};

VISUTWIN_EXAMPLE_MAIN(RenderTargetOrientationExample)
