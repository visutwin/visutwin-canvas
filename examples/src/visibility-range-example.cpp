// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
// An original scene: upstream has no visibility ranges, so there is no example to port. It
// shows MeshInstance::setVisibilityRange choosing a level of detail by camera distance and
// crossfading neighbouring levels with complementary dithers.
//
// A field of 5 x 9 markers, 6 units apart, on a grey ground 80 units square (receiving
// shadows, casting none). Each marker is 1.6 units across on every axis and is
// three mesh instances at one spot, coloured so the level shows: a green sphere (the near
// level, drawn up to 12 units and fading out by 16), a yellow cylinder (fading in over 12-16,
// out over 28-32) and a red box (fading in over 28-32, drawn beyond). The distance is from the
// camera to the centre of each instance's bounds. A directional light of 1.2 turned (-50, 30, 0)
// casts shadows, and a marker's shadow is that of the level the camera sees. Ambient 0.3.
//
// The camera, 4 units up and looking down the field, dollies between z = 14 and z = -30 and
// back over 20 seconds, so the bands sweep through the field: nearest green, then yellow, then
// red, with a dithered crossfade wherever a marker sits in a band.
//
// The same field drawn instanced (key 3) is one hardware-instanced mesh per level, 45 instances
// each, with a PER-INSTANCE range: each instance is measured from its own origin and faded or
// hidden in the vertex stage, and must look exactly like the separate entities. Under TAA (key T)
// the dither changes with the jitter every frame, so the crossfade resolves to a smooth blend.
//
// Keys:
//   1  bands 4 units wide / hard switches at 14 and 30 (bands)
//   2  pause / resume the dolly
//   3  separate entities / one instanced mesh per level (separate)
//   T  TAA on / off (off)
// VISUTWIN_VR_INSTANCED=1 starts instanced and VISUTWIN_VR_TAA=1 with TAA (for captures).
//
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "scene/visibilityRange.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kColumns = 5;
    constexpr int kRows = 9;
    constexpr float kSpacing = 6.0f;
    constexpr float kCameraHeight = 4.0f;
    constexpr float kDollyNear = 14.0f;
    constexpr float kDollyFar = -30.0f;
    constexpr float kDollyPeriod = 20.0f;   // seconds, there and back

    // Where each level changes to the next, and the width of its band.
    constexpr std::array<float, 2> kSwitchDistances = {14.0f, 30.0f};
    constexpr float kBandWidth = 4.0f;
    constexpr float kMarkerSize = 1.6f;
}

class VisibilityRangeExample final : public ExampleApp
{
public:
    VisibilityRangeExample() : ExampleApp({.title = "Visibility Range"}) {}

protected:
    bool create() override
    {
        scene()->setAmbientLight(0.3f, 0.3f, 0.3f);

        const auto makeMaterial = [this](const Color& color) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(color);
            material->setUseMetalness(true);
            material->setMetalness(0.0f);
            material->setGloss(0.4f);
            _materials.push_back(material);
            return material.get();
        };
        StandardMaterial* ground = makeMaterial(Color(0.55f, 0.55f, 0.55f, 1.0f));
        const std::array<StandardMaterial*, 3> levelMaterials = {makeMaterial(Color(0.2f, 0.8f, 0.25f, 1.0f)),
            makeMaterial(Color(0.95f, 0.8f, 0.15f, 1.0f)), makeMaterial(Color(0.9f, 0.2f, 0.15f, 1.0f))};
        const std::array<const char*, 3> levelShapes = {"sphere", "cylinder", "box"};

        Entity* floor = createPrimitive("plane", ground, Vector3(0.0f, 0.0f, -20.0f), Vector3(80.0f, 1.0f, 80.0f));
        floor->setName("Ground");
        // Receives shadows only: as a caster the ground shadows itself in rings.
        if (auto* render = floor->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }

        // A uniform scale, so the instanced draws (which carry normals by the bare 3x3) shade the
        // markers as the separate entities (inverse transpose) do.
        const Vector3 markerScale(kMarkerSize, kMarkerSize, kMarkerSize);
        std::vector<uint8_t> instanceBytes;
        for (int row = 0; row < kRows; ++row) {
            for (int column = 0; column < kColumns; ++column) {
                const Vector3 position((static_cast<float>(column) - (kColumns - 1) * 0.5f) * kSpacing, 1.0f,
                    -static_cast<float>(row) * kSpacing);
                for (size_t level = 0; level < levelShapes.size(); ++level) {
                    Entity* marker = createPrimitive(levelShapes[level], levelMaterials[level], position,
                        markerScale);
                    marker->setName("Marker-" + std::to_string(row) + "-" + std::to_string(column) + "-" +
                        std::to_string(level));
                    _separate.push_back(marker);
                    if (auto* render = marker->findComponent<RenderComponent>()) {
                        for (MeshInstance* meshInstance : render->meshInstances()) {
                            _levels.push_back({meshInstance, static_cast<int>(level), false});
                        }
                    }
                }
                const Matrix4 matrix = Matrix4::trs(position, Quaternion(), markerScale);
                const size_t offset = instanceBytes.size();
                instanceBytes.resize(offset + VertexFormat::INSTANCING_MATRIX_SIZE);
                std::memcpy(instanceBytes.data() + offset, &matrix, VertexFormat::INSTANCING_MATRIX_SIZE);
            }
        }
        // One instanced mesh per level, all 45 markers in one draw.
        for (size_t level = 0; level < levelShapes.size(); ++level) {
            Entity* field = createPrimitive(levelShapes[level], levelMaterials[level], Vector3(0.0f, 0.0f, 0.0f),
                Vector3(1.0f, 1.0f, 1.0f));
            field->setName(std::string("InstancedField-") + std::to_string(level));
            VertexBufferOptions options;
            options.data = instanceBytes;
            auto buffer = device()->createVertexBuffer(VertexFormat::defaultInstancingFormat(),
                kRows * kColumns, options);
            if (auto* render = field->findComponent<RenderComponent>()) {
                for (MeshInstance* meshInstance : render->meshInstances()) {
                    meshInstance->setInstancing(buffer, kRows * kColumns);
                    _levels.push_back({meshInstance, static_cast<int>(level), true});
                }
            }
            _instanced.push_back(field);
        }

        Entity* light = createDirectionalLight(Vector3(-50.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.2f);
        if (auto* lightComponent = light->findComponent<LightComponent>()) {
            lightComponent->setCastShadows(true);
            lightComponent->setShadowDistance(60.0f);
            lightComponent->setShadowResolution(2048);
        }

        Entity* camera = createCamera(Vector3(0.0f, kCameraHeight, kDollyNear));
        _cameraEntity = camera;
        _camera = camera->findComponent<CameraComponent>();
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.55f, 0.65f, 0.8f, 1.0f));
        placeCamera();

        if (const char* value = std::getenv("VISUTWIN_VR_INSTANCED"); value && value[0] == '1') {
            _useInstanced = true;
        }
        if (const char* value = std::getenv("VISUTWIN_VR_TAA"); value && value[0] == '1') {
            _taa = true;
        }
        applyMode();
        applyRanges();
        spdlog::info("Keys: 1 crossfade bands / hard switches, 2 pause the dolly, 3 instanced, T TAA");
        return true;
    }

    void update(const float dt) override
    {
        if (!_paused) {
            _time += dt;
            placeCamera();
        }
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        if (keyboard->wasPressed(Key::Digit1)) {
            _bands = !_bands;
            applyRanges();
        } else if (keyboard->wasPressed(Key::Digit2)) {
            _paused = !_paused;
        } else if (keyboard->wasPressed(Key::Digit3)) {
            _useInstanced = !_useInstanced;
            applyMode();
        } else if (keyboard->wasPressed(Key::T)) {
            _taa = !_taa;
            applyMode();
        }
    }

private:
    struct Level
    {
        MeshInstance* meshInstance;
        int level;
        bool instanced;
    };

    void applyMode()
    {
        for (Entity* entity : _separate) {
            entity->setEnabled(!_useInstanced);
        }
        for (Entity* entity : _instanced) {
            entity->setEnabled(_useInstanced);
        }
        if (_camera) {
            _camera->setTaaEnabled(_taa);
        }
        spdlog::info("Markers drawn {}, TAA {}", _useInstanced ? "instanced (a range per instance)" :
            "as separate entities", _taa ? "on" : "off");
    }

    void placeCamera()
    {
        // A cosine from the near end to the far one and back: slow at the ends.
        const float phase = 0.5f - 0.5f * std::cos(_time / kDollyPeriod * 2.0f * std::numbers::pi_v<float>);
        const float z = kDollyNear + (kDollyFar - kDollyNear) * phase;
        _cameraEntity->setLocalPosition(0.0f, kCameraHeight, z);
        _cameraEntity->lookAt(Vector3(0.0f, 1.0f, z - 20.0f));
    }

    // Level i is drawn from switch i - 1 to switch i; each band is centred on its switch.
    void applyRanges()
    {
        const float half = _bands ? kBandWidth * 0.5f : 0.0f;
        for (const Level& entry : _levels) {
            VisibilityRange range;
            if (entry.level > 0) {
                const float at = kSwitchDistances[static_cast<size_t>(entry.level - 1)];
                range.fadeInStart = at - half;
                range.fadeInEnd = at + half;
            }
            if (entry.level < static_cast<int>(kSwitchDistances.size())) {
                const float at = kSwitchDistances[static_cast<size_t>(entry.level)];
                range.fadeOutStart = at - half;
                range.fadeOutEnd = at + half;
            }
            range.perInstance = entry.instanced;
            entry.meshInstance->setVisibilityRange(range);
        }
        spdlog::info("Levels switch at {} and {} units, {}", kSwitchDistances[0], kSwitchDistances[1],
            _bands ? "crossfading over 4 units" : "with no crossfade");
    }

    Entity* _cameraEntity = nullptr;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Level> _levels;

    float _time = 0.0f;
    bool _bands = true;
    bool _paused = false;
    bool _useInstanced = false;
    bool _taa = false;
    CameraComponent* _camera = nullptr;
    std::vector<Entity*> _separate;
    std::vector<Entity*> _instanced;
};

VISUTWIN_EXAMPLE_MAIN(VisibilityRangeExample)
