// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Local light PCSS demo (VisuTwin counterpart of upstream's
// contact-hardening-shadows test example): a Draco-compressed robot-arm.glb
// stands on a receiver floor lit by a spot light (warm, left) and an omni
// light (cool, right), both casting contact-hardening soft shadows (upstream
// shadowPCSS.js port). Auto-cycles PCF <-> PCSS on both lights every 3 s —
// with PCSS the arm's shadow sharpens at its base and softens with distance.
// The helipad env atlas provides the ambient/specular IBL.
// Keys: 1 = PCF, 2 = PCSS, Space = auto-cycle, Esc = quit.
//
#include <algorithm>
#include <memory>
#include <variant>

#include <core/shape/boundingBox.h>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class PcssLocalExample final: public ExampleApp
{
public:
    PcssLocalExample(): ExampleApp({.title = "Local PCSS Shadows"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.0f, 0.0f, 0.0f);

        // Helipad env atlas for image-based ambient/specular (upstream sets a low
        // skybox intensity so the local lights dominate the shading).
        _helipadAsset = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false}
        );
        if (const auto helipadResource = _helipadAsset->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));
            scene()->setSkyboxMip(1.0f);
            scene()->setSkyboxIntensity(0.1f);
        } else {
            spdlog::warn("Failed to load helipad env atlas — continuing without IBL");
        }

        createCamera(Vector3(0.0f, 7.0f, 14.0f), Vector3(-24.0f, 0.0f, 0.0f));

        // Receiver-only floor (a large flat ground; a big caster would inflate the
        // shadow depth fits). Metallic + low gloss to match the upstream floor look.
        _floorMaterial = std::make_shared<StandardMaterial>();
        _floorMaterial->setDiffuse(Color(0.5f, 0.5f, 0.52f, 1.0f));
        _floorMaterial->setMetalness(0.7f);
        _floorMaterial->setGlossInvert(false);
        _floorMaterial->setGloss(0.25f);
        auto* floor = createPrimitive("plane", _floorMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(60.0f, 1.0f, 60.0f));
        if (auto* render = floor->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
            render->setReceiveShadows(true);
        }

        createRobotArm();

        // Spot light (warm) over the left of the arm.
        // Emission is the node's -Y axis (straight down). Hover above-left and
        // slightly behind the arm so its shadow stretches toward the camera.
        auto* spotLight = new Entity();
        spotLight->setEngine(engine());
        _spot = static_cast<LightComponent*>(spotLight->addComponent<LightComponent>());
        if (_spot) {
            _spot->setType(LightType::LIGHTTYPE_SPOT);
            _spot->setColor(Color(1.0f, 0.95f, 0.85f, 1.0f));
            _spot->setIntensity(2.6f);
            _spot->setRange(30.0f);
            _spot->setInnerConeAngle(35.0f);
            _spot->setOuterConeAngle(55.0f);
            _spot->setCastShadows(true);
            _spot->setShadowResolution(1024);
            _spot->setShadowBias(0.0005f);
            _spot->setShadowNormalBias(0.02f);
            _spot->setPenumbraSize(30.0f);  // local-light scale: search px on the shadow map
        }
        spotLight->setLocalPosition(-4.0f, 9.0f, -1.5f);
        root()->addChild(spotLight);

        // Omni light (cool) to the right of the arm.
        auto* omniLight = new Entity();
        omniLight->setEngine(engine());
        _omni = static_cast<LightComponent*>(omniLight->addComponent<LightComponent>());
        if (_omni) {
            _omni->setType(LightType::LIGHTTYPE_OMNI);
            _omni->setColor(Color(0.7f, 0.85f, 1.0f, 1.0f));
            _omni->setIntensity(2.2f);
            _omni->setRange(25.0f);
            _omni->setCastShadows(true);
            _omni->setShadowResolution(1024);
            _omni->setShadowBias(0.0005f);
            _omni->setPenumbraSize(30.0f);
        }
        omniLight->setLocalPosition(4.0f, 6.5f, 2.0f);
        root()->addChild(omniLight);

        spdlog::info("Local PCSS: robot arm lit by spot (left, warm) + omni (right, cool), PCF <-> PCSS every 3 s");
        spdlog::info("Keys: 1 = PCF, 2 = PCSS, Space = auto-cycle, Esc = quit");

        applyMode(false);
        return true;
    }

    void update(const float dt) override
    {
        if (_autoCycle) {
            _cycleTimer += dt;
            if (_cycleTimer >= 3.0f) {
                _cycleTimer = 0.0f;
                _usePcss = !_usePcss;
                applyMode(_usePcss);
            }
        }
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_1:
            _autoCycle = false;
            _usePcss = false;
            applyMode(false);
            return true;
        case SDLK_2:
            _autoCycle = false;
            _usePcss = true;
            applyMode(true);
            return true;
        case SDLK_SPACE:
            _autoCycle = true;
            _cycleTimer = 0.0f;
            return true;
        default:
            return false;
        }
    }

private:
    // Union AABB over every mesh instance owned by (or descended from) an entity —
    // used to auto-scale/position the loaded GLB hero regardless of its authored size.
    static BoundingBox calcEntityAABB(Entity* entity)
    {
        BoundingBox bbox;
        bbox.setCenter(0, 0, 0);
        bbox.setHalfExtents(0, 0, 0);
        if (!entity) {
            return bbox;
        }
        bool hasAny = false;
        for (auto* render : RenderComponent::instances()) {
            if (!render || !render->entity()) {
                continue;
            }
            auto* owner = render->entity();
            if (owner != entity && !owner->isDescendantOf(entity)) {
                continue;
            }
            for (auto* mi : render->meshInstances()) {
                if (!mi) {
                    continue;
                }
                if (!hasAny) { bbox = mi->aabb(); hasAny = true; }
                else { bbox.add(mi->aabb()); }
            }
        }
        return bbox;
    }

    // Hero shadow caster: Draco-compressed robot-arm.glb, auto-scaled from its
    // AABB to ~6 units tall and dropped so its base rests on the floor (y = 0).
    // Keep the Asset alive for the whole program — the instantiated render
    // entity references meshes/materials owned by the container resource.
    void createRobotArm()
    {
        _armAsset = std::make_unique<Asset>(
            "robot-arm",
            AssetType::CONTAINER,
            assetPath("models/robot-arm.glb")
        );

        Entity* robotArm = nullptr;
        if (const auto armResource = _armAsset->resource();
            armResource && std::holds_alternative<ContainerResource*>(*armResource)) {
            if (auto* container = std::get<ContainerResource*>(*armResource)) {
                robotArm = container->instantiateRenderEntity();
            }
        }
        if (!robotArm) {
            spdlog::error("Failed to load/instantiate robot-arm.glb — is Draco enabled?");
            return;
        }

        robotArm->setEngine(engine());
        root()->addChild(robotArm);

        // Enable shadow casting/receiving on every render component in the tree.
        for (auto* render : RenderComponent::instances()) {
            if (!render || !render->entity()) {
                continue;
            }
            auto* owner = render->entity();
            if (owner == robotArm || owner->isDescendantOf(robotArm)) {
                render->setCastShadows(true);
                render->setReceiveShadows(true);
            }
        }

        const auto bbox = calcEntityAABB(robotArm);
        const auto& he = bbox.halfExtents();
        const auto& ct = bbox.center();
        const float maxExtent = std::max({he.getX(), he.getY(), he.getZ()}) * 2.0f;
        const float targetHeight = 6.0f;
        const float s = (maxExtent > 0.001f) ? (targetHeight / maxExtent) : 3.0f;
        // The AABB was measured with the model's own root scale applied, so compose
        // with it rather than replacing it (instantiateRenderEntity returns the glTF
        // root node itself for single-root scenes).
        const float s0 = robotArm->localScale().getX();
        robotArm->setLocalScale(s0 * s, s0 * s, s0 * s);
        // Recenter horizontally and lift so the scaled AABB minimum sits at y = 0.
        const float minY = (ct.getY() - he.getY()) * s;
        robotArm->setLocalPosition(-ct.getX() * s, -minY, -ct.getZ() * s);
        spdlog::info("robot-arm.glb: extent={:.2f}, scale={:.3f}", maxExtent, s);
    }

    void applyMode(const bool pcss) const
    {
        if (_spot) _spot->setShadowType(pcss ? SHADOW_PCSS_32F : SHADOW_PCF3_32F);
        if (_omni) _omni->setShadowType(pcss ? SHADOW_PCSS_32F : SHADOW_PCF3_32F);
        spdlog::info("Local shadow mode: {}", pcss ? "PCSS (contact-hardening)" : "PCF 3x3");
    }

    std::unique_ptr<Asset> _helipadAsset;
    std::unique_ptr<Asset> _armAsset;
    std::shared_ptr<StandardMaterial> _floorMaterial;

    LightComponent* _spot = nullptr;
    LightComponent* _omni = nullptr;

    bool _usePcss = false;
    bool _autoCycle = true;
    float _cycleTimer = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(PcssLocalExample)
