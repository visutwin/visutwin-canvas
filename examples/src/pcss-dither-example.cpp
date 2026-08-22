// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of the upstream "dithered-transparency" example.
//
// Two glass tables, both alpha-blended AND opacity-dithered, showing that the two
// strengths are independent. LEFT leaves alphaDither unset, so opacity drives both the
// blend and the dither density — the coupled behaviour every material had before.
// RIGHT sets alphaDither explicitly, so opacity drives only the blend and alphaDither
// only the dither. Both tables also dither their SHADOWS, so a half-opaque table throws
// a correspondingly thinned shadow rather than a solid one.
//
// Values are frozen at upstream's initial state (opacity 0.5, alphaDither 0.5, TAA off).
// Upstream drives them from sliders, and at these values both tables deliberately look
// identical — the difference only appears once a slider moves.
//
// DEVIATION: the shadow dither runs in the Metal shadow fragment shader, which serves both
// the PCF and VSM paths. On Vulkan the PCF shadow pass is depth-only and omits the fragment
// stage entirely, so a dithered shadow there would need one attached; this example uses VSM,
// whose fragment stage does run.
//
// @credit Low-poly Glass Table by Sketchfab, CC BY 4.0
//   https://sketchfab.com/3d-models/low-poly-glass-table-6acac6d9201e448b92dff859b6f63aad
//
#include <memory>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

// Upstream's initial slider values.
constexpr float INITIAL_OPACITY = 0.5f;
constexpr float INITIAL_ALPHA_DITHER = 0.5f;
constexpr bool INITIAL_TAA = false;

class PcssDitherExample final: public ExampleApp
{
public:
    PcssDitherExample(): ExampleApp({.title = "Dithered Transparency"}) {}

protected:
    bool create() override
    {
        // Setup skydome
        _envAtlasAsset = std::make_unique<Asset>(
            "env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        if (const auto envResource = _envAtlasAsset->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*envResource));
        } else {
            spdlog::error("Failed to load the table-mountain environment atlas");
        }
        scene()->setSkyboxMip(2);
        scene()->setExposure(4.5f);

        _diffuseAsset = std::make_unique<Asset>(
            "color", AssetType::TEXTURE, assetPath("textures/playcanvas.png"));

        Texture* diffuseTexture = nullptr;
        if (const auto diffuseResource = _diffuseAsset->resource()) {
            diffuseTexture = std::get<Texture*>(*diffuseResource);
        }

        // Ground plane.
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.8f, 0.8f, 0.8f, 1.0f));
        _groundMaterial->setDiffuseMap(diffuseTexture);
        createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(60.0f, 1.0f, 30.0f));

        _tableAsset = std::make_unique<Asset>(
            "table", AssetType::CONTAINER, assetPath("models/glass-table.glb"));
        const auto tableResource = _tableAsset->resource();
        if (!tableResource) {
            spdlog::error("Failed to load models/glass-table.glb");
            return false;
        }
        auto* tableContainer = std::get<ContainerResource*>(*tableResource);

        // LEFT — alphaDither stays unset, so opacity drives both blend strength and dither
        // density, exactly like the legacy behaviour.
        auto leftMaterials = spawnTable(tableContainer, Vector3(-7.0f, 0.0f, 0.0f));

        // RIGHT — alphaDither set explicitly, decoupled from opacity.
        auto rightMaterials = spawnTable(tableContainer, Vector3(7.0f, 0.0f, 0.0f));

        // Everything except alphaDither applies to both tables, so the only difference between
        // them is that unset-vs-explicit state.
        const auto applyShared = [](const std::vector<StandardMaterial*>& materials) {
            for (auto* material : materials) {
                material->setOpacity(INITIAL_OPACITY);
                material->setOpacityDitherMode(DitherMode::DITHER_BAYER8);
                material->setOpacityShadowDitherMode(DitherMode::DITHER_BAYER8);
            }
        };
        applyShared(leftMaterials);
        applyShared(rightMaterials);
        for (auto* material : rightMaterials) {
            material->setAlphaDither(INITIAL_ALPHA_DITHER);
        }

        // Directional light casting a soft VSM shadow.
        auto* light = createDirectionalLight(Vector3(75.0f, 120.0f, 20.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lc = light->findComponent<LightComponent>()) {
            lc->setRange(200.0f);
            lc->setShadowResolution(2048);
            lc->setShadowType(ShadowType::SHADOW_VSM_16F);
            lc->setVsmBlurSize(20);
            lc->setShadowBias(0.1f);
            lc->setShadowNormalBias(0.1f);
        }

        // Camera.
        auto* cameraEntity = createCamera(Vector3(-14.0f, 12.0f, 20.0f));

        const Vector3 focusPoint(0.0f, 4.0f, 0.0f);
        if (auto* cameraComponent = cameraEntity->findComponent<CameraComponent>()) {
            if (cameraComponent->camera()) {
                cameraComponent->camera()->setFov(70.0f);
            }
            // Upstream's CameraFrame: ACES tone mapping, a scene color map, TAA jitter 1, and
            // sharpening only while TAA is on.
            cameraComponent->requestSceneColorMap(true);   // upstream: cameraFrame.rendering.sceneColorMap
            auto taa = cameraComponent->taa();
            taa.enabled = INITIAL_TAA;
            taa.jitter = 1.0f;
            cameraComponent->setTaa(taa);
            auto rendering = cameraComponent->rendering();
            rendering.toneMapping = TONEMAP_ACES;
            rendering.sharpness = INITIAL_TAA ? 1.0f : 0.0f;
            cameraComponent->setRendering(rendering);
            cameraComponent->setToneMapping(TONEMAP_ACES);
        }
        cameraEntity->lookAt(focusPoint);

        addOrbitControls(cameraEntity, focusPoint);

        spdlog::info("Dithered transparency: left = dither coupled to opacity, right = decoupled "
                     "(alphaDither {:.2f}). Esc quits.", INITIAL_ALPHA_DITHER);

        return true;
    }

private:
    // Instantiate the glass table and collect its alpha-blended materials. Each blended
    // material is CLONED — a container's materials are shared across instantiations, so
    // without this the two tables could not be configured independently.
    std::vector<StandardMaterial*> spawnTable(ContainerResource* container, const Vector3& position)
    {
        auto* entity = container->instantiateRenderEntity();
        entity->setEngine(engine());
        entity->setLocalScale(3.0f, 3.0f, 3.0f);
        entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
        root()->addChild(entity);

        std::vector<StandardMaterial*> materials;
        for (auto* render : entity->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                auto* source = meshInstance->material();
                if (!source || !source->transparent()) {
                    continue;
                }
                auto clone = source->clone();
                meshInstance->setMaterial(clone.get());
                _clonedMaterials.push_back(clone);
                if (auto* standard = dynamic_cast<StandardMaterial*>(clone.get())) {
                    materials.push_back(standard);
                }
            }
        }
        return materials;
    }

    std::unique_ptr<Asset> _envAtlasAsset;
    std::unique_ptr<Asset> _tableAsset;
    std::unique_ptr<Asset> _diffuseAsset;

    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::vector<std::shared_ptr<Material>> _clonedMaterials;   // keeps the clones alive
};

VISUTWIN_EXAMPLE_MAIN(PcssDitherExample)
