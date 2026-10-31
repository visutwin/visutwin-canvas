// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream graphics/instancing-gooch.
//
// One custom shading model on instanced, skinned, morphed and static geometry: Gooch shading, a
// wrap-around brightness (N.L mapped from [-1, 1] to [0, 1]) mixing a cool tone (blue plus 0.4 of
// the surface colour) into a warm one (red plus 0.4 of it), then the usual fog, tone mapping and
// gamma. A low-poly tree is hardware-instanced 500 times over a ring between radius 2 and 10, each
// instance scaled 0.03-0.28, standing on a green box ground 30 x 30. The bitmoji character dances
// (the win-dance clip at 0.62 speed) in the middle, every one of its materials swapped for the
// Gooch one with its own diffuse map. The light direction turns round the scene once every 2 pi
// seconds; the camera orbits slowly at 8 units, 3 up, under the helipad environment (skybox mip 2),
// and tone-maps ACES.
//
// DEVIATIONS:
// - The Gooch material is a StandardMaterial whose lit colour a chunk override replaces
//   (forward-fragment-user's forwardUserLitColor), where upstream writes a ShaderMaterial on the
//   engine's transform chunks: the engine's vertex stages bring instancing, skinning and morphing
//   with them. The brightness is taken per fragment, upstream's per vertex.
// - The light direction is scene-wide data (Scene::setUserShaderData, slot 0), upstream's a
//   uniform set on every material.
// - An instance is a matrix (position and scale), where upstream's instance format is a position
//   and a scale read by the shader; the trees are placed by a fixed-seed generator.
// - The ground colour is gamma-encoded before it is set, since a material colour here is decoded
//   and upstream's shader uses its colour as linear.
//
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <random>
#include <vector>

#include "../animClips.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // Number of instanced trees to render
    constexpr int kInstanceCount = 500;

    // The Gooch shading, replacing the lit colour: the fog, tone mapping and gamma after it are the
    // forward program's own
    constexpr const char* kGoochChunk = R"slang(
float3 forwardUserLitColor(float3 color, float3 albedo, float3 worldNormal, float3 worldPos)
{
    // Gooch shading constants
    const float diffuseCool = 0.4;
    const float diffuseWarm = 0.4;
    const float3 cool = float3(0.0, 0.0, 0.6);
    const float3 warm = float3(0.6, 0.0, 0.0);

    // wrap-around diffuse lighting, toward the light direction in the scene's user data
    const float brightness = (dot(normalize(worldNormal), lighting.userData[0].xyz) + 1.0) * 0.5;

    // simple Gooch shading that highlights structural and contextual data
    const float3 kCool = min(cool + diffuseCool * albedo, float3(1.0));
    const float3 kWarm = min(warm + diffuseWarm * albedo, float3(1.0));
    return lerp(kCool, kWarm, brightness);
}

float forwardUserFogFactor(float fogFactor, float3 worldPos)
{
    return fogFactor;
}

float3 forwardUserFragmentEnd(float3 color, float3 worldPos, float3 worldNormal)
{
    return color;
}
)slang";

    // A Gooch material of a linear colour, with an optional diffuse map
    std::shared_ptr<StandardMaterial> createGoochMaterial(Texture* texture, const Color& linear = Color(1, 1, 1, 1))
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(std::pow(linear.r, 1.0f / 2.2f), std::pow(linear.g, 1.0f / 2.2f),
            std::pow(linear.b, 1.0f / 2.2f), 1.0f));
        if (texture) {
            material->setDiffuseMap(texture);
        }
        material->setShaderChunk("forward-fragment-user", kGoochChunk);
        return material;
    }
}

class InstancingGoochExample final : public ExampleApp
{
public:
    InstancingGoochExample() : ExampleApp({.title = "Instancing Gooch"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        _treeAsset = std::make_unique<Asset>("tree", AssetType::CONTAINER, assetPath("models/low-poly-tree.glb"));
        _bitmojiAsset = std::make_unique<Asset>("model", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _danceAsset = std::make_unique<Asset>("danceAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/win-dance.glb"));
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        auto* tree = _treeAsset->resourceAs<ContainerResource>();
        auto* bitmoji = _bitmojiAsset->resourceAs<ContainerResource>();
        Texture* helipad = _helipadAsset->resourceAs<Texture>();
        const auto danceTrack = firstAnimTrack(_danceAsset.get());
        if (!tree || !bitmoji || !helipad || !danceTrack) {
            spdlog::error("instancing-gooch needs the low-poly tree, bitmoji, its win-dance clip and the helipad atlas");
            return false;
        }

        // Setup skydome
        scene()->setSkyboxMip(2);
        scene()->setEnvAtlas(helipad);

        // Camera
        _camera = createCamera(Vector3(0.0f, 3.0f, 8.0f));
        _camera->findComponent<CameraComponent>()->setToneMapping(TONEMAP_ACES);

        // The instance matrices: random points in the ring between radius 2 and 10
        std::mt19937 random(7u);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        constexpr float radius0 = 2.0f;
        constexpr float radius1 = 10.0f;
        constexpr int kInstanceBytes = VertexFormat::INSTANCING_MATRIX_SIZE;
        std::vector<uint8_t> instanceBytes(static_cast<size_t>(kInstanceCount) * kInstanceBytes);
        for (int i = 0; i < kInstanceCount; ++i) {
            const float angle = unit(random) * 2.0f * std::numbers::pi_v<float>;
            const float radius = std::sqrt(unit(random) * (radius1 * radius1 - radius0 * radius0) + radius0 * radius0);
            const float scale = 0.03f + unit(random) * 0.25f;
            const Matrix4 matrix = Matrix4::trs(Vector3(radius * std::cos(angle), 1.0f, radius * std::sin(angle)),
                Quaternion::fromEulerAngles(0.0f, 0.0f, 0.0f), Vector3(scale, scale, scale));
            std::memcpy(instanceBytes.data() + static_cast<size_t>(i) * kInstanceBytes, &matrix, kInstanceBytes);
        }
        VertexBufferOptions options;
        options.data = std::move(instanceBytes);
        _instances = device()->createVertexBuffer(VertexFormat::defaultInstancingFormat(), kInstanceCount, options);

        // The forest: the tree model's first mesh instance, its material swapped for a Gooch one
        // keeping its texture, set up for instancing
        Entity* forest = tree->instantiateRenderEntity();
        forest->setEngine(engine());
        root()->addChild(forest);
        MeshInstance* treeInstance = nullptr;
        for (auto* render : forest->findComponents<RenderComponent>()) {
            if (!render->meshInstances().empty()) {
                treeInstance = render->meshInstances()[0];
                break;
            }
        }
        if (!treeInstance) {
            spdlog::error("instancing-gooch: the tree has no mesh instance");
            return false;
        }
        const auto* treeMaterial = dynamic_cast<const StandardMaterial*>(treeInstance->material());
        auto material = createGoochMaterial(treeMaterial ? treeMaterial->diffuseMap() : nullptr);
        treeInstance->setMaterial(material);
        treeInstance->setInstancing(_instances, kInstanceCount);

        // The ground, static geometry, with a Gooch material of its own without a texture
        _groundMaterial = createGoochMaterial(nullptr, Color(0.13f, 0.55f, 0.13f, 1.0f));
        createPrimitive("box", _groundMaterial.get(), Vector3(0.0f, -0.5f, 0.0f), Vector3(30.0f, 1.0f, 30.0f))
            ->setName("Ground");

        // The animated, morphed bitmoji, every material swapped for a Gooch one
        Entity* bitmojiEntity = bitmoji->instantiateRenderEntity();
        bitmojiEntity->setEngine(engine());
        bitmojiEntity->setLocalScale(2.5f, 2.5f, 2.5f);
        root()->addChild(bitmojiEntity);
        for (auto* render : bitmojiEntity->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                meshInstance->setCastShadow(false);
                const auto* source = dynamic_cast<const StandardMaterial*>(meshInstance->material());
                meshInstance->setMaterial(createGoochMaterial(source ? source->diffuseMap() : nullptr));
            }
        }

        // Play the animation
        auto* anim = static_cast<AnimComponent*>(bitmojiEntity->addComponent<AnimComponent>());
        anim->assignAnimation("Walk", danceTrack, {}, 0.62f);
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // A light direction turning round the scene, shared by the Gooch materials
        const Vector3 lightDir = Vector3(std::sin(_time), -0.5f, std::cos(_time)).normalized();
        scene()->setUserShaderData(0, Vector4(-lightDir.getX(), -lightDir.getY(), -lightDir.getZ(), 0.0f));

        // Orbit the camera
        _camera->setLocalPosition(8.0f * std::sin(_time * 0.01f), 3.0f, 8.0f * std::cos(_time * 0.01f));
        _camera->lookAt(Vector3(0.0f, 1.0f, 0.0f));
    }

private:
    std::unique_ptr<Asset> _treeAsset;
    std::unique_ptr<Asset> _bitmojiAsset;
    std::unique_ptr<Asset> _danceAsset;
    std::unique_ptr<Asset> _helipadAsset;
    std::shared_ptr<VertexBuffer> _instances;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    Entity* _camera = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(InstancingGoochExample)
