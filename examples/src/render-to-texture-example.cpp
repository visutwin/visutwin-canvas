// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Render-to-texture: a second "sensor" camera renders the world into an offscreen
// RGBA8 target, which an emissive monitor plane in the UI layer displays. The
// sensor camera orbits and the scene objects animate, so the monitor shows a live
// feed. The ground also carries a baked lightmap sampled at UV1.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

constexpr int MONITOR_LAYER_ID = LAYERID_UI;

class RenderToTextureExample final: public ExampleApp
{
public:
    RenderToTextureExample(): ExampleApp({.title = "Render-To-Texture Sensor View"}) {}

protected:
    bool create() override
    {
        scene()->setSkyboxMip(0);
        // Lower exposure so the baked lightmap pools stay below clipping and read clearly.
        scene()->setExposure(0.4f);

        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );

        // KTX2 (Basis Universal UASTC) — transcoded to ASTC 4x4 at load time, exercising
        // the compressed-texture pipeline (checkboard.png remains as the uncompressed source).
        _checkerboard = std::make_unique<Asset>(
            "checkerboard", AssetType::TEXTURE, assetPath("textures/checkboard.ktx2"));

        // Baked lightmap (warm/cool light pools) sampled at UV1 on the ground plane.
        _lightmapAsset = std::make_unique<Asset>(
            "lightmap-pools",
            AssetType::TEXTURE,
            assetPath("textures/lightmap-pools.tga"),
            AssetData{ .mipmaps = false }
        );

        const auto helipadResource = _helipad->resource();
        const auto checkerResource = _checkerboard->resource();
        if (!helipadResource || !checkerResource) {
            spdlog::error("Failed to load required textures");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        _checkerMaterial = std::make_shared<StandardMaterial>();
        _checkerMaterial->setDiffuse(Color(0.8f, 0.8f, 0.8f, 1.0f));
        _checkerMaterial->setDiffuseMap(std::get<Texture*>(*checkerResource));

        // Baked lightmap: adds warm/cool light pools to the ground's indirect diffuse.
        if (const auto lightmapResource = _lightmapAsset->resource();
            lightmapResource && std::holds_alternative<Texture*>(*lightmapResource)) {
            _checkerMaterial->setLightMap(std::get<Texture*>(*lightmapResource));
        }

        _redMaterial = std::make_shared<StandardMaterial>();
        _redMaterial->setDiffuse(Color(1.0f, 0.1f, 0.1f, 1.0f));
        _cyanMaterial = std::make_shared<StandardMaterial>();
        _cyanMaterial->setDiffuse(Color(0.1f, 1.0f, 1.0f, 1.0f));
        _yellowMaterial = std::make_shared<StandardMaterial>();
        _yellowMaterial->setDiffuse(Color(1.0f, 1.0f, 0.1f, 1.0f));

        // World layer scene objects.
        createPrimitive("plane", _checkerMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(20.0f, 20.0f, 20.0f), {LAYERID_WORLD});
        _sphere = createPrimitive("sphere", _redMaterial.get(), Vector3(-2.0f, 1.0f, 0.0f),
            Vector3(2.0f, 2.0f, 2.0f), {LAYERID_WORLD});
        _cone = createPrimitive("cone", _cyanMaterial.get(), Vector3(0.0f, 1.0f, -2.0f),
            Vector3(2.0f, 2.0f, 2.0f), {LAYERID_WORLD});
        _box = createPrimitive("box", _yellowMaterial.get(), Vector3(2.0f, 1.0f, 0.0f),
            Vector3(2.0f, 2.0f, 2.0f), {LAYERID_WORLD});

        // Offscreen sensor texture and render target.
        TextureOptions sensorTextureOptions;
        sensorTextureOptions.name = "SensorRT";
        sensorTextureOptions.width = 512;
        sensorTextureOptions.height = 256;
        sensorTextureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        sensorTextureOptions.mipmaps = false;
        sensorTextureOptions.minFilter = FilterMode::FILTER_LINEAR;
        sensorTextureOptions.magFilter = FilterMode::FILTER_LINEAR;
        _sensorTexture = std::make_shared<Texture>(device().get(), sensorTextureOptions);
        _sensorTexture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        _sensorTexture->setAddressV(ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions sensorRtOptions;
        sensorRtOptions.graphicsDevice = device().get();
        sensorRtOptions.colorBuffer = _sensorTexture.get();
        sensorRtOptions.depth = true;
        sensorRtOptions.name = "SensorRenderTarget";
        auto sensorRenderTarget = device()->createRenderTarget(sensorRtOptions);

        // Monitor mesh in UI layer so it is visible to main camera only (texture camera excludes UI).
        _monitorMaterial = std::make_shared<StandardMaterial>();
        _monitorMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _monitorMaterial->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _monitorMaterial->setEmissiveMap(_sensorTexture.get());
        auto* monitor = createPrimitive("plane", _monitorMaterial.get(),
            Vector3(6.0f, 8.0f, -5.0f), Vector3(20.0f, 10.0f, 10.0f), {MONITOR_LAYER_ID});
        monitor->setLocalEulerAngles(90.0f, 0.0f, 0.0f);

        // Main camera renders world + skybox + UI(defaults from CameraComponent).
        auto* mainCameraEntity = createCamera(Vector3(0.0f, 9.0f, 15.0f));
        if (auto* mainCamera = mainCameraEntity->findComponent<CameraComponent>();
            mainCamera && mainCamera->camera()) {
            mainCamera->camera()->setFov(100.0f);
            mainCamera->camera()->setClearColor(Color(0.2f, 0.2f, 0.25f, 1.0f));
        }
        mainCameraEntity->lookAt(Vector3(1.0f, 4.0f, 0.0f));

        // Texture camera renders only world + skybox into the sensor target.
        _textureCameraEntity = createCamera(Vector3(0.0f, 2.0f, 5.0f));
        if (auto* textureCamera = _textureCameraEntity->findComponent<CameraComponent>();
            textureCamera && textureCamera->camera()) {
            textureCamera->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
            textureCamera->camera()->setRenderTarget(sensorRenderTarget);
            textureCamera->camera()->setClearColor(Color(0.05f, 0.05f, 0.05f, 1.0f));
        }
        _textureCameraEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        // World omni light (shared by both cameras through world layer).
        _light = new Entity();
        _light->setEngine(engine());
        if (auto* lightComp = static_cast<LightComponent*>(_light->addComponent<LightComponent>())) {
            lightComp->setType(LightType::LIGHTTYPE_OMNI);
            lightComp->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            lightComp->setRange(200.0f);
            lightComp->setIntensity(5.0f);
            lightComp->setLayers({LAYERID_WORLD});
        }
        _light->setLocalPosition(0.0f, 2.0f, 5.0f);
        root()->addChild(_light);

        return true;
    }

    void update(const float dt) override
    {
        _time += std::clamp(dt, 0.0f, 0.1f);
        const float time = _time;

        // Animate sensor camera and scene objects to validate live monitor feed.
        _textureCameraEntity->setLocalPosition(8.0f * std::sin(time * 0.9f), 4.0f, 8.0f * std::cos(time * 0.9f));
        _textureCameraEntity->lookAt(Vector3(0.0f, 1.0f, 0.0f));

        _sphere->setLocalPosition(-2.0f, 1.0f + 0.35f * std::sin(time * 1.8f), 0.0f);
        _cone->setLocalEulerAngles(0.0f, time * 45.0f, 0.0f);
        _box->setLocalEulerAngles(time * 30.0f, time * 20.0f, 0.0f);

        _light->setLocalPosition(6.0f * std::sin(time * 0.7f), 3.0f, 6.0f * std::cos(time * 0.7f));
    }

private:
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _checkerboard;
    std::unique_ptr<Asset> _lightmapAsset;

    std::shared_ptr<StandardMaterial> _checkerMaterial;
    std::shared_ptr<StandardMaterial> _redMaterial;
    std::shared_ptr<StandardMaterial> _cyanMaterial;
    std::shared_ptr<StandardMaterial> _yellowMaterial;
    std::shared_ptr<StandardMaterial> _monitorMaterial;
    std::shared_ptr<Texture> _sensorTexture;

    Entity* _sphere = nullptr;
    Entity* _cone = nullptr;
    Entity* _box = nullptr;
    Entity* _light = nullptr;
    Entity* _textureCameraEntity = nullptr;

    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(RenderToTextureExample)
