// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Dynamic grab-pass refraction demo: a glass sphere refracts the mid-frame scene
// color grab, distorting the colorful column backdrop behind it. Auto-cycles between
// dynamic (grab-pass) refraction and the env-atlas fallback every few seconds
// (1 = dynamic, 2 = env atlas, Space = auto-cycle).
//
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class RefractionExample final: public ExampleApp
{
public:
    RefractionExample(): ExampleApp({.title = "Dynamic Refraction"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.25f, 0.25f, 0.28f);

        // The helipad environment atlas gives the env-atlas refraction/reflection path
        // real content to sample (mirrors the upstream material-refraction example).
        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        const auto helipadResource = _helipad->resource();
        if (!helipadResource) {
            spdlog::error("Failed to load helipad texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        // Seaside-rocks PBR maps texture the backdrop so refraction shows a realistic
        // rocky background rather than flat synthetic color.
        _rocksColor = std::make_unique<Asset>(
            "rocks-color", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-color.jpg"));
        _rocksNormal = std::make_unique<Asset>(
            "rocks-normal", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-normal.jpg"));
        _rocksGloss = std::make_unique<Asset>(
            "rocks-gloss", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-gloss.jpg"));

        Texture* rocksColorTex = requireTexture(_rocksColor, "rocks-color");
        Texture* rocksNormalTex = requireTexture(_rocksNormal, "rocks-normal");
        Texture* rocksGlossTex = requireTexture(_rocksGloss, "rocks-gloss");
        if (!rocksColorTex || !rocksNormalTex || !rocksGlossTex) {
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 1.2f, 6.0f), Vector3(-4.0f, 0.0f, 0.0f));

        // The dynamic-refraction grab pass only runs when the camera publishes the
        // scene color map.
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->requestSceneColorMap(true);
        }

        createDirectionalLight(Vector3(50.0f, -30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.4f);

        // Colorful column backdrop — sharp vertical color edges make the refraction
        // distortion unmistakable.
        const Color columnColors[6] = {
            Color(0.9f, 0.15f, 0.15f, 1.0f), Color(0.95f, 0.6f, 0.1f, 1.0f),
            Color(0.9f, 0.85f, 0.1f, 1.0f), Color(0.15f, 0.7f, 0.25f, 1.0f),
            Color(0.15f, 0.4f, 0.9f, 1.0f), Color(0.6f, 0.2f, 0.8f, 1.0f)
        };
        for (int i = 0; i < 6; ++i) {
            auto material = std::make_shared<StandardMaterial>();
            material->setName("column-" + std::to_string(i));
            // Tinted seaside-rocks: the color map keeps a rocky backdrop while the per-column
            // tint preserves sharp color edges that make the refraction distortion obvious.
            material->setDiffuse(columnColors[i]);
            material->setDiffuseMap(rocksColorTex);
            material->setNormalMap(rocksNormalTex);
            material->setBumpiness(1.0f);
            material->setGlossMap(rocksGlossTex);
            material->setMetalness(0.0f);
            material->setGloss(0.4f);
            _materials.push_back(material);
            createPrimitive("box", material.get(),
                Vector3(-3.75f + 1.5f * static_cast<float>(i), 1.0f, -2.5f),
                Vector3(1.2f, 4.0f, 0.6f));
        }

        // Rocky floor (seaside-rocks color+normal+gloss), like the upstream ground.
        auto floorMaterial = std::make_shared<StandardMaterial>();
        floorMaterial->setName("floor");
        floorMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
        floorMaterial->setDiffuseMap(rocksColorTex);
        floorMaterial->setNormalMap(rocksNormalTex);
        floorMaterial->setBumpiness(1.0f);
        floorMaterial->setGlossMap(rocksGlossTex);
        floorMaterial->setMetalness(0.0f);
        floorMaterial->setGloss(0.4f);
        _materials.push_back(floorMaterial);
        createPrimitive("plane", floorMaterial.get(), Vector3(0.0f, -1.0f, 0.0f),
            Vector3(24.0f, 1.0f, 24.0f));

        // The glass sphere: transparent so it renders after the depth-layer grab.
        _glassMaterial = std::make_shared<StandardMaterial>();
        _glassMaterial->setName("glass");
        _glassMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _glassMaterial->setMetalness(0.0f);
        _glassMaterial->setGlossInvert(true);
        _glassMaterial->setGloss(0.05f);  // near-mirror smooth
        _glassMaterial->setTransmissionFactor(1.0f);
        _glassMaterial->setRefractionIndex(1.5f);
        _glassMaterial->setThickness(0.8f);
        _glassMaterial->setUseDynamicRefraction(true);
        _glassMaterial->setTransparent(true);
        createPrimitive("sphere", _glassMaterial.get(), Vector3(0.0f, 1.0f, 0.8f),
            Vector3(2.4f, 2.4f, 2.4f));

        spdlog::info("Dynamic refraction: glass sphere over colorful columns");
        spdlog::info("Keys: 1 = dynamic, 2 = +dispersion, 3 = +volume attenuation, 4 = env atlas, Space = auto-cycle, Esc = quit");

        applyMode(0);
        return true;
    }

    void update(const float dt) override
    {
        if (_autoCycle) {
            _cycleTimer += dt;
            if (_cycleTimer >= 3.0f) {
                _cycleTimer = 0.0f;
                applyMode((_phase + 1) % 4);
            }
        }
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        if (event.key.key >= SDLK_1 && event.key.key <= SDLK_4) {
            _autoCycle = false;
            applyMode(static_cast<int>(event.key.key - SDLK_1));
            return true;
        }
        if (event.key.key == SDLK_SPACE) {
            _autoCycle = true;
            _cycleTimer = 0.0f;
            spdlog::info("Auto-cycle resumed");
            return true;
        }
        return false;
    }

private:
    static Texture* requireTexture(const std::unique_ptr<Asset>& asset, const char* label)
    {
        const auto resource = asset->resource();
        if (!resource) {
            spdlog::error("Failed to load texture asset '{}'", label);
            return nullptr;
        }
        return std::get<Texture*>(*resource);
    }

    // Phases: 0 = dynamic grab, 1 = dynamic + dispersion (KHR_materials_dispersion),
    // 2 = dynamic + Beer-law volume attenuation (KHR_materials_volume), 3 = env atlas.
    void applyMode(const int newPhase)
    {
        _phase = newPhase;
        _glassMaterial->setUseDynamicRefraction(_phase != 3);
        _glassMaterial->setDispersion(_phase == 1 ? 10.0f : 0.0f);
        if (_phase == 2) {
            _glassMaterial->setAttenuationColor(Color(0.9f, 0.3f, 0.1f, 1.0f));
            _glassMaterial->setAttenuationDistance(0.35f);
        } else {
            _glassMaterial->setAttenuationDistance(0.0f);
        }
        static const char* names[4] = {
            "dynamic grab pass", "dynamic + dispersion",
            "dynamic + volume attenuation (orange Beer-law)", "env atlas"};
        spdlog::info("Refraction mode: {}", names[_phase]);
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _rocksColor;
    std::unique_ptr<Asset> _rocksNormal;
    std::unique_ptr<Asset> _rocksGloss;

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _glassMaterial;

    bool _autoCycle = true;
    int _phase = 0;
    float _cycleTimer = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(RefractionExample)
