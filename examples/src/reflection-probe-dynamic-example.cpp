// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of the upstream "graphics/reflection-cubemap" example.
//
// A high-polygon shiny ball reflects the scene through a cubemap re-rendered from its
// own centre every frame. Six coloured primitives orbit and tumble around it over a
// green ground plane, lit by a yellow shadow-casting directional light and the helipad
// skydome. The ball sits alone on an "Excluded" layer so the six cube faces never
// capture it — a probe at the ball's centre would otherwise photograph its own inside.
//
// Around the edges, that same captured cube is reprojected into the other spherical
// layouts and blitted to screen, which is what upstream's panel demonstrates:
//   cube -> equirect, cube -> octahedral, equirect -> octahedral, octahedral -> equirect
// plus a prefiltered environment atlas built from the same cube.
//
// DEVIATIONS:
//  - upstream drives the capture with its `cubemapRenderer` utility script and binds
//    the result to ONE material (`shinyMat.cubeMap`, `useSkybox = false`). This engine
//    exposes the same technique as framework/extras/ReflectionProbe, whose cube is
//    installed SCENE-wide via Scene::setReflectionProbe — there is no per-material
//    probe assignment yet. The orbiting primitives and the ground therefore take their
//    specular from the captured cube too, where upstream leaves them on the skybox.
//  - upstream rebuilds the env atlas every frame. EnvLighting::generateAtlas allocates
//    a fresh texture per call and runs a 1024/2048-sample convolution, so this builds
//    it once from the first captured cube instead.
//  - upstream draws the previews with app.drawTexture, an immediate-mode call this
//    engine does not have; each preview is a quad pass appended to the frame graph.
//  - the shape and colour of each primitive comes from a fixed seed so the scene is
//    reproducible for screenshot comparison; upstream reseeds from Math.random().
//
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/extras/reflectionProbe.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/envLighting.h"
#include "scene/graphics/envReproject.h"
#include "scene/graphics/renderPassDownsample.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

// Layer for objects that must not render into the cubemap (upstream: 'Excluded').
constexpr int LAYERID_EXCLUDED = 100;

// Upstream renders the dynamic cube at 256 with mipmaps.
constexpr int PROBE_FACE_SIZE = 256;

constexpr int NUM_PRIMITIVES = 6;

// DEVIATION: upstream builds a 200x200-band sphere because its built-in `sphere`
// primitive is coarse. This engine's built-in sphere is 48x48 — still short of what a
// mirror ball's silhouette wants — so the high-poly mesh is generated at upstream's
// band count here.
static std::shared_ptr<Mesh> createHighQualitySphere(const std::shared_ptr<GraphicsDevice>& device,
    const int latitudeBands, const int longitudeBands)
{
    constexpr float PI_F = 3.14159265358979323846f;
    constexpr float radius = 0.5f;

    // Interleaved standard vertex: position(3) normal(3) uv0(2) tangent(4) uv1(2).
    std::vector<float> vertices;
    std::vector<uint32_t> indices;

    for (int lat = 0; lat <= latitudeBands; ++lat) {
        const float theta = static_cast<float>(lat) * PI_F / static_cast<float>(latitudeBands);
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);

        for (int lon = 0; lon <= longitudeBands; ++lon) {
            const float phi = static_cast<float>(lon) * 2.0f * PI_F / static_cast<float>(longitudeBands);
            const float sinPhi = std::sin(phi);
            const float cosPhi = std::cos(phi);

            const float nx = cosPhi * sinTheta;
            const float ny = cosTheta;
            const float nz = sinPhi * sinTheta;
            const float u = 1.0f - static_cast<float>(lon) / static_cast<float>(longitudeBands);
            const float v = 1.0f - static_cast<float>(lat) / static_cast<float>(latitudeBands);

            vertices.insert(vertices.end(), {
                radius * nx, radius * ny, radius * nz,
                nx, ny, nz,
                u, v,
                -sinPhi, 0.0f, cosPhi, 1.0f,
                u, v
            });
        }
    }

    for (int lat = 0; lat < latitudeBands; ++lat) {
        for (int lon = 0; lon < longitudeBands; ++lon) {
            const auto first = static_cast<uint32_t>(lat * (longitudeBands + 1) + lon);
            const auto second = static_cast<uint32_t>(first + longitudeBands + 1);
            indices.insert(indices.end(), {first, second, first + 1u});
            indices.insert(indices.end(), {second, second + 1u, first + 1u});
        }
    }

    const int vertexCount = static_cast<int>(vertices.size() / 14u);

    auto vertexFormat = std::make_shared<VertexFormat>(
        56, VertexFormat::standardElements(), true, false);

    VertexBufferOptions vbOptions;
    vbOptions.data.resize(vertices.size() * sizeof(float));
    std::memcpy(vbOptions.data.data(), vertices.data(), vbOptions.data.size());
    auto vertexBuffer = device->createVertexBuffer(vertexFormat, vertexCount, vbOptions);

    std::vector<uint8_t> indexBytes(indices.size() * sizeof(uint32_t));
    std::memcpy(indexBytes.data(), indices.data(), indexBytes.size());
    auto indexBuffer = device->createIndexBuffer(
        INDEXFORMAT_UINT32, static_cast<int>(indices.size()), indexBytes);

    auto mesh = std::make_shared<Mesh>();
    mesh->setVertexBuffer(vertexBuffer);
    mesh->setIndexBuffer(indexBuffer);
    Primitive prim;
    prim.type = PRIMITIVE_TRIANGLES;
    prim.indexed = true;
    prim.base = 0;
    prim.count = static_cast<int>(indices.size());
    mesh->setPrimitive(prim);
    mesh->setAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(radius, radius, radius)));
    return mesh;
}

class ReflectionProbeDynamicExample final: public ExampleApp
{
public:
    ReflectionProbeDynamicExample()
        : ExampleApp({.title = "Dynamic Reflection Cubemap"}) {}

protected:
    bool create() override
    {
        // Setup skydome — upstream: skyboxMip 0 (full resolution), intensity 2.
        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        if (const auto helipadResource = _helipad->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));
        } else {
            spdlog::error("Failed to load helipad env atlas texture");
            return false;
        }
        scene()->setSkyboxMip(0);
        scene()->setSkyboxIntensity(2.0f);

        // A layer for objects that do not render into the cubemap. Upstream pushes it
        // onto the end of the existing composition rather than rebuilding one.
        const auto layers = scene()->layers();
        auto excludedLayer = std::make_shared<Layer>("Excluded", LAYERID_EXCLUDED);
        layers->pushOpaque(excludedLayer);
        layers->pushTransparent(excludedLayer);

        // -----------------------------------------------------------------------
        // Shiny ball — on the excluded layer so it never renders into its own cube.
        // -----------------------------------------------------------------------
        _shinyMaterial = std::make_shared<StandardMaterial>();
        _shinyMaterial->setDiffuse(Color(0.6f, 0.6f, 0.6f, 1.0f));   // darken the reflection a little
        _shinyMaterial->setMetalness(1.0f);                          // shiny, no diffuse component
        _shinyMaterial->setUseMetalness(true);

        _shinyMesh = createHighQualitySphere(device(), 200, 200);

        auto* shinyBall = new Entity();
        shinyBall->setName("ShinyBall");
        shinyBall->setEngine(engine());
        shinyBall->setLocalPosition(0.0f, 0.0f, 0.0f);
        shinyBall->setLocalScale(10.0f, 10.0f, 10.0f);
        if (auto* render = static_cast<RenderComponent*>(shinyBall->addComponent<RenderComponent>())) {
            render->setMaterial(_shinyMaterial.get());
            render->setLayers({LAYERID_EXCLUDED});
            render->addMeshInstance(std::make_unique<MeshInstance>(
                _shinyMesh.get(), _shinyMaterial.get(), shinyBall));
        }
        root()->addChild(shinyBall);

        // -----------------------------------------------------------------------
        // The dynamic cubemap. Constructed BEFORE the main camera: its six face
        // cameras render as ordinary cameras and layer composition renders cameras in
        // construction order, so building it later would leave the main camera
        // sampling the previous frame's cube (upstream orders it with priority -1).
        //
        // Upstream's camera-on-the-ball renders the World and Skybox layers only.
        // -----------------------------------------------------------------------
        _probe = std::make_unique<ReflectionProbe>(engine(), PROBE_FACE_SIZE);
        _probe->setPosition(Vector3(0.0f, 0.0f, 0.0f));
        _probe->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
        // No box projection — upstream treats the capture as an infinite environment.
        _probe->setBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f), false);
        _probe->setDynamic(true);

        // -----------------------------------------------------------------------
        // Six random primitives in the world layer, plus a green ground plane.
        // -----------------------------------------------------------------------
        const char* shapes[] = {"box", "cone", "cylinder", "sphere", "capsule"};
        std::mt19937 rng(2026u);
        std::uniform_int_distribution<int> shapeDist(0, 4);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);

        for (int i = 0; i < NUM_PRIMITIVES; ++i) {
            const Color color(unit(rng), unit(rng), unit(rng), 1.0f);
            _entities.push_back(createColoredPrimitive(
                shapes[shapeDist(rng)], Vector3(0.0f, 0.0f, 0.0f),
                Vector3(3.0f, 3.0f, 3.0f), color));
        }

        // Green plane as a base to cast shadows on.
        createColoredPrimitive("plane", Vector3(0.0f, -8.0f, 0.0f),
            Vector3(20.0f, 20.0f, 20.0f), Color(0.3f, 0.5f, 0.3f, 1.0f));

        // -----------------------------------------------------------------------
        // Main camera — renders world, excluded and skybox layers.
        // -----------------------------------------------------------------------
        _camera = createCamera(Vector3(20.0f, 2.0f, 0.0f));
        if (auto* cameraComp = _camera->findComponent<CameraComponent>();
            cameraComp && cameraComp->camera()) {
            cameraComp->setLayers({LAYERID_WORLD, LAYERID_EXCLUDED, LAYERID_SKYBOX,
                                   LAYERID_IMMEDIATE, LAYERID_UI});
            cameraComp->camera()->setFov(60.0f);
            cameraComp->setToneMapping(TONEMAP_ACES);
        }

        // -----------------------------------------------------------------------
        // Yellow shadow-casting directional light, world layer only.
        // -----------------------------------------------------------------------
        auto* light = createDirectionalLight(Vector3(0.0f, 0.0f, 0.0f),
            Color(1.0f, 1.0f, 0.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setLayers({LAYERID_WORLD});
            lightComp->setRange(40.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowResolution(1024);
            lightComp->setShadowNormalBias(0.05f);
            lightComp->setShadowDistance(40.0f);
        }

        createReprojectionPanel();

        spdlog::info("Dynamic reflection cubemap: the shiny ball re-renders its own {}px cube "
                     "every frame from the World + Skybox layers. Esc quits.", PROBE_FACE_SIZE);
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Rotate primitives around their center and also orbit them around the shiny sphere.
        for (size_t e = 0; e < _entities.size(); ++e) {
            const float scale = static_cast<float>(e + 1) / static_cast<float>(_entities.size());
            const float offset = _time + static_cast<float>(e) * 200.0f;
            _entities[e]->setLocalPosition(
                7.0f * std::sin(offset),
                2.0f * (static_cast<float>(e) - 3.0f),
                7.0f * std::cos(offset));
            _entities[e]->rotate(1.0f * scale, 2.0f * scale, 3.0f * scale);
        }

        // Slowly orbit the camera around.
        _camera->setLocalPosition(
            20.0f * std::cos(_time * 0.2f),
            2.0f,
            20.0f * std::sin(_time * 0.2f));
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
    }

    void postRender() override
    {
        // Mip generation and the four reprojections below are one batch: each would
        // otherwise create, encode and commit a command buffer of its own, five per
        // frame. Encoding order inside the batch still orders the GPU work, so the
        // reprojections continue to read the mips generated just above them.
        device()->beginEnvBatch();

        // Regenerate the roughness mips from the freshly captured faces and keep the
        // probe installed (upstream's cubemapRenderer does this inside its own update).
        _probe->update();

        const auto& sourceCube = _probe->cubemapShared();
        if (!sourceCube) {
            device()->endEnvBatch();
            return;
        }

        // Cube -> equirect, and cube -> octahedral.
        reproject(sourceCube, _textureEqui, true);
        reproject(sourceCube, _textureOcta, true);

        // ...then round-trip between the two 2D layouts, which is what actually
        // exercises the projections against each other rather than against the cube.
        reproject(_textureEqui, _textureOcta2, false);
        reproject(_textureOcta, _textureEqui2, false);

        device()->endEnvBatch();

        // The prefiltered atlas is built once — see the DEVIATIONS note. It is fed the
        // EQUIRECT reprojection rather than the cube: generateAtlas runs its source
        // through equirectToCubemap itself, so handing it a cubemap makes it sample a
        // cube as if it were a 2D panorama and the atlas comes out black.
        if (!_atlasBuilt) {
            _atlasBuilt = true;
            if (auto* atlas = EnvLighting::generateAtlas(device().get(), _textureEqui.get(), 512)) {
                _textureAtlas.reset(atlas);
                _atlasPass->setSourceTexture(_textureAtlas.get());
            }
        }
    }

    void destroy() override
    {
        // The panel passes are registered with the renderer and the probe owns cameras
        // and render targets, so both go while the engine is still alive.
        _panelPasses.clear();
        _atlasPass.reset();
        _probe.reset();
    }

private:
    Entity* createColoredPrimitive(const char* type, const Vector3& position,
        const Vector3& scale, const Color& color)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setGloss(0.6f);
        material->setMetalness(0.7f);
        material->setUseMetalness(true);
        _materials.push_back(material);

        return createPrimitive(type, material.get(), position, scale, {LAYERID_WORLD});
    }

    std::shared_ptr<Texture> createProjectionTexture(const char* name, const int size,
        const TextureProjection projection) const
    {
        TextureOptions options;
        options.name = name;
        options.width = static_cast<uint32_t>(size);
        options.height = static_cast<uint32_t>(size);
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        options.projection = projection;
        auto texture = std::make_shared<Texture>(device().get(), options);
        texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        // Reprojection renders INTO these, so the GPU image has to exist before the
        // first pass. Metal creates it lazily on first use; Vulkan decides whether a
        // texture is color-renderable when the image is created, so an un-uploaded
        // target is rejected outright ("target is not color-renderable").
        texture->upload();
        return texture;
    }

    // reprojectTexture reads both ends' layouts off the textures themselves, so the
    // only thing left to say here is whether the source is the cube.
    void reproject(const std::shared_ptr<Texture>& source,
        const std::shared_ptr<Texture>& target, const bool sourceIsCubemap) const
    {
        EnvReprojectOptions options;
        options.source = source;
        options.sourceIsCubemap = sourceIsCubemap;
        options.target = target;
        options.rects.push_back(EnvReprojectRect{
            0, 0, static_cast<int>(target->width()), static_cast<int>(target->height()), 0});
        options.encodeRgbp = false;   // shown directly, not fed back as lighting
        reprojectTexture(device().get(), options);
    }

    // Upstream lays the five previews out with app.drawTexture(x, y, w, h) in NDC,
    // where x/y is the centre and w/h the size. This engine has no immediate texture
    // draw, so each becomes a full-screen-quad pass with a viewport.
    std::shared_ptr<RenderPassDownsample> addPanel(const std::shared_ptr<Texture>& texture,
        const float x, const float y, const float w, const float h)
    {
        const auto [deviceWidth, deviceHeight] = device()->size();
        const auto screenW = static_cast<float>(std::max(1, deviceWidth));
        const auto screenH = static_cast<float>(std::max(1, deviceHeight));

        auto pass = std::make_shared<RenderPassDownsample>(device(), texture.get());
        pass->init(nullptr);
        pass->setRequiresCubemaps(false);

        const float left = ((x - w * 0.5f) + 1.0f) * 0.5f * screenW;
        const float top = (1.0f - (y + h * 0.5f)) * 0.5f * screenH;
        const Vector4 viewport(left, top, w * 0.5f * screenW, h * 0.5f * screenH);
        pass->setViewport(viewport);
        pass->setScissor(viewport);

        engine()->renderer()->addAppendPass(pass);
        _panelPasses.push_back(pass);
        return pass;
    }

    void createReprojectionPanel()
    {
        _textureEqui = createProjectionTexture("reproject-equi", 256,
            TextureProjection::TEXTUREPROJECTION_EQUIRECT);
        _textureEqui2 = createProjectionTexture("reproject-equi2", 256,
            TextureProjection::TEXTUREPROJECTION_EQUIRECT);
        _textureOcta = createProjectionTexture("reproject-octa", 64,
            TextureProjection::TEXTUREPROJECTION_OCTAHEDRAL);
        _textureOcta2 = createProjectionTexture("reproject-octa2", 32,
            TextureProjection::TEXTUREPROJECTION_OCTAHEDRAL);

        // Stands in until the first cube exists and the real atlas replaces it.
        _textureAtlas = createProjectionTexture("reproject-atlas", 512,
            TextureProjection::TEXTUREPROJECTION_OCTAHEDRAL);

        addPanel(_textureEqui, -0.6f, 0.7f, 0.6f, 0.3f);
        addPanel(_textureOcta, 0.7f, 0.7f, 0.4f, 0.4f);
        addPanel(_textureOcta2, -0.7f, -0.7f, 0.4f, 0.4f);
        addPanel(_textureEqui2, 0.6f, -0.7f, 0.6f, 0.3f);
        _atlasPass = addPanel(_textureAtlas, 0.0f, -0.7f, 0.5f, 0.4f);
    }

    std::unique_ptr<Asset> _helipad;
    std::shared_ptr<StandardMaterial> _shinyMaterial;
    std::shared_ptr<Mesh> _shinyMesh;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Entity*> _entities;
    std::unique_ptr<ReflectionProbe> _probe;
    Entity* _camera = nullptr;

    std::shared_ptr<Texture> _textureEqui;
    std::shared_ptr<Texture> _textureEqui2;
    std::shared_ptr<Texture> _textureOcta;
    std::shared_ptr<Texture> _textureOcta2;
    std::shared_ptr<Texture> _textureAtlas;
    std::vector<std::shared_ptr<RenderPassDownsample>> _panelPasses;
    std::shared_ptr<RenderPassDownsample> _atlasPass;
    bool _atlasBuilt = false;

    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ReflectionProbeDynamicExample)
