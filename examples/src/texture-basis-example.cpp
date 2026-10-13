// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/texture-basis.
//
// Basis Universal textures, transcoded at load to the device's compressed format (ASTC on Apple
// GPUs, BC7 on desktop ones): a torus (tube 0.2, ring 0.3, 50 segments, 40 sides) at scale 2,
// tumbling (its euler angles a, 2a and 4a, a growing 10 degrees a second), with a metalness
// material (metalness 0.7, gloss 0.8) whose colour, normal and gloss maps are the seaside rocks,
// each tiled 7 x 7. The normal map is PACKED: x in alpha, y in green, the layout a two-channel
// block-compressed normal map keeps. Lit by the helipad environment (skybox mip 1, intensity
// 1.4) and a white directional light at (45, 0, 45); the camera at (0, 0, 4) clears to
// (0.4, 0.45, 0.5) with ACES.
//
// DEVIATION: the three textures are KTX2 (ETC1S, mipmapped) where upstream's are .basis files,
// which this engine does not load: encoded from the same seaside-rocks01 JPEGs with upstream's
// layout (sRGB colour, linear gloss, the normal map packed into green and alpha), so they match
// upstream's up to the encoder's own rounding.
//
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

class TextureBasisExample final : public ExampleApp
{
public:
    TextureBasisExample() : ExampleApp({.title = "Texture Basis"}) {}

protected:
    bool create() override
    {
        _color = std::make_unique<Asset>("color", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-color.ktx2"));
        _gloss = std::make_unique<Asset>("gloss", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-gloss.ktx2"));
        _normal = std::make_unique<Asset>("normal", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-normal.ktx2"), AssetData{.type = TextureType::TEXTURETYPE_SWIZZLEGGGR});
        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        Texture* color = _color->resourceAs<Texture>();
        Texture* gloss = _gloss->resourceAs<Texture>();
        Texture* normal = _normal->resourceAs<Texture>();
        Texture* envAtlas = _envAtlas->resourceAs<Texture>();
        if (!color || !gloss || !normal || !envAtlas) {
            spdlog::error("texture-basis: a texture failed to load");
            return false;
        }

        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(1.4f);
        scene()->setEnvAtlas(envAtlas);

        createDirectionalLight(Vector3(45.0f, 0.0f, 45.0f));

        _material = std::make_shared<StandardMaterial>();
        _material->setUseMetalness(true);
        _material->setGloss(0.8f);
        _material->setMetalness(0.7f);
        _material->setDiffuseMap(color);
        _material->setNormalMap(normal);
        _material->setGlossMap(gloss);
        _material->setDiffuseMapTiling(Vector2(7.0f, 7.0f));
        _material->setNormalMapTiling(Vector2(7.0f, 7.0f));
        _material->setGlossMapTiling(Vector2(7.0f, 7.0f));

        // The torus
        _torusMesh = createMeshFromGeometry(device(), createTorusGeometry(0.2f, 0.3f, 360.0f, 50, 40));
        _shape = new Entity();
        _shape->setEngine(engine());
        auto* render = static_cast<RenderComponent*>(_shape->addComponent<RenderComponent>());
        render->setMaterial(_material.get());
        render->addMeshInstance(std::make_unique<MeshInstance>(_torusMesh, _material, _shape));
        _shape->setLocalScale(2.0f, 2.0f, 2.0f);
        root()->addChild(_shape);

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 4.0f));
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        cameraComponent->setToneMapping(TONEMAP_ACES);
        return true;
    }

    void update(const float dt) override
    {
        _angle = std::fmod(_angle + dt * 10.0f, 360.0f);
        _shape->setLocalEulerAngles(_angle, _angle * 2.0f, _angle * 4.0f);
    }

private:
    std::unique_ptr<Asset> _color;
    std::unique_ptr<Asset> _gloss;
    std::unique_ptr<Asset> _normal;
    std::unique_ptr<Asset> _envAtlas;
    std::shared_ptr<StandardMaterial> _material;
    std::shared_ptr<Mesh> _torusMesh;
    Entity* _shape = nullptr;
    float _angle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(TextureBasisExample)
