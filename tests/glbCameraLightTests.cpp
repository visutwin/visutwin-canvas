// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.09.2026
//
// glTF cameras (core) and KHR_lights_punctual lights become camera and light
// components on the instantiated hierarchy. A parser that reads neither leaves a model's cameras and lights simply
// not there. Checked through BOTH halves of the load pipeline.
//
// What this test pins: both are imported DISABLED; a camera sits on
// its node (glTF and the engine both look down -Z); a light sits on an extra CHILD
// entity turned 90 degrees about X (glTF lights shine down -Z, lights here down -Y);
// yfov goes from radians to degrees, ymag is the ortho half height, an aspect ratio
// is manual only when the file gives one; "point" is omni, cone angles go to degrees,
// a missing range is 9999, falloff is inverse-squared, intensity is clamped to [0, 2].

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/entity.h"
#include "framework/parsers/glbContainerResource.h"
#include "support/check.h"
#include "support/gltfModel.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-4f;

    tinygltf::Model buildModel()
    {
        tinygltf::Model model;

        tinygltf::Camera persp;
        persp.type = "perspective";
        persp.perspective.yfov = 0.8;
        persp.perspective.znear = 0.05;
        persp.perspective.zfar = 200.0;
        persp.perspective.aspectRatio = 1.5;
        tinygltf::Camera ortho;
        ortho.type = "orthographic";
        ortho.orthographic.xmag = 4.0;
        ortho.orthographic.ymag = 2.0;
        ortho.orthographic.znear = 0.1;
        ortho.orthographic.zfar = 50.0;
        tinygltf::Camera infinite;   // no zfar, no aspect ratio
        infinite.type = "perspective";
        infinite.perspective.yfov = 1.0;
        infinite.perspective.znear = 0.2;
        model.cameras = {persp, ortho, infinite};

        tinygltf::Light spot;
        spot.type = "spot";
        spot.color = {1.0, 0.5, 0.25};
        spot.intensity = 5.0;
        spot.range = 12.0;
        spot.spot.innerConeAngle = 0.2;
        spot.spot.outerConeAngle = 0.5;
        tinygltf::Light sun;
        sun.type = "directional";
        sun.intensity = 0.7;
        tinygltf::Light bulb;
        bulb.type = "point";
        model.lights = {spot, sun, bulb};

        auto node = [](const std::string& name) {
            tinygltf::Node n;
            n.name = name;
            return n;
        };
        tinygltf::Node camNode = node("Cam");
        camNode.camera = 0;
        camNode.translation = {1.0, 2.0, 3.0};
        tinygltf::Node orthoNode = node("Ortho");
        orthoNode.camera = 1;
        tinygltf::Node infiniteNode = node("Infinite");
        infiniteNode.camera = 2;
        tinygltf::Node spotNode = node("Spot");
        spotNode.light = 0;
        tinygltf::Node sunNode = node("Sun");
        sunNode.light = 1;
        // Turned 90 degrees about +Y: its -Z (the glTF light direction) is world -X.
        sunNode.rotation = {0.0, std::sqrt(0.5), 0.0, std::sqrt(0.5)};
        tinygltf::Node bulbNode = node("Bulb");
        bulbNode.light = 2;
        model.nodes = {camNode, orthoNode, infiniteNode, spotNode, sunNode, bulbNode};

        tinygltf::Scene scene;
        scene.nodes = {0, 1, 2, 3, 4, 5};
        model.scenes.push_back(scene);
        model.defaultScene = 0;
        model.extensionsUsed = {"KHR_lights_punctual"};
        return model;
    }

    Entity* entityNamed(Entity* root, const std::string& name)
    {
        return dynamic_cast<Entity*>(root->findByName(name));
    }

    // The light entity is the node's child of the same name.
    LightComponent* lightOf(Entity* node)
    {
        for (const auto& child : node->children()) {
            if (auto* entity = dynamic_cast<Entity*>(child.get())) {
                if (auto* light = entity->findComponent<LightComponent>()) {
                    return light;
                }
            }
        }
        return nullptr;
    }

    void checkHierarchy(Entity* root, const std::string& label)
    {
        std::cout << label << '\n';
        Entity* cam = entityNamed(root, "Cam");
        auto* camera = cam ? cam->findComponent<CameraComponent>() : nullptr;
        check(camera != nullptr, "the camera node has a camera component");
        if (camera) {
            const Camera* c = camera->camera();
            check(!camera->enabled(), "imported disabled");
            check(c->projection() == ProjectionType::Perspective &&
                  nearStrict(c->fov(), 0.8f * 180.0f / 3.14159265f, kTolerance),
                "perspective, yfov in degrees");
            check(nearStrict(c->nearClip(), 0.05f, kTolerance) &&
                  nearStrict(c->farClip(), 200.0f, kTolerance), "near and far");
            check(c->aspectRatioMode() == AspectRatioMode::ASPECT_MANUAL &&
                  nearStrict(c->aspectRatio(), 1.5f, kTolerance),
                "the file's aspect ratio, manual");
            check(nearStrict(cam->localPosition(), 1.0f, 2.0f, 3.0f, kTolerance), "on the node itself");
        }
        Entity* orthoNode = entityNamed(root, "Ortho");
        auto* ortho = orthoNode ? orthoNode->findComponent<CameraComponent>() : nullptr;
        check(ortho && ortho->camera()->projection() == ProjectionType::Orthographic &&
              nearStrict(ortho->camera()->orthoHeight(), 2.0f, kTolerance) &&
              nearStrict(ortho->camera()->aspectRatio(), 2.0f, kTolerance),
            "orthographic: ymag is the half height, aspect xmag / ymag");
        Entity* infiniteNode = entityNamed(root, "Infinite");
        auto* infinite = infiniteNode ? infiniteNode->findComponent<CameraComponent>() : nullptr;
        check(infinite && nearStrict(infinite->camera()->farClip(), 1000.0f, kTolerance) &&
              infinite->camera()->aspectRatioMode() == AspectRatioMode::ASPECT_AUTO,
            "no zfar keeps the default far plane, no aspect ratio stays automatic");

        Entity* spotNode = entityNamed(root, "Spot");
        auto* spot = spotNode ? lightOf(spotNode) : nullptr;
        check(spot != nullptr, "the light node has a child carrying the light");
        if (spot) {
            check(!spot->enabled(), "imported disabled");
            check(spot->entity()->name() == "Spot", "the child is named after the node");
            check(spot->type() == LightType::LIGHTTYPE_SPOT, "spot");
            check(nearStrict(spot->intensity(), 2.0f, kTolerance), "intensity 5 clamped to 2");
            check(nearStrict(spot->color().g, 0.5f, kTolerance) &&
                  nearStrict(spot->color().b, 0.25f, kTolerance), "colour");
            check(nearStrict(spot->range(), 12.0f, kTolerance), "range");
            check(nearStrict(spot->innerConeAngle(), 0.2f * 180.0f / 3.14159265f, kTolerance) &&
                  nearStrict(spot->outerConeAngle(), 0.5f * 180.0f / 3.14159265f, kTolerance),
                "cone angles in degrees");
            check(spot->falloffMode() == LightFalloff::LIGHTFALLOFF_INVERSESQUARED, "inverse-squared falloff");
            check(nearStrict(spot->direction(), 0.0f, 0.0f, -1.0f, kTolerance), "shines down the node's -Z");
        }
        Entity* sunNode = entityNamed(root, "Sun");
        auto* sun = sunNode ? lightOf(sunNode) : nullptr;
        check(sun && sun->type() == LightType::LIGHTTYPE_DIRECTIONAL && nearStrict(sun->intensity(), 0.7f, kTolerance),
            "directional, intensity kept");
        check(sun && nearStrict(sun->direction(), -1.0f, 0.0f, 0.0f, kTolerance),
            "a turned node turns its light (-Z -> world -X)");
        Entity* bulbNode = entityNamed(root, "Bulb");
        auto* bulb = bulbNode ? lightOf(bulbNode) : nullptr;
        check(bulb && bulb->type() == LightType::LIGHTTYPE_OMNI && nearStrict(bulb->range(), 9999.0f, kTolerance) &&
              nearStrict(bulb->intensity(), 1.0f, kTolerance),
            "point is omni; no range is 9999; default intensity 1");
    }
}

int main()
{
    std::cout << std::unitbuf;
    const auto device = std::make_shared<StubGraphicsDevice>();
    const bool loaded = forEachLoadPath([] { return buildModel(); }, device, "glbCameraLightTests",
        [](GlbContainerResource* container, const LoadPath path) {
            const bool created = path == LoadPath::CreateFromModel;
            std::unique_ptr<Entity> root(container ? container->instantiateRenderEntity() : nullptr);
            if (!root) {
                std::cout << (created ? "  FAIL createFromModel instantiates\n"
                                      : "  FAIL createFromPrepared instantiates\n");
                return false;
            }
            checkHierarchy(root.get(),
                created ? "createFromModel" : "\nprepareFromModel + createFromPrepared (the async path)");
            return true;
        });
    if (!loaded) {
        return 1;
    }

    return finish("glTF camera/light");
}
