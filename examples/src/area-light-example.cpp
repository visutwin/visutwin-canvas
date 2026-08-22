// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// LTC area-light demo (parity with upstream graphics/area-lights): three animated
// area lights — a white rect, a yellow sphere and a large blue "sky" disk — illuminate
// the statue.glb hero standing on a seaside-rocks textured floor, lit by the helipad
// environment atlas. Each light carries an emissive primitive matching its shape.
//
// DEVIATION: upstream builds these as spot/omni/directional lights with an area SHAPE
// (cone clipping, shadows, no distance falloff on the directional). This engine has a
// single positional LIGHTTYPE_AREA_RECT type (two-sided, range-windowed, no shadows),
// so all three are area lights; the directional disk is emulated by placing a large
// disk far away along the animated direction (same angular size as upstream's).
//
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// Emulated-directional disk: upstream places a disk of angular diameter
// scale/range = 0.2 rad at distance far=5000; we reproduce the same angular
// size at a nearer distance the positional area light can handle.
constexpr float kDiskDistance = 100.0f;
constexpr float kDiskSize = 0.2f * kDiskDistance;

class AreaLightExample final: public ExampleApp
{
public:
    AreaLightExample(): ExampleApp({.title = "Area Lights"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);

        // Skydome + image-based lighting from the helipad environment atlas (darkened),
        // matching the upstream counterpart.
        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(0.4f);

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
            spdlog::error("Failed to load helipad env atlas");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        // Seaside-rocks textured ground plane (color + normal + gloss), metallic PBR so the
        // LTC area lights produce glossy stretched reflections across it. Textures tiled 7x7.
        _floorColorTex = std::make_unique<Asset>(
            "floor-color", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-color.jpg"));
        _floorNormalTex = std::make_unique<Asset>(
            "floor-normal", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-normal.jpg"));
        _floorGlossTex = std::make_unique<Asset>(
            "floor-gloss", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-gloss.jpg"));

        Texture* floorColor = requireTexture(_floorColorTex, "floor-color");
        Texture* floorNormal = requireTexture(_floorNormalTex, "floor-normal");
        Texture* floorGloss = requireTexture(_floorGlossTex, "floor-gloss");
        if (!floorColor || !floorNormal || !floorGloss) {
            return false;
        }

        _floorMaterial = std::make_shared<StandardMaterial>();
        _floorMaterial->setName("seaside-rocks-floor");
        _floorMaterial->setUseMetalness(true);
        _floorMaterial->setMetalness(0.7f);
        _floorMaterial->setGloss(0.8f);
        _floorMaterial->setDiffuseMap(floorColor);
        _floorMaterial->setNormalMap(floorNormal);
        _floorMaterial->setGlossMap(floorGloss);
        _floorMaterial->setDiffuseMapTiling(Vector2(7.0f, 7.0f));
        _floorMaterial->setNormalMapTiling(Vector2(7.0f, 7.0f));
        _floorMaterial->setMetalnessMapTiling(Vector2(7.0f, 7.0f));

        auto* floor = new Entity();
        floor->setEngine(engine());
        floor->setLocalScale(20.0f, 20.0f, 20.0f);
        if (auto* render = static_cast<RenderComponent*>(floor->addComponent<RenderComponent>())) {
            render->setMaterial(_floorMaterial.get());
            render->setType("plane");
        }
        root()->addChild(floor);

        // Statue hero standing on the floor (upstream scale 0.4).
        _statue = std::make_unique<Asset>(
            "statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        const auto statueResource = _statue->resource();
        if (!statueResource) {
            spdlog::error("Failed to load statue model");
            return false;
        }
        auto* statueEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
        statueEntity->setLocalScale(0.4f, 0.4f, 0.4f);
        root()->addChild(statueEntity);

        // Camera matching upstream: pos (0, 2.5, 12), lookAt origin, fov 60, gray clear.
        // lookAt(0,0,0) from (0,2.5,12): pitch = -atan2(2.5, 12).
        _camera = createCamera(Vector3(0.0f, 2.5f, 12.0f), Vector3(-11.77f, 0.0f, 0.0f));
        if (auto* cameraComp = _camera->findComponent<CameraComponent>()) {
            cameraComp->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
            cameraComp->camera()->setFov(60.0f);
            cameraComp->camera()->setFarClip(100000.0f);
        }

        // Three lights matching upstream: white rect, yellow sphere, blue "sky" disk.
        _light1 = createAreaLight(
            AreaLightShape::LIGHTSHAPE_RECT, Vector3(-3.0f, 4.0f, 0.0f), 4.0f,
            Color(1.0f, 1.0f, 1.0f, 1.0f), 2.0f, 10.0f, _rectMaterial);
        _light2 = createAreaLight(
            AreaLightShape::LIGHTSHAPE_SPHERE, Vector3(5.0f, 2.0f, -2.0f), 2.0f,
            Color(1.0f, 1.0f, 0.0f, 1.0f), 2.0f, 10.0f, _sphereMaterial);
        _light3 = createAreaLight(
            AreaLightShape::LIGHTSHAPE_DISK, Vector3(0.0f, 0.0f, 0.0f), kDiskSize,
            Color(0.7f, 0.7f, 1.0f, 1.0f), 10.0f, 1000.0f, _diskMaterial);

        spdlog::info("Area lights: white rect + yellow sphere + blue sky disk over statue");
        spdlog::info("Keys: Space = pause/resume animation, Esc = quit");

        animateLights(0.0f);
        return true;
    }

    void update(const float dt) override
    {
        if (_animate) {
            _time += dt;
            animateLights(_time);
        }
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
            _animate = !_animate;
            spdlog::info("Animation {}", _animate ? "resumed" : "paused");
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

    static float lerp(const float a, const float b, const float t)
    {
        return a + (b - a) * t;
    }

    // Mirrors upstream createAreaLight: a parent entity carrying the area light
    // plus an emissive primitive matching the light-source shape.
    Entity* createAreaLight(const AreaLightShape shape, const Vector3& position,
        const float size, const Color& color, const float intensity, const float range,
        std::shared_ptr<StandardMaterial>& outMaterial) const
    {
        auto* lightParent = new Entity();
        lightParent->setEngine(engine());
        lightParent->setLocalPosition(position.getX(), position.getY(), position.getZ());
        root()->addChild(lightParent);

        auto* lightEntity = new Entity();
        lightEntity->setEngine(engine());
        if (auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_AREA_RECT);
            light->setAreaShape(shape);
            light->setColor(color);
            light->setIntensity(intensity);
            light->setRange(range);
            light->setAreaWidth(size);
            light->setAreaHeight(size);
        }
        lightParent->addChild(lightEntity);

        // Emissive primitive that is the light source color (upstream: emissive
        // material with lighting off; plane for rect, sphere, flattened cone for disk).
        auto brightMaterial = std::make_shared<StandardMaterial>();
        // The unlit path outputs base color (not emissive), so the source color
        // rides in diffuse.
        brightMaterial->setDiffuse(color);
        brightMaterial->setEmissive(color);
        brightMaterial->setUseLighting(false);
        brightMaterial->setCullMode(
            shape == AreaLightShape::LIGHTSHAPE_RECT ? CullMode::CULLFACE_NONE : CullMode::CULLFACE_BACK
        );
        outMaterial = brightMaterial;

        auto* brightShape = new Entity();
        brightShape->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(brightShape->addComponent<RenderComponent>())) {
            render->setMaterial(brightMaterial.get());
            render->setType(
                shape == AreaLightShape::LIGHTSHAPE_SPHERE ? "sphere" :
                shape == AreaLightShape::LIGHTSHAPE_DISK ? "cone" : "plane"
            );
        }
        brightShape->setLocalScale(
            size, shape == AreaLightShape::LIGHTSHAPE_DISK ? 0.001f : size, size
        );
        lightParent->addChild(brightShape);

        return lightParent;
    }

    // Per-frame animation mirroring the upstream update callback.
    void animateLights(const float t) const
    {
        const float factor1 = (std::sin(t) + 1.0f) * 0.5f;
        const float factor2 = (std::sin(t * 0.6f) + 1.0f) * 0.5f;
        const float factor3 = (std::sin(t * 0.4f) + 1.0f) * 0.5f;

        _light1->setLocalEulerAngles(lerp(-90.0f, 110.0f, factor1), 0.0f, 90.0f);
        _light1->setLocalPosition(-4.0f, lerp(2.0f, 4.0f, factor3), lerp(-2.0f, 2.0f, factor2));

        _light2->setLocalPosition(5.0f, lerp(1.0f, 3.0f, factor1), lerp(-2.0f, 2.0f, factor2));

        _light3->setLocalEulerAngles(
            lerp(230.0f, 310.0f, factor2), lerp(-30.0f, 0.0f, factor3), 90.0f
        );
        // Upstream: position = camera + lightY * far (the disk hangs in the sky
        // along its emission axis). Matrix4::getElement takes (col, row); Y axis = col 1.
        const auto& wt = _light3->worldTransform();
        const Vector3 dir(wt.getElement(1, 0), wt.getElement(1, 1), wt.getElement(1, 2));
        const Vector3 camPos = _camera->position();
        _light3->setLocalPosition(
            camPos.getX() + dir.getX() * kDiskDistance,
            camPos.getY() + dir.getY() * kDiskDistance,
            camPos.getZ() + dir.getZ() * kDiskDistance
        );
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _statue;
    std::unique_ptr<Asset> _floorColorTex;
    std::unique_ptr<Asset> _floorNormalTex;
    std::unique_ptr<Asset> _floorGlossTex;

    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::shared_ptr<StandardMaterial> _rectMaterial;
    std::shared_ptr<StandardMaterial> _sphereMaterial;
    std::shared_ptr<StandardMaterial> _diskMaterial;

    Entity* _camera = nullptr;
    Entity* _light1 = nullptr;
    Entity* _light2 = nullptr;
    Entity* _light3 = nullptr;

    bool _animate = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(AreaLightExample)
