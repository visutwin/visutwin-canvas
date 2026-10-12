// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2026
//
// Port of upstream materials/parallax-mapping.
//
// A closed brick room with a brick sphere resting on the floor, lit by a warm spot
// light above and a cool omni light in the far corner. The relief is marched from
// the height map, so it holds up on the flat walls and on the curved sphere alike,
// and the effect is strongest where the surface is seen at a grazing angle.
//
// The room is six inward-facing planes rather than a box seen from inside: a
// tangent frame is mirrored on a back face, and two-sided
// lighting flips only the normal and not the tangents, so the marched relief on
// those faces comes out inside out. Every face front-facing avoids that.
//
// DEVIATION: upstream's control panel (parallax mode, sample count, height, base, the two
// lights and the environment) is not reproduced; the values are its initial ones.
//
#include <memory>
#include <string>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/render/primitiveGeometry.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kRoomSize = 10.0f;
    constexpr float kSphereSize = 2.2f;
}

class ParallaxMappingExample final: public ExampleApp
{
public:
    ParallaxMappingExample(): ExampleApp({.title = "Parallax Mapping"}) {}

protected:
    bool create() override
    {
        // The room is closed, so the environment only fills the shadows.
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setSkyboxIntensity(0.1f);
        scene()->setExposure(1.0f);

        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/morning-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (Texture* envTexture = _envAtlas->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envTexture);
        }

        _color = load("color", "textures/bricks076a/color.png");
        _normal = load("normal", "textures/bricks076a/normal.png");
        _height = load("height", "textures/bricks076a/height.png");
        _ao = load("ao", "textures/bricks076a/ao.png");
        _roughness = load("roughness", "textures/bricks076a/roughness.png");
        if (_color == nullptr || _height == nullptr) {
            spdlog::error("parallax-mapping needs the bricks076a texture set");
            return false;
        }

        const Vector3 spherePosition(0.0f, (kSphereSize - kRoomSize) * 0.5f, 0.0f);

        _roomMaterial = brickMaterial(3.0f, 3.0f);
        _sphereMaterial = brickMaterial(3.0f, 2.0f);

        struct Wall { const char* name; Vector3 position; Vector3 rotation; };
        const float half = kRoomSize * 0.5f;
        const Wall walls[] = {
            {"floor",      Vector3(0.0f, -half, 0.0f), Vector3(0.0f, 0.0f, 0.0f)},
            {"ceiling",    Vector3(0.0f, half, 0.0f),  Vector3(180.0f, 0.0f, 0.0f)},
            {"wall back",  Vector3(0.0f, 0.0f, -half), Vector3(90.0f, 0.0f, 0.0f)},
            {"wall front", Vector3(0.0f, 0.0f, half),  Vector3(-90.0f, 0.0f, 0.0f)},
            {"wall left",  Vector3(-half, 0.0f, 0.0f), Vector3(0.0f, 0.0f, -90.0f)},
            {"wall right", Vector3(half, 0.0f, 0.0f),  Vector3(0.0f, 0.0f, 90.0f)},
        };
        for (const auto& wall : walls) {
            Entity* entity = createPrimitive("plane", _roomMaterial.get(), wall.position,
                Vector3(kRoomSize, 1.0f, kRoomSize));
            entity->setName(wall.name);
            entity->setLocalEulerAngles(wall.rotation.getX(), wall.rotation.getY(),
                wall.rotation.getZ());
        }

        // A 128-band sphere rather than the primitive: a coarser sphere leaves the silhouette
        // and the shading faceted, and the facet edges shimmer as the parallax offset steps
        // across them.
        _sphereMesh = createMeshFromGeometry(device(), createSphereGeometry(0.5f, 128, 128));
        auto* sphere = new Entity();
        sphere->setName("sphere");
        sphere->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(sphere->addComponent<RenderComponent>())) {
            render->addMeshInstance(std::make_unique<MeshInstance>(
                _sphereMesh.get(), _sphereMaterial.get(), sphere));
        }
        sphere->setLocalScale(kSphereSize, kSphereSize, kSphereSize);
        sphere->setLocalPosition(spherePosition);
        root()->addChild(sphere);

        // A spot light above the sphere. A light shines down its entity's -Y axis,
        // so aiming it is lookAt (which orients -Z) plus a quarter turn.
        auto* spot = new Entity();
        spot->setName("spot light");
        spot->setEngine(engine());
        if (auto* comp = static_cast<LightComponent*>(spot->addComponent<LightComponent>())) {
            comp->setType(LightType::LIGHTTYPE_SPOT);
            comp->setColor(Color(1.0f, 0.95f, 0.85f, 1.0f));
            comp->setIntensity(3.0f);
            comp->setRange(18.0f);
            comp->setInnerConeAngle(12.0f);
            comp->setOuterConeAngle(32.0f);
            comp->setCastShadows(true);
            comp->setShadowBias(0.2f);
            comp->setShadowNormalBias(0.05f);
            comp->setShadowResolution(1024);
        }
        root()->addChild(spot);
        spot->setLocalPosition(2.8f, 4.0f, 2.8f);
        spot->lookAt(spherePosition);
        spot->rotateLocal(90.0f, 0.0f, 0.0f);

        // A cooler omni in the opposite corner, filling the shadows.
        auto* omni = new Entity();
        omni->setName("omni light");
        omni->setEngine(engine());
        if (auto* comp = static_cast<LightComponent*>(omni->addComponent<LightComponent>())) {
            comp->setType(LightType::LIGHTTYPE_OMNI);
            comp->setColor(Color(0.55f, 0.7f, 1.0f, 1.0f));
            comp->setIntensity(2.0f);
            comp->setRange(18.0f);
            comp->setCastShadows(true);
            comp->setShadowBias(0.2f);
            comp->setShadowNormalBias(0.05f);
            comp->setShadowResolution(1024);
        }
        root()->addChild(omni);
        omni->setLocalPosition(-3.2f, -2.0f, -3.4f);

        auto* camera = createCamera(Vector3(spherePosition.getX() + 2.6f,
            spherePosition.getY() + 1.5f, spherePosition.getZ() + 3.0f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setFov(60.0f);
            comp->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
            comp->camera()->setNearClip(0.1f);
            comp->camera()->setFarClip(50.0f);
        }
        auto* controls = addOrbitControls(camera, spherePosition);
        controls->storeResetState();

        spdlog::info("Parallax mapping on brickwork: height 0.4, base 0.5 "
                     "(the relief pivots around mid-grey).");
        return true;
    }

private:
    Texture* load(const char* name, const char* relative)
    {
        _assets.push_back(std::make_unique<Asset>(name, AssetType::TEXTURE,
            assetPath(relative)));
        Texture* texture = _assets.back()->resourceAs<Texture>();
        if (!texture) {
            return nullptr;
        }
        return texture;
    }

    /// Every map shares the parallax offset, so every map needs the same tiling.
    std::shared_ptr<StandardMaterial> brickMaterial(const float tileU, const float tileV)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuseMap(_color);
        material->setNormalMap(_normal);
        material->setHeightMap(_height);
        material->setAoMap(_ao);
        // The map is roughness rather than gloss, so it is used inverted.
        material->setGlossMap(_roughness);
        material->setGlossInvert(true);
        material->setUseMetalness(true);
        material->setMetalness(0.0f);

        // Occlusion mapping, 16 samples at most, a relief 0.4 deep pivoting around mid-grey.
        material->setParallaxMode(ParallaxMode::PARALLAX_OCCLUSION);
        material->setParallaxSamples(16.0f);
        material->setHeightMapFactor(0.4f);
        material->setHeightMapBase(0.5f);

        // The parallax offset is applied to the base UV and every map follows it,
        // so every map needs the same tiling. The height map has no tiling of its
        // own precisely because it reads the base UV.
        const Vector2 tiling(tileU, tileV);
        material->setDiffuseMapTiling(tiling);
        material->setNormalMapTiling(tiling);
        material->setAoMapTiling(tiling);
        material->setGlossMapTiling(tiling);

        _materials.push_back(material);
        return material;
    }

    std::shared_ptr<Mesh> _sphereMesh;
    std::unique_ptr<Asset> _envAtlas;
    std::vector<std::unique_ptr<Asset>> _assets;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _roomMaterial, _sphereMaterial;
    Texture* _color = nullptr;
    Texture* _normal = nullptr;
    Texture* _height = nullptr;
    Texture* _ao = nullptr;
    Texture* _roughness = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(ParallaxMappingExample)
