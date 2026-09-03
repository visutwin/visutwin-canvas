// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of the upstream "materials/material-refraction" example.
//
// Two refractive capsules stand in a ring of small ones over a textured ground,
// with the camera orbiting them. Refraction is driven by the material's
// refraction/thickness properties and can be resolved two ways, toggled at
// runtime: the env-atlas fallback (default) or the mid-frame scene-colour grab
// (upstream's useDynamicRefraction). The metalness workflow toggles too —
// refraction works in both, but without metalness the Fresnel comes from the
// material's specular colour, which is black by default.
//
// Keys: 1 = dynamic refraction, 2 = metalness workflow, Space = auto-cycle.
//
// DEVIATIONS from upstream:
//  - Upstream drives the two toggles from UI checkboxes; there is no UI layer
//    here, so they are keys plus an auto-cycle that walks all four combinations.
//  - Upstream's `refraction` property is `transmissionFactor` here, and
//    `BLEND_NORMAL` is `setTransparent(true)`.
//
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class RefractionExample final: public ExampleApp
{
public:
    RefractionExample(): ExampleApp({.title = "Material Refraction"}) {}

protected:
    bool create() override
    {
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

        _rocksColor = std::make_unique<Asset>(
            "diffuse", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-color.jpg"));
        _rocksNormal = std::make_unique<Asset>(
            "normal", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-normal.jpg"));
        _rocksGloss = std::make_unique<Asset>(
            "other", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-gloss.jpg"));

        Texture* diffuseTex = requireTexture(_rocksColor, "diffuse");
        Texture* normalTex = requireTexture(_rocksNormal, "normal");
        Texture* otherTex = requireTexture(_rocksGloss, "other");
        if (!diffuseTex || !normalTex || !otherTex) {
            return false;
        }

        // The depth layer is where the framebuffer is copied to a texture for the
        // following layers. Move it after World AND Skybox so the grab captures both;
        // by default it sits between them, which would leave the sky out of the
        // refracted image.
        if (const auto layers = scene()->layers()) {
            if (const auto depthLayer = layers->getLayerById(LAYERID_DEPTH)) {
                layers->remove(depthLayer);
                layers->insertOpaque(depthLayer, 2);
            }
        }

        auto* cameraEntity = createCamera(Vector3(0.0f, 0.0f, 3.0f), Vector3(0.0f, 0.0f, 0.0f));
        _camera = cameraEntity;
        if (auto* cameraComponent = cameraEntity->findComponent<CameraComponent>()) {
            cameraComponent->setToneMapping(TONEMAP_ACES);
            _cameraComponent = cameraComponent;
        }

        createDirectionalLight(Vector3(85.0f, -100.0f, 0.0f), Color(1.0f, 0.8f, 0.25f, 1.0f));

        // Ground
        auto groundMaterial = std::make_shared<StandardMaterial>();
        groundMaterial->setName("ground");
        groundMaterial->setDiffuse(Color(1.0f, 2.5f, 2.5f, 1.0f));
        groundMaterial->setDiffuseMap(diffuseTex);
        groundMaterial->setGloss(0.4f);
        groundMaterial->setMetalness(0.5f);
        groundMaterial->setUseMetalness(true);
        _materials.push_back(groundMaterial);
        createPrimitive("box", groundMaterial.get(),
            Vector3(0.0f, -2.0f, 0.0f), Vector3(30.0f, 1.0f, 30.0f));

        // Basic refractive material. Low metalness, otherwise it turns reflective.
        _material = std::make_shared<StandardMaterial>();
        _material->setName("refractive");
        _material->setMetalness(0.0f);
        _material->setGloss(1.0f);
        _material->setGlossMap(otherTex);
        _material->setGlossMapChannel(MapChannel::MAP_CHANNEL_G);
        _material->setUseMetalness(true);
        _material->setTransmissionFactor(0.8f);           // upstream: refraction
        _material->setRefractionIndex(1.0f / 1.33f);      // water
        _material->setTransparent(true);                  // upstream: BLEND_NORMAL
        _material->setThickness(0.4f);
        _material->setThicknessMap(otherTex);
        _materials.push_back(_material);

        // Clone and apply the second material's extra settings.
        _material2 = std::static_pointer_cast<StandardMaterial>(_material->clone());
        _material2->setName("refractive-2");
        _material2->setDiffuse(Color(0.9f, 0.6f, 0.6f, 1.0f));
        _material2->setNormalMap(normalTex);
        _material2->setBumpiness(2.0f);
        _material2->setRefractionMap(diffuseTex);
        _materials.push_back(_material2);

        // Two main objects with refraction materials
        createObject(-0.5f, 0.0f, 0.0f, _material.get(), 0.7f);
        createObject(0.5f, 0.0f, 0.0f, _material2.get(), 0.7f);

        // A ring of objects with a simple color material as a background
        auto objMaterial = std::make_shared<StandardMaterial>();
        objMaterial->setName("ring");
        objMaterial->setDiffuse(Color(0.5f, 0.5f, 2.5f, 1.0f));
        objMaterial->setGloss(0.5f);
        objMaterial->setMetalness(0.5f);
        objMaterial->setUseMetalness(true);
        _materials.push_back(objMaterial);
        constexpr int count = 8;
        for (int i = 0; i < count; ++i) {
            const float angle = (static_cast<float>(i) / count) * 2.0f * 3.14159265358979f;
            createObject(std::cos(angle) * 2.5f, -0.3f, std::sin(angle) * 2.5f,
                objMaterial.get(), 0.2f);
        }

        spdlog::info("Material refraction: two refractive capsules over a textured ground.");
        spdlog::info("Keys: 1 = dynamic refraction, 2 = metalness workflow, "
                     "Space = auto-cycle, Esc = quit");

        applyToggles();
        return true;
    }

    void update(const float dt) override
    {
        // Rotate the camera around the objects.
        _time += dt;
        _camera->setLocalPosition(3.0f * std::sin(_time * 0.5f), 0.0f, 3.0f * std::cos(_time * 0.5f));
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        if (_autoCycle) {
            _cycleTimer += dt;
            if (_cycleTimer >= 4.0f) {
                _cycleTimer = 0.0f;
                // Walk the four combinations of the two upstream toggles.
                _phase = (_phase + 1) % 4;
                _dynamic = (_phase & 1) != 0;
                _metalness = (_phase & 2) == 0;
                applyToggles();
            }
        }
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        if (event.key.key == SDLK_1) {
            _autoCycle = false;
            _dynamic = !_dynamic;
            applyToggles();
            return true;
        }
        if (event.key.key == SDLK_2) {
            _autoCycle = false;
            _metalness = !_metalness;
            applyToggles();
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

    void createObject(const float x, const float y, const float z,
        Material* material, const float scale)
    {
        createPrimitive("capsule", material, Vector3(x, y, z), Vector3(scale, scale, scale));
    }

    void applyToggles()
    {
        _material->setUseDynamicRefraction(_dynamic);
        _material2->setUseDynamicRefraction(_dynamic);

        // Dynamic refraction reads the scene colour grab, which only exists when the
        // camera asks for it. requestSceneColorMap is a REFERENCE COUNT, not a
        // boolean setter — several effects can want the grab at once — so it takes
        // one call per change of mind, not one per toggle evaluation. Passing the
        // flag straight through drove the count to -1 on the first call (the initial
        // state is off) and tripped its assert.
        if (_cameraComponent && _dynamic != _sceneColorMapRequested) {
            _cameraComponent->requestSceneColorMap(_dynamic);
            _sceneColorMapRequested = _dynamic;
        }

        _material->setUseMetalness(_metalness);
        _material2->setUseMetalness(_metalness);

        spdlog::info("Refraction: {} | workflow: {}",
            _dynamic ? "dynamic (scene colour grab)" : "env atlas",
            _metalness ? "metalness" : "specular");
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _rocksColor;
    std::unique_ptr<Asset> _rocksNormal;
    std::unique_ptr<Asset> _rocksGloss;

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _material;
    std::shared_ptr<StandardMaterial> _material2;

    Entity* _camera = nullptr;
    CameraComponent* _cameraComponent = nullptr;

    float _time = 0.0f;
    bool _autoCycle = true;
    bool _dynamic = false;    // upstream initial UI value
    bool _sceneColorMapRequested = false;   // what the camera was last asked for
    bool _metalness = true;   // upstream initial UI value
    int _phase = 0;
    float _cycleTimer = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(RefractionExample)
