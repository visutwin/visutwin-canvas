// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Parallax occlusion mapping on the seaside-rocks material.
//
// Four quads carry the same colour/normal/gloss/height set and differ only in how
// the height map is used, so the contribution of each stage is visible side by
// side under one grazing directional light:
//
//   1. no height map          — flat normal-mapped rock
//   2. POM, base 0            — the whole displacement sinks BELOW the polygon
//   3. POM, base 0.5          — mid-grey reads as the original surface, so the
//                               ridges stand proud and the cracks sink in
//   4. POM, base 0.5 + shadow — the height field casts onto itself
//
// The camera sits off to one side, because parallax is a grazing-angle effect:
// viewed head-on all four quads look nearly identical, which is itself worth seeing.
//
#include <cmath>
#include <memory>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class ParallaxExample final: public ExampleApp
{
public:
    ParallaxExample(): ExampleApp({.title = "Parallax Occlusion Mapping"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.08f, 0.09f, 0.11f);

        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (const auto env = _envAtlas->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*env));
        }

        _color = std::make_unique<Asset>("color", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-color.jpg"));
        _normal = std::make_unique<Asset>("normal", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-normal.jpg"));
        _height = std::make_unique<Asset>("height", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-height.jpg"));

        Texture* colorTex = textureOf(_color);
        Texture* normalTex = textureOf(_normal);
        Texture* heightTex = textureOf(_height);
        if (colorTex == nullptr || heightTex == nullptr) {
            spdlog::error("Parallax example needs the seaside-rocks texture set");
            return false;
        }

        // A grazing key light: the self-shadow term only has something to say when
        // the light rakes across the height field.
        createDirectionalLight(Vector3(14.0f, 35.0f, 0.0f),
            Color(1.0f, 0.96f, 0.88f, 1.0f), 1.2f, false);

        struct Variant { const char* label; bool height; float base; float shadow; };
        const Variant variants[] = {
            {"no height map", false, 0.0f, 0.0f},
            {"POM, base 0.0", true, 0.0f, 0.0f},
            {"POM, base 0.5", true, 0.5f, 0.0f},
            {"POM, base 0.5 + self-shadow", true, 0.5f, 1.0f},
        };

        constexpr float spacing = 2.2f;
        int index = 0;
        for (const auto& v : variants) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
            material->setDiffuseMap(colorTex);
            material->setNormalMap(normalTex);
            material->setMetalness(0.0f);
            material->setGloss(0.35f);
            if (v.height) {
                material->setHeightMap(heightTex);
                material->setHeightMapFactor(0.1f);   // the 2.22 default
                material->setHeightMapBase(v.base);
                material->setHeightMapShadow(v.shadow);
            }
            _materials.push_back(material);

            const float x = (static_cast<float>(index) - 1.5f) * spacing;
            // A plane primitive faces +Y, so it is stood upright to face the
            // camera. Parallax is a grazing-angle effect and an upright quad seen
            // slightly from the side shows it without the quad shrinking to a
            // sliver, which is what a floor-plane layout does at this framing.
            Entity* quad = createPrimitive("plane", material.get(),
                Vector3(x, 0.0f, 0.0f), Vector3(1.7f, 1.0f, 1.7f));
            quad->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
            spdlog::info("Quad {}: {}", index + 1, v.label);
            ++index;
        }

        auto* camera = createCamera(Vector3(1.5f, 0.9f, 8.4f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setNearClip(0.05f);
            comp->camera()->setFarClip(100.0f);
            comp->camera()->setFov(45.0f);
        }
        auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f));
        controls->setOrbitDistance(8.5f);
        controls->storeResetState();

        spdlog::info("Left to right: flat, POM below the surface, POM about a "
                     "mid-grey base, and the same with self-shadowing.");
        return true;
    }

private:
    static Texture* textureOf(const std::unique_ptr<Asset>& asset)
    {
        const auto resource = asset->resource();
        if (!resource || !std::holds_alternative<Texture*>(*resource)) {
            return nullptr;
        }
        return std::get<Texture*>(*resource);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _color;
    std::unique_ptr<Asset> _normal;
    std::unique_ptr<Asset> _height;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
};

VISUTWIN_EXAMPLE_MAIN(ParallaxExample)
