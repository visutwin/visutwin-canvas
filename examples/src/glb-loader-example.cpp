// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream loaders/glb.
//
// Loads geometry-camera-light.glb, a grey cube on a scaled plane that also carries
// two cameras (one perspective, one orthographic) and two KHR_lights_punctual
// lights (a blue point light and an orange spot). The glb's cameras start
// disabled; each gets automatic aspect ratio and a physical exposure of aperture
// 4, shutter 1/100 and sensitivity 500, the lights are all enabled, and the active
// camera switches every two seconds.
//
// DEVIATION: the engine's GLB parser imports neither glTF cameras nor
// KHR_lights_punctual lights — it creates the node entities, by name, and nothing
// else. This port therefore reads the file's JSON chunk itself and adds the
// camera and light components to those node entities exactly as upstream's parser
// builds them: cameras disabled, glTF near/far/yfov/ymag; lights disabled, range
// 9999 when unset, inverse-squared falloff, cone angles converted to degrees, and
// the light on a child entity turned 90 degrees about X so it shines down the
// node's -Z as glTF specifies. Upstream parents each camera on a child entity
// with no transform of its own, so putting it on the node itself is equivalent.
//
// DEVIATION: the engine has no Scene::physicalUnits and no camera aperture,
// shutter or sensitivity. What physical units change for this scene is reproduced
// by hand: upstream converts a glTF intensity I (candela) to luminance
// I * conversion and back to intensity luminance / conversion = I, so each light
// gets the glb's raw intensity; and the scene exposure is set to the value
// upstream's Camera::getExposure computes from the three camera settings,
// 1 / (1.2 * 2^log2(N^2 / t * 100 / S)). Both cameras share those settings, so a
// scene-wide exposure is exact.
//
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/camera.h"

using namespace visutwin::canvas;

namespace
{
    constexpr const char* kModel = "models/geometry-camera-light.glb";

    // Upstream's physical camera settings for the glb's cameras.
    constexpr float kAperture = 4.0f;
    constexpr float kShutter = 1.0f / 100.0f;
    constexpr float kSensitivity = 500.0f;

    constexpr float kSwitchInterval = 2.0f;
    constexpr float kRadToDeg = 57.29577951308232f;

    /// Upstream Camera::getExposure.
    float physicalExposure(const float aperture, const float shutter, const float sensitivity)
    {
        const float ev100 = std::log2((aperture * aperture) / shutter * 100.0f / sensitivity);
        return 1.0f / (std::pow(2.0f, ev100) * 1.2f);
    }

    /// The JSON chunk of a binary glTF file, or a null value when it is not one.
    nlohmann::json readGlbJson(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.size() < 20) {
            return {};
        }
        const auto u32 = [&bytes](const size_t offset) {
            return static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset])) |
                static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8 |
                static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16 |
                static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24;
        };
        const uint32_t jsonLength = u32(12);
        if (u32(0) != 0x46546C67u || u32(16) != 0x4E4F534Au || 20 + static_cast<size_t>(jsonLength) > bytes.size()) {
            return {};
        }
        return nlohmann::json::parse(bytes.begin() + 20, bytes.begin() + 20 + jsonLength, nullptr, false);
    }
}

class GlbLoaderExample final: public ExampleApp
{
public:
    GlbLoaderExample(): ExampleApp({.title = "GLB"}) {}

protected:
    bool create() override
    {
        _asset = std::make_unique<Asset>("scene", AssetType::CONTAINER, assetPath(kModel));
        const auto resource = _asset->resource();
        if (!resource || !std::holds_alternative<ContainerResource*>(*resource) ||
            !std::get<ContainerResource*>(*resource)) {
            spdlog::error("Failed to load {}", kModel);
            return false;
        }

        // Create an instance using render component.
        Entity* entity = std::get<ContainerResource*>(*resource)->instantiateRenderEntity();
        if (!entity) {
            spdlog::error("Failed to instantiate {}", kModel);
            return false;
        }
        entity->setEngine(engine());
        root()->addChild(entity);

        // The part of upstream's parser this engine's does not have (see header).
        const nlohmann::json gltf = readGlbJson(assetPath(kModel));
        if (gltf.is_discarded() || gltf.is_null()) {
            spdlog::error("Failed to read the glTF JSON of {}", kModel);
            return false;
        }
        addCamerasAndLights(entity, gltf);

        // glb lights use physical units: the exposure upstream's cameras would compute.
        scene()->setExposure(physicalExposure(kAperture, kShutter, kSensitivity));

        // Find all cameras - by default they are disabled.
        _cameras = entity->findComponents<CameraComponent>();
        for (auto* component : _cameras) {
            // Set the aspect ratio to automatic to work with any window size.
            component->camera()->setAspectRatioMode(AspectRatioMode::ASPECT_AUTO);
        }

        // Enable all lights from the glb.
        for (auto* component : entity->findComponents<LightComponent>()) {
            component->setEnabled(true);
        }

        spdlog::info("GLB: {} cameras, {} lights, exposure {}", _cameras.size(),
            entity->findComponents<LightComponent>().size(), scene()->exposure());
        if (_cameras.empty()) {
            spdlog::error("{} has no cameras", kModel);
            return false;
        }
        return true;
    }

    void update(const float dt) override
    {
        _time -= dt;

        // Change the camera every few seconds.
        if (_time <= 0.0f) {
            _time = kSwitchInterval;

            // Disable current camera.
            _cameras[_activeCamera]->setEnabled(false);

            // Activate next camera.
            _activeCamera = (_activeCamera + 1) % _cameras.size();
            _cameras[_activeCamera]->setEnabled(true);
        }
    }

private:
    void addCamerasAndLights(Entity* instance, const nlohmann::json& gltf)
    {
        if (!gltf.contains("nodes")) {
            return;
        }
        const auto& nodes = gltf["nodes"];
        const nlohmann::json lights = gltf.contains("extensions") && gltf["extensions"].contains("KHR_lights_punctual")
            ? gltf["extensions"]["KHR_lights_punctual"].value("lights", nlohmann::json::array())
            : nlohmann::json::array();

        for (const auto& node : nodes) {
            const std::string name = node.value("name", "");
            auto* nodeEntity = dynamic_cast<Entity*>(instance->findByName(name));
            if (!nodeEntity) {
                continue;
            }
            // Entity::setEngine does not propagate, and addComponent needs the
            // engine's component systems, so the instantiated node needs it too.
            nodeEntity->setEngine(engine());

            if (node.contains("camera") && gltf.contains("cameras")) {
                addCamera(nodeEntity, gltf["cameras"][node["camera"].get<size_t>()]);
            }

            if (node.contains("extensions") && node["extensions"].contains("KHR_lights_punctual")) {
                const size_t index = node["extensions"]["KHR_lights_punctual"].value("light", size_t{0});
                if (index < lights.size()) {
                    addLight(nodeEntity, name, lights[index]);
                }
            }
        }
    }

    // Upstream glb-parser createCamera.
    void addCamera(Entity* node, const nlohmann::json& gltfCamera)
    {
        const bool orthographic = gltfCamera.value("type", "") == "orthographic";
        const auto& props = gltfCamera[orthographic ? "orthographic" : "perspective"];

        auto* component = static_cast<CameraComponent*>(node->addComponent<CameraComponent>());
        Camera* camera = component->camera();
        camera->setProjection(orthographic ? ProjectionType::Orthographic : ProjectionType::Perspective);
        camera->setNearClip(props.value("znear", 0.1f));
        if (props.contains("zfar")) {
            camera->setFarClip(props["zfar"].get<float>());
        }
        camera->setAspectRatioMode(AspectRatioMode::ASPECT_AUTO);
        if (orthographic) {
            camera->setOrthoHeight(props.value("ymag", 10.0f));
            if (props.contains("xmag") && props.contains("ymag")) {
                camera->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
                camera->setAspectRatio(props["xmag"].get<float>() / props["ymag"].get<float>());
            }
        } else {
            camera->setFov(props.value("yfov", 0.0f) * kRadToDeg);
            if (props.contains("aspectRatio")) {
                camera->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
                camera->setAspectRatio(props["aspectRatio"].get<float>());
            }
        }
        component->setEnabled(false);
    }

    // Upstream khr-lights-punctual createLight.
    void addLight(Entity* node, const std::string& name, const nlohmann::json& gltfLight)
    {
        const std::string type = gltfLight.value("type", "point");

        // Rotate to match light orientation in the glTF specification, on a child
        // entity that does not exist in the glTF hierarchy.
        auto* lightEntity = new Entity();
        lightEntity->setName(name);
        lightEntity->setEngine(engine());
        node->addChild(lightEntity);
        lightEntity->rotateLocal(90.0f, 0.0f, 0.0f);

        auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
        light->setType(type == "point" ? LightType::LIGHTTYPE_OMNI
            : type == "spot" ? LightType::LIGHTTYPE_SPOT
            : LightType::LIGHTTYPE_DIRECTIONAL);
        if (gltfLight.contains("color")) {
            const auto& c = gltfLight["color"];
            light->setColor(Color(c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), 1.0f));
        } else {
            light->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        }
        light->setRange(gltfLight.value("range", 9999.0f));
        light->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
        // Under physical units upstream's luminance / conversion gives back the
        // glTF intensity itself (see header).
        light->setIntensity(gltfLight.value("intensity", 1.0f));
        if (gltfLight.contains("spot")) {
            const auto& spot = gltfLight["spot"];
            light->setInnerConeAngle(spot.value("innerConeAngle", 0.0f) * kRadToDeg);
            light->setOuterConeAngle(spot.value("outerConeAngle", 0.785398f) * kRadToDeg);
        }
        light->setEnabled(false);
    }

    std::unique_ptr<Asset> _asset;
    std::vector<CameraComponent*> _cameras;
    size_t _activeCamera = 0;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(GlbLoaderExample)
