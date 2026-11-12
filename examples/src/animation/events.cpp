// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream animation/events.
//
// Animation events: the bitmoji character walks a circle (radius 3, 13.8 degrees a second) over a
// floor of 11 x 11 boxes, and its walk track carries two events named "foot_step", at a tenth and
// at six tenths of its duration, as each foot touches the ground, each naming the bone of that foot.
// The anim component fires the event as playback passes it; the handler lights the box under the
// foot's bone with a random colour (normalised, emissive intensity 50), which fades by 5% a frame,
// the five most recent boxes fading at once. The walk plays at 0.62 speed. The camera at (0, 1, 0)
// follows the character, through a camera frame (4x MSAA, bloom 0.01, neutral tone mapping), under
// the table mountain environment (skybox mip 2, intensity 0.4, exposure 2).
//
// DEVIATION: the box colours come from a fixed-seed generator, so every run lights them alike.
//
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "animClips.h"
#include "exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/parsers/glbContainerResource.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class AnimEventsExample final : public ExampleApp
{
public:
    AnimEventsExample() : ExampleApp({}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        _modelAsset = std::make_unique<Asset>("model", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _walkAsset = std::make_unique<Asset>("walkAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/walk.glb"));
        _envAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        auto* container = _modelAsset->resourceAs<ContainerResource>();
        Texture* env = _envAsset->resourceAs<Texture>();
        auto walkTrack = firstAnimTrack(_walkAsset.get());
        if (!container || !env || !walkTrack) {
            spdlog::error("animation/events needs bitmoji.glb, its walk animation and the table mountain atlas");
            return false;
        }

        // Setup skydome
        scene()->setExposure(2.0f);
        scene()->setSkyboxMip(2);
        scene()->setEnvAtlas(env);
        scene()->setSkyboxIntensity(0.4f);

        // A camera with a camera frame: neutral tone mapping, 4x MSAA and a little bloom
        _camera = createCamera(Vector3(0.0f, 1.0f, 0.0f));
        auto* cameraComponent = _camera->findComponent<CameraComponent>();
        cameraComponent->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        cameraComponent->setToneMapping(TONEMAP_NEUTRAL);
        auto rendering = cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.01f;
        cameraComponent->setRendering(rendering);

        // A floor made up of box models
        for (int i = -5; i <= 5; ++i) {
            for (int j = -5; j <= 5; ++j) {
                auto material = std::make_shared<StandardMaterial>();
                material->setDiffuse(Color(0.7f, 0.7f, 0.7f, 1.0f));
                material->setGloss(0.3f);
                material->setMetalness(0.2f);
                material->setUseMetalness(true);
                createPrimitive("box", material.get(), Vector3(static_cast<float>(i), -0.5f, static_cast<float>(j)),
                    Vector3(0.95f, 1.0f, 0.95f));
                _boxes[{i, j}] = material;
            }
        }

        // The character, on a parent that turns it round the centre of the scene
        _model = container->instantiateRenderEntity();
        _model->setEngine(engine());
        _model->setLocalPosition(-3.0f, 0.0f, 0.0f);
        _modelParent = new Entity();
        _modelParent->setEngine(engine());
        _modelParent->addChild(_model);
        root()->addChild(_modelParent);

        // Two events on the walk track, one for each foot step, just as each foot touches the ground
        const float duration = walkTrack->duration();
        walkTrack->setEvents({
            AnimEvent{"foot_step", duration * 0.1f, {{"bone", "R_foot0002_bind_JNT"}}},
            AnimEvent{"foot_step", duration * 0.6f, {{"bone", "L_foot0002_bind_JNT"}}},
        });
        _walkTrack = walkTrack;

        // The walk on the anim component, at a defined speed
        _anim = static_cast<AnimComponent*>(_model->addComponent<AnimComponent>());
        _anim->assignAnimation("Walk", walkTrack, {}, 0.62f);
        _anim->on("foot_step", [this](const AnimEventFired& fired) {
            // Light up the box under the foot's bone
            if (!fired.event) {
                return;
            }
            const auto bone = fired.event->properties.find("bone");
            if (bone == fired.event->properties.end()) {
                return;
            }
            if (GraphNode* node = _model->findByName(bone->second)) {
                highlightBox(node->position());
            }
        });
        return true;
    }

    void update(const float dt) override
    {
        // Turn the model round the centre of the scene
        _modelParent->rotate(0.0f, 13.8f * dt, 0.0f);

        // Fade the lit boxes, keeping the five most recent
        for (const auto& material : _highlighted) {
            material->setEmissiveIntensity(material->emissiveIntensity() * 0.95f);
        }
        while (_highlighted.size() > 5) {
            _highlighted.pop_front();
        }

        // Follow the model with the camera
        Vector3 target = _model->position();
        target = Vector3(target.getX(), 0.5f, target.getZ());
        _camera->lookAt(target);
    }

private:
    // Light up the box at a position with a random colour, through the emissive property
    void highlightBox(const Vector3& position)
    {
        const int i = static_cast<int>(std::floor(position.getX() + 0.5f));
        const int j = static_cast<int>(std::floor(position.getZ() + 0.5f));
        const auto box = _boxes.find({i, j});
        if (box == _boxes.end()) {
            return;
        }
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        Vector3 color(unit(_random), unit(_random), unit(_random));
        color = color * (1.0f / std::max(color.length(), 1e-6f));
        box->second->setEmissive(Color(color.getX(), color.getY(), color.getZ(), 1.0f));
        box->second->setEmissiveIntensity(50.0f);
        _highlighted.push_back(box->second);
    }

    std::unique_ptr<Asset> _modelAsset;
    std::unique_ptr<Asset> _walkAsset;
    std::unique_ptr<Asset> _envAsset;
    std::shared_ptr<AnimTrack> _walkTrack;
    std::map<std::pair<int, int>, std::shared_ptr<StandardMaterial>> _boxes;
    std::deque<std::shared_ptr<StandardMaterial>> _highlighted;
    Entity* _camera = nullptr;
    Entity* _model = nullptr;
    Entity* _modelParent = nullptr;
    AnimComponent* _anim = nullptr;
    std::mt19937 _random{7u};
};

VISUTWIN_EXAMPLE_MAIN(AnimEventsExample)
