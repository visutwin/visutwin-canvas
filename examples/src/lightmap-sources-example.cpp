// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream test/lightmap-sources (a hidden test example, added with the
// change that gave a mesh instance its own lightmap slot).
//
// The two sources of a lightmap side by side, each group a box floating above its
// own ground plane:
//   1. "mesh instance": both meshes share ONE plain material and are baked by the
//      lightmapper, which stores each result on its mesh instance;
//   2. "material": a material with a lightmap assigned (clouds.jpg standing in for a
//      baked one, so it is obvious who reads it), not baked;
//   3. "both": that same material, baked as well — the mesh instance lightmap wins.
// A baked directional light with an area, plus baked ambient, gives the box a soft
// shadow on its plane that is visibly IN the lightmap. The scene is static.
//
// What it holds: group 1's plane and box share a material, so before lightmaps
// belonged to mesh instances the plane showed the box's bake (or the other way
// round) — whichever was applied last. Group 3 must show the bake, not clouds.
//
// DEVIATIONS:
// - The bake is the GpuLightmapper (upstream's own UV-space technique); its options
//   carry upstream's scene.lightmap* and ambientBake* values where they exist. It has
//   no lightmap filter, and no occlusion brightness/contrast for the ambient bake.
// - Upstream's shadowBias 0.2 is in its own units; the authoring value here is 0.05,
//   as in lightmap-bake (0.2 pushes casters away far enough to lose small shadows).
// - Built-in primitives here mirror UV0 into UV1, where upstream's box unwraps each
//   face into its own padded cell. The six faces of the box therefore share one
//   lightmap square, and the box's own bake is a blend of all of them; the PLANE's
//   UV1 is the same in both engines, which is why the planes carry the comparison.
// - Labels are text elements scaled into world units (ElementComponent::setFontSize
//   takes an int), in Liberation Sans, metric-compatible with upstream's Arial.
//
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/input/elementInput.h"
#include "framework/lightmapper/gpuLightmapper.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

class LightmapSourcesExample final : public ExampleApp
{
public:
    LightmapSourcesExample()
        : ExampleApp({.title = "Lightmap Sources"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ElementComponentSystem>();
        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
    }

    bool create() override
    {
        _lightmapAsset = std::make_unique<Asset>("lightmap", AssetType::TEXTURE,
            assetPath("textures/clouds.jpg"));
        const auto lightmapResource = _lightmapAsset->resource();
        if (!lightmapResource) {
            spdlog::error("Failed to load clouds.jpg");
            return false;
        }
        Texture* assignedLightmap = std::get<Texture*>(*lightmapResource);
        _font = std::make_unique<Asset>("label-font", AssetType::FONT, assetPath("fonts/liberation-sans.json"));

        // A dim sky ambient. A lightmap is taken to carry the ambient light already, so
        // none of the groups adds it at runtime: the baked groups get it baked in, and
        // the material-only group is lit by its assigned lightmap alone.
        scene()->setAmbientLight(0.16f, 0.18f, 0.24f);

        // The material of the meshes that take their lightmap from the mesh instance.
        _plain = std::make_shared<StandardMaterial>();
        _plain->setDiffuse(Color(0.8f, 0.8f, 0.8f));
        _plain->setGloss(0.3f);

        // The same material with a lightmap assigned on it, as a glb or the Editor
        // would. The lightmap samples uv1.
        _withLightmap = std::make_shared<StandardMaterial>();
        _withLightmap->setDiffuse(Color(0.8f, 0.8f, 0.8f));
        _withLightmap->setGloss(0.3f);
        _withLightmap->setLightMap(assignedLightmap);

        createGroup("mesh instance\n(runtime bake)", -3.6f, _plain.get(), true);
        createGroup("material\n(assigned texture)", 0.0f, _withLightmap.get(), false);
        createGroup("both\n(instance wins)", 3.6f, _withLightmap.get(), true);

        // A baked light: it affects lightmapped objects only (MASK_BAKE, upstream's
        // affectDynamic false / affectLightmapped true / bake true), and its area
        // spreads the shadow it bakes.
        auto* lightEntity = new Entity();
        lightEntity->setName("baked light");
        lightEntity->setEngine(engine());
        root()->addChild(lightEntity);
        lightEntity->setLocalEulerAngles(50.0f, -62.0f, 0.0f);
        auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
        light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        light->setColor(Color(1.0f, 0.8f, 0.55f));
        light->setIntensity(1.8f);
        light->setCastShadows(true);
        light->setShadowBias(0.05f);
        light->setShadowNormalBias(0.05f);
        light->setShadowDistance(50.0f);
        light->setShadowResolution(1024);
        light->setShadowType(SHADOW_PCF3_32F);
        light->setMask(MASK_BAKE);

        auto* camera = createCamera(Vector3(0.0f, 4.6f, 11.0f));
        if (auto* cameraComponent = camera->findComponent<CameraComponent>();
            cameraComponent && cameraComponent->camera()) {
            cameraComponent->camera()->setClearColor(Color(0.05f, 0.06f, 0.08f, 1.0f));
        }
        camera->lookAt(Vector3(0.0f, 0.7f, 0.0f));

        // Bake once the scene is complete.
        GpuLightmapper::Options options;
        options.sizeMultiplier = 256.0f;            // scene.lightmapSizeMultiplier
        options.maxResolution = 2048;               // scene.lightmapMaxResolution
        options.directionalBakeNumSamples = 24;     // light.bakeNumSamples
        options.directionalBakeArea = 25.0f;        // light.bakeArea
        options.ambientBake = true;                 // scene.ambientBake
        options.ambientBakeNumSamples = 20;
        options.ambientBakeSpherePart = 0.4f;
        options.bakeCameraTarget = Vector3(0.0f, 0.7f, 0.0f);
        options.bakeCameraDistance = 20.0f;
        _baker = std::make_unique<GpuLightmapper>(engine());
        _baker->bake(_bakeTargets, options);
        return true;
    }

    void preRender() override
    {
        _elementInput->syncTextElements();
    }

    void postRender() override
    {
        if (_baker && _baker->baking() && _baker->update()) {
            spdlog::info("Lightmap sources: baked {} mesh instance(s)", _bakeTargets.size());
        }
    }

    void destroy() override
    {
        _baker.reset();
    }

private:
    // A box floating above its own ground plane, both rendered with `material`. When
    // lightmapped, the baker bakes both and the box casts a shadow onto the plane.
    // DEVIATION: upstream parents the three entities to a group root at x; here they
    // are placed at x directly under the scene root (createPrimitive's parent).
    void createGroup(const std::string& label, const float x, Material* material, const bool lightmapped)
    {
        const auto addMesh = [&](const char* type, const Vector3& position, const Vector3& scale,
                                 const bool castsIntoBake) {
            auto* entity = createPrimitive(type, material, position + Vector3(x, 0.0f, 0.0f), scale);
            auto* render = entity->findComponent<RenderComponent>();
            if (!render) {
                return;
            }
            // Upstream's castShadows false / castShadowsLightmap: there is no realtime
            // light here, so casting is only ever into the bake.
            render->setCastShadows(castsIntoBake);
            if (lightmapped) {
                for (auto* meshInstance : render->meshInstances()) {
                    _bakeTargets.push_back(meshInstance);
                }
            }
        };
        addMesh("plane", Vector3(0.0f, 0.0f, 0.0f), Vector3(3.2f, 1.0f, 3.2f), false);
        addMesh("box", Vector3(0.0f, 1.25f, 0.0f), Vector3(1.1f, 1.1f, 1.1f), lightmapped);

        createLabel(label, Vector3(x, 2.45f, 0.0f));
    }

    void createLabel(const std::string& message, const Vector3& position)
    {
        constexpr int kFontSize = 64;
        constexpr float kFontSizeWorld = 0.16f;     // upstream fontSize 0.16
        const float scale = kFontSizeWorld / static_cast<float>(kFontSize);

        FontResource* fontResource = nullptr;
        if (const auto res = _font->resource();
            res.has_value() && std::holds_alternative<FontResource*>(*res)) {
            fontResource = std::get<FontResource*>(*res);
        }
        if (!fontResource) {
            spdlog::warn("liberation-sans.json failed to load; the labels will be missing");
            return;
        }

        auto* text = new Entity();
        text->setEngine(engine());
        if (auto* element = static_cast<ElementComponent*>(text->addComponent<ElementComponent>())) {
            element->setType(ElementType::Text);
            element->setAnchor(Vector4(0.5f, 0.5f, 0.5f, 0.5f));
            element->setPivot(Vector2(0.5f, 0.5f));
            element->setFontResource(fontResource);
            element->setFontSize(kFontSize);
            element->setWidth(static_cast<float>(kFontSize) * 12.0f);
            element->setHeight(static_cast<float>(kFontSize) * 2.0f);
            element->setHorizontalAlign(ElementHorizontalAlign::Center);
            element->setText(message);
        }
        text->setLocalPosition(position);
        text->setLocalScale(scale, scale, scale);
        root()->addChild(text);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _lightmapAsset;
    std::unique_ptr<Asset> _font;
    std::shared_ptr<StandardMaterial> _plain;
    std::shared_ptr<StandardMaterial> _withLightmap;
    std::vector<MeshInstance*> _bakeTargets;
    std::unique_ptr<GpuLightmapper> _baker;
};

VISUTWIN_EXAMPLE_MAIN(LightmapSourcesExample)
