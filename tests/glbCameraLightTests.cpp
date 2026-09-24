// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// glTF cameras (core) and KHR_lights_punctual lights become camera and light
// components on the instantiated hierarchy, as upstream's createCamera / createLight
// build them. Until 2026-09-24 the parser read neither, so a model's cameras and
// lights simply were not there. Checked through BOTH halves of the load pipeline.
//
// What upstream does and this test pins: both are imported DISABLED; a camera sits on
// its node (glTF and the engine both look down -Z); a light sits on an extra CHILD
// entity turned 90 degrees about X (glTF lights shine down -Z, lights here down -Y);
// yfov goes from radians to degrees, ymag is the ortho half height, an aspect ratio
// is manual only when the file gives one; "point" is omni, cone angles go to degrees,
// a missing range is 9999, falloff is inverse-squared, intensity is clamped to [0, 2].

#include <tiny_gltf.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/entity.h"
#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "platform/graphics/graphicsDevice.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    bool near(const float a, const float b, const float eps = 1e-4f) { return std::fabs(a - b) < eps; }

    bool near(const Vector3& v, const float x, const float y, const float z)
    {
        return near(v.getX(), x) && near(v.getY(), y) && near(v.getZ(), z);
    }

    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&, int,
            const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

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
            check(c->projection() == ProjectionType::Perspective && near(c->fov(), 0.8f * 180.0f / 3.14159265f),
                "perspective, yfov in degrees");
            check(near(c->nearClip(), 0.05f) && near(c->farClip(), 200.0f), "near and far");
            check(c->aspectRatioMode() == AspectRatioMode::ASPECT_MANUAL && near(c->aspectRatio(), 1.5f),
                "the file's aspect ratio, manual");
            check(near(cam->localPosition(), 1.0f, 2.0f, 3.0f), "on the node itself");
        }
        Entity* orthoNode = entityNamed(root, "Ortho");
        auto* ortho = orthoNode ? orthoNode->findComponent<CameraComponent>() : nullptr;
        check(ortho && ortho->camera()->projection() == ProjectionType::Orthographic &&
              near(ortho->camera()->orthoHeight(), 2.0f) && near(ortho->camera()->aspectRatio(), 2.0f),
            "orthographic: ymag is the half height, aspect xmag / ymag");
        Entity* infiniteNode = entityNamed(root, "Infinite");
        auto* infinite = infiniteNode ? infiniteNode->findComponent<CameraComponent>() : nullptr;
        check(infinite && near(infinite->camera()->farClip(), 1000.0f) &&
              infinite->camera()->aspectRatioMode() == AspectRatioMode::ASPECT_AUTO,
            "no zfar keeps the default far plane, no aspect ratio stays automatic");

        Entity* spotNode = entityNamed(root, "Spot");
        auto* spot = spotNode ? lightOf(spotNode) : nullptr;
        check(spot != nullptr, "the light node has a child carrying the light");
        if (spot) {
            check(!spot->enabled(), "imported disabled");
            check(spot->entity()->name() == "Spot", "the child is named after the node");
            check(spot->type() == LightType::LIGHTTYPE_SPOT, "spot");
            check(near(spot->intensity(), 2.0f), "intensity 5 clamped to 2");
            check(near(spot->color().g, 0.5f) && near(spot->color().b, 0.25f), "colour");
            check(near(spot->range(), 12.0f), "range");
            check(near(spot->innerConeAngle(), 0.2f * 180.0f / 3.14159265f) &&
                  near(spot->outerConeAngle(), 0.5f * 180.0f / 3.14159265f), "cone angles in degrees");
            check(spot->falloffMode() == LightFalloff::LIGHTFALLOFF_INVERSESQUARED, "inverse-squared falloff");
            check(near(spot->direction(), 0.0f, 0.0f, -1.0f), "shines down the node's -Z");
        }
        Entity* sunNode = entityNamed(root, "Sun");
        auto* sun = sunNode ? lightOf(sunNode) : nullptr;
        check(sun && sun->type() == LightType::LIGHTTYPE_DIRECTIONAL && near(sun->intensity(), 0.7f),
            "directional, intensity kept");
        check(sun && near(sun->direction(), -1.0f, 0.0f, 0.0f), "a turned node turns its light (-Z -> world -X)");
        Entity* bulbNode = entityNamed(root, "Bulb");
        auto* bulb = bulbNode ? lightOf(bulbNode) : nullptr;
        check(bulb && bulb->type() == LightType::LIGHTTYPE_OMNI && near(bulb->range(), 9999.0f) &&
              near(bulb->intensity(), 1.0f), "point is omni; no range is 9999; default intensity 1");
    }
}

int main()
{
    std::cout << std::unitbuf;
    const auto device = std::make_shared<StubDevice>();
    {
        tinygltf::Model model = buildModel();
        auto container = GlbParser::createFromModel(model, device, "glbCameraLightTests");
        std::unique_ptr<Entity> root(container ? container->instantiateRenderEntity() : nullptr);
        if (!root) {
            std::cout << "  FAIL createFromModel instantiates\n";
            return 1;
        }
        checkHierarchy(root.get(), "createFromModel");
    }
    {
        tinygltf::Model model = buildModel();
        auto prepared = GlbParser::prepareFromModel(model, PixelFormat::PIXELFORMAT_RGBA8, "glbCameraLightTests");
        auto container = GlbParser::createFromPrepared(model, std::move(prepared), device, "glbCameraLightTests");
        std::unique_ptr<Entity> root(container ? container->instantiateRenderEntity() : nullptr);
        if (!root) {
            std::cout << "  FAIL createFromPrepared instantiates\n";
            return 1;
        }
        checkHierarchy(root.get(), "\nprepareFromModel + createFromPrepared (the async path)");
    }

    std::cout << (failures == 0 ? "\nAll glTF camera/light tests passed\n" : "\nglTF camera/light tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
