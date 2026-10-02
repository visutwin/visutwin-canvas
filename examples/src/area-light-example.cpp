// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream graphics/area-lights: three animated lights with an area SHAPE — a
// white rect on a shadowed spot, a yellow sphere on an omni and a blue "sky" disk on a
// shadowed directional light, held 5000 units from the camera along its direction —
// illuminate the statue.glb hero standing on a seaside-rocks textured floor, lit by the
// helipad environment atlas. Each light carries an emissive primitive matching its
// shape, and the spot a black back face. Clustered area lights are enabled, as upstream.
//
// DEVIATION: upstream loads the LTC LUTs from a JSON asset; this engine has them built in.
//
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// Upstream `far`: the directional disk light is held this far from the camera.
constexpr float kFar = 5000.0f;

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

        // Area lights are disabled by default for clustered lighting.
        scene()->lighting().areaLightsEnabled = true;

        // Three lights matching upstream: white rect, yellow sphere, blue "sky" disk.
        _light1 = createAreaLight(LightType::LIGHTTYPE_SPOT, LightShape::LIGHTSHAPE_RECT,
            Vector3(-3.0f, 4.0f, 0.0f), 4.0f, Color(1.0f, 1.0f, 1.0f, 1.0f), 2.0f, true, 10.0f);
        _light2 = createAreaLight(LightType::LIGHTTYPE_OMNI, LightShape::LIGHTSHAPE_SPHERE,
            Vector3(5.0f, 2.0f, -2.0f), 2.0f, Color(1.0f, 1.0f, 0.0f, 1.0f), 2.0f, false, 10.0f);
        _light3 = createAreaLight(LightType::LIGHTTYPE_DIRECTIONAL, LightShape::LIGHTSHAPE_DISK,
            Vector3(0.0f, 0.0f, 0.0f), 0.2f, Color(0.7f, 0.7f, 1.0f, 1.0f), 10.0f, true, kFar);

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

    // Upstream createAreaLight: a parent entity carrying the light, scaled to the source's
    // size, plus an emissive primitive matching the shape (and for a spot, a black
    // primitive facing the other way).
    Entity* createAreaLight(const LightType type, const LightShape shape, const Vector3& position,
        const float scale, const Color& color, const float intensity, const bool shadows,
        const float range)
    {
        auto* lightParent = new Entity();
        lightParent->setEngine(engine());
        lightParent->setLocalPosition(position.getX(), position.getY(), position.getZ());
        root()->addChild(lightParent);

        auto* lightEntity = new Entity();
        lightEntity->setEngine(engine());
        if (auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>())) {
            light->setType(type);
            light->setShape(shape);
            light->setColor(color);
            light->setIntensity(intensity);
            light->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
            light->setRange(range);
            light->setCastShadows(shadows);
            light->setInnerConeAngle(80.0f);
            light->setOuterConeAngle(85.0f);
            light->setShadowBias(0.1f);
            light->setShadowNormalBias(0.1f);
            light->setShadowResolution(2048);
        }
        lightEntity->setLocalScale(scale, scale, scale);
        lightParent->addChild(lightEntity);

        const bool directional = type == LightType::LIGHTTYPE_DIRECTIONAL;
        const char* primitive = shape == LightShape::LIGHTSHAPE_SPHERE ? "sphere"
            : shape == LightShape::LIGHTSHAPE_DISK ? "cone" : "plane";
        const CullMode cull = shape == LightShape::LIGHTSHAPE_RECT ? CullMode::CULLFACE_NONE
                                                                   : CullMode::CULLFACE_BACK;

        // Emissive material that is the light source colour.
        auto brightMaterial = std::make_shared<StandardMaterial>();
        brightMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        brightMaterial->setEmissive(color);
        brightMaterial->setUseLighting(false);
        brightMaterial->setCullMode(cull);
        _materials.push_back(brightMaterial);

        auto* brightShape = new Entity();
        brightShape->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(brightShape->addComponent<RenderComponent>())) {
            render->setMaterial(brightMaterial.get());
            render->setType(primitive);
            render->setCastShadows(!directional);
        }
        const float extent = directional ? scale * range : scale;
        brightShape->setLocalScale(extent, shape == LightShape::LIGHTSHAPE_DISK ? 0.001f : extent, extent);
        lightParent->addChild(brightShape);

        // A black primitive on the back of a spot's source.
        if (type == LightType::LIGHTTYPE_SPOT) {
            auto blackMaterial = std::make_shared<StandardMaterial>();
            blackMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
            blackMaterial->setUseLighting(false);
            blackMaterial->setCullMode(cull);
            _materials.push_back(blackMaterial);

            auto* blackShape = new Entity();
            blackShape->setEngine(engine());
            if (auto* render = static_cast<RenderComponent*>(blackShape->addComponent<RenderComponent>())) {
                render->setMaterial(blackMaterial.get());
                render->setType(primitive);
            }
            blackShape->setLocalPosition(0.0f, 0.01f / scale, 0.0f);
            blackShape->setLocalEulerAngles(-180.0f, 0.0f, 0.0f);
            brightShape->addChild(blackShape);
        }

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
        // along its emission axis).
        const Vector3 dir(_light3->worldTransform().getColumn(1));
        _light3->setPosition(_camera->position() + dir * kFar);
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _statue;
    std::unique_ptr<Asset> _floorColorTex;
    std::unique_ptr<Asset> _floorNormalTex;
    std::unique_ptr<Asset> _floorGlossTex;

    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;

    Entity* _camera = nullptr;
    Entity* _light1 = nullptr;
    Entity* _light2 = nullptr;
    Entity* _light3 = nullptr;

    bool _animate = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(AreaLightExample)
