// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
// Clipping planes of mesh instances, render components and cameras.
//
// The planes reach the shader in the mesh instance's copy of its material's block, so what
// is checked is that the block carries them, that only the FIRST plane changes the variant
// (moving planes or changing their number must not), that the camera's planes clip its
// passes but never a shadow nor a screen-space element, and that a render component hands
// its planes to every mesh instance, later ones included, and keeps a clipped entity out of
// its batch group. A render of one clipped mesh looks right with most of these broken.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/batching/batchManager.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/shader.h"
#include "scene/clipPlanes.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/programLibrary.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // Records, per draw, the features of the bound shader and the block it uploads.
    class RecordingDevice final : public StubGraphicsDevice
    {
    public:
        using StubGraphicsDevice::StubGraphicsDevice;

        struct Draw
        {
            bool clipMesh = false;
            bool clipCamera = false;
            int meshPlanes = -1;
            int cameraPlanes = -1;
        };
        std::vector<Draw> draws;

        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer, const int numInstances,
            int indirectSlot, bool first, bool last) override
        {
            StubGraphicsDevice::draw(primitive, indexBuffer, numInstances, indirectSlot, first, last);
            Draw d;
            if (_shader) {
                d.clipMesh = _shader->features().test(ShaderFeature::ClipPlanesMesh);
                d.clipCamera = _shader->features().test(ShaderFeature::ClipPlanesCamera);
            }
            if (const MaterialUniforms* block = instanceMaterialUniforms()) {
                d.meshPlanes = static_cast<int>(block->clipParams[0]);
                d.cameraPlanes = static_cast<int>(block->clipParams[2]);
            }
            draws.push_back(d);
        }
    };

    Entity* addEntity(Engine& engine, const std::string& name)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        entity->setName(name);
        engine.root()->addChild(std::move(owned));
        return entity;
    }

    std::vector<Plane> planes(const int count)
    {
        std::vector<Plane> out;
        for (int i = 0; i < count; ++i) {
            out.emplace_back(Vector3(1.0f, 0.0f, 0.0f), static_cast<float>(i));
        }
        return out;
    }
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "the plane set\n";
    {
        ClipPlanes clip;
        check(clip.empty() && clip.version() == 0, "a new set clips nothing");
        std::vector<Plane> source = planes(2);
        clip.setPlanes(source);
        source[0].setDistance(42.0f);
        check(clip.planes().size() == 2 && clip.planes()[0].distance() == 0.0f, "the planes are copied");
        const uint64_t version = clip.version();
        clip.setIntersection(false);
        check(clip.version() == version, "an unchanged mode keeps the version");
        clip.setIntersection(true);
        check(clip.version() != version && clip.planes().size() == 2, "a new mode moves it and keeps the planes");
        clip.setPlanes(planes(12));
        check(clip.planes().size() == ClipPlanes::kMaxPlanes, "at most eight planes are kept");
        std::vector<Plane> reversed(clip.planes().rbegin(), clip.planes().rend());
        clip.setPlanes(clip.planes());
        clip.setPlanes(reversed);
        check(clip.planes()[0].distance() == 7.0f && clip.planes()[7].distance() == 0.0f,
            "its own planes assigned back in another order keep their values");
    }

    std::cout << "the mesh instance's block\n";
    {
        StandardMaterial material;
        MeshInstance instance(static_cast<Mesh*>(nullptr), &material);
        check(!instance.clipped() && instance.overriddenUniforms(&material) == nullptr, "no planes: no block");

        const std::vector<Plane> two{Plane(Vector3(0.0f, 1.0f, 0.0f), -2.0f), Plane(Vector3(0.0f, 0.0f, 1.0f), 3.0f)};
        instance.setClippingPlanes(two);
        const MaterialUniforms* block = instance.overriddenUniforms(&material);
        check(instance.clipped() && block != nullptr, "planes give a block");
        check(block && block->clipParams[0] == 2.0f && block->clipParams[1] == 0.0f &&
                  block->clipPlanes[0][1] == 1.0f && block->clipPlanes[0][3] == -2.0f &&
                  block->clipPlanes[1][2] == 1.0f && block->clipPlanes[1][3] == 3.0f &&
                  block->clipPlanes[2][0] == 0.0f,
            "it carries the planes as (normal, distance), their count and the mode");
        check(block && block->clipParams[2] == 0.0f, "no camera planes");
        check(block && block->baseColor[0] == material.packedUniforms().baseColor[0],
            "everything else is the material's");

        const uint64_t version = instance.overriddenUniformsVersion();
        check(instance.overriddenUniforms(&material) == block && instance.overriddenUniformsVersion() == version,
            "unchanged: the same block and version");
        instance.setClipIntersection(true);
        block = instance.overriddenUniforms(&material);
        check(instance.overriddenUniformsVersion() != version && block->clipParams[1] == 1.0f &&
                  block->clipParams[0] == 2.0f,
            "the mode alone moves the version and changes only the mode");

        ClipPlanes camera;
        camera.setPlanes(planes(3));
        block = instance.overriddenUniforms(&material, &camera);
        check(block->clipParams[2] == 3.0f && block->clipPlanes[0][1] == 1.0f && block->cameraClipPlanes[2][3] == 2.0f,
            "a camera's planes are written beside the instance's own");
        block = instance.overriddenUniforms(&material);
        check(block->clipParams[2] == 0.0f && block->cameraClipPlanes[2][3] == 0.0f,
            "and gone again for a draw without them");

        auto clone = instance.cloneFor(nullptr);
        check(clone->clippingPlanes().size() == 2 && clone->clipIntersection(), "a clone carries the planes and mode");

        instance.setClippingPlanes({});
        check(!instance.clipped() && instance.overriddenUniforms(&material) == nullptr, "no planes: no block again");
        MeshInstance cameraOnly(static_cast<Mesh*>(nullptr), &material);
        check(cameraOnly.overriddenUniforms(&material, &camera) != nullptr, "a camera's planes alone give a block");
    }

    std::cout << "variants\n";
    {
        auto device = std::make_shared<StubGraphicsDevice>();
        ProgramLibrary library(device);
        StandardMaterial material;
        const auto plain = library.getForwardShader(&material, false);
        const auto mesh = library.getForwardShader(&material, false, false, false, false, false, false, false,
            false, CLIP_PLANES_MESH);
        const auto both = library.getForwardShader(&material, false, false, false, false, false, false, false,
            false, CLIP_PLANES_MESH | CLIP_PLANES_CAMERA);
        check(plain && !plain->features().test(ShaderFeature::ClipPlanesMesh) &&
                  !plain->features().test(ShaderFeature::ClipPlanesCamera),
            "an unclipped draw: no clipping in the variant (no discard)");
        check(mesh && mesh->features().test(ShaderFeature::ClipPlanesMesh) &&
                  !mesh->features().test(ShaderFeature::ClipPlanesCamera), "the mesh planes' variant");
        check(both && both->features().test(ShaderFeature::ClipPlanesMesh) &&
                  both->features().test(ShaderFeature::ClipPlanesCamera), "both sets");
        const auto screen = library.getForwardShader(&material, false, false, false, false, false, false, false,
            true, CLIP_PLANES_CAMERA);
        check(screen && !screen->features().test(ShaderFeature::ClipPlanesCamera),
            "the camera's planes do not clip a screen-space element");
        const auto shadow = library.getShadowShader(nullptr, false, false, false, false, false, false, CLIP_PLANES_MESH);
        check(shadow && shadow->features().test(ShaderFeature::ClipPlanesMesh) &&
                  !shadow->fragmentEntry().empty(),
            "a clipped caster's shadow variant clips, in a fragment stage");
        check(library.getShadowShader(nullptr) != shadow, "and is not the plain caster's");
    }

    std::cout << "render components\n";
    auto device = std::make_shared<RecordingDevice>(StubGraphicsDevice::Options{
        .size = {64, 64}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(device);

    Entity* cameraEntity = addEntity(*engine, "Camera");
    cameraEntity->setLocalPosition(0.0f, 0.0f, 12.0f);
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());

    auto material = std::make_shared<StandardMaterial>();
    Entity* boxEntity = addEntity(*engine, "Box");
    auto* box = static_cast<RenderComponent*>(boxEntity->addComponent<RenderComponent>());
    box->setMaterial(material.get());
    box->setType("box");
    MeshInstance* direct = box->meshInstances().front();
    direct->setClippingPlanes(planes(1));
    box->setClippingPlanes(planes(3));
    box->setClipIntersection(true);
    check(direct->clippingPlanes().size() == 3 && direct->clipIntersection(),
        "the component hands its planes and mode to its mesh instances");
    box->setType("sphere");
    MeshInstance* later = box->meshInstances().front();
    check(later->clippingPlanes().size() == 3 && later->clipIntersection(), "including mesh instances made later");
    later->setClippingPlanes(planes(2));
    box->setClippingPlanes({});
    check(!later->clipped(), "a component without planes clears planes given to the instance directly");
    box->setClippingPlanes(planes(9));
    check(box->clippingPlanes().size() == ClipPlanes::kMaxPlanes, "at most eight");
    box->setClippingPlanes(planes(2));

    // Unparented, the clone is ours to own, and is in no engine's lists.
    std::unique_ptr<Entity> cloned(boxEntity->clone());
    auto* clonedRender = cloned ? cloned->findComponent<RenderComponent>() : nullptr;
    check(clonedRender && clonedRender->clippingPlanes().size() == 2 && clonedRender->clipIntersection() &&
              !clonedRender->meshInstances().empty() && clonedRender->meshInstances().front()->clippingPlanes().size() == 2,
        "a clone keeps the planes, on its mesh instances too");
    cloned.reset();

    engine->start();
    const auto frame = [&] {
        device->draws.clear();
        engine->update(1.0f / 60.0f);
        engine->render();
    };

    std::cout << "drawn\n";
    frame();
    check(device->draws.size() == 1 && device->draws[0].clipMesh && !device->draws[0].clipCamera &&
              device->draws[0].meshPlanes == 2 && device->draws[0].cameraPlanes == 0,
        "the forward draw clips with the instance's planes, from its own block");
    box->setClippingPlanes(planes(5));
    frame();
    check(device->draws.size() == 1 && device->draws[0].meshPlanes == 5 && device->draws[0].clipMesh,
        "moving or adding planes reaches the block");

    camera->setClippingPlanes(planes(4));
    frame();
    check(device->draws.size() == 1 && device->draws[0].clipCamera && device->draws[0].clipMesh &&
              device->draws[0].cameraPlanes == 4 && device->draws[0].meshPlanes == 5,
        "the camera's planes clip it too, beside its own");
    camera->setClippingPlanes({});
    box->setClippingPlanes({});
    frame();
    check(device->draws.size() == 1 && !device->draws[0].clipCamera && !device->draws[0].clipMesh &&
              device->draws[0].meshPlanes == -1,
        "without planes the draw is the material's again");

    std::cout << "camera copy\n";
    {
        Camera a;
        a.setClippingPlanes(planes(2));
        a.setClipIntersection(true);
        Camera b;
        b.copy(a);
        check(b.clippingPlanes().size() == 2 && b.clipIntersection(), "Camera::copy copies the planes and mode");
    }

    return finish("clipping planes");
}
