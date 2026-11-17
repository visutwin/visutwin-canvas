// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.11.2026
//
// A mesh's opacity dither pattern offset (MeshInstance::ditherId). The shaders used to hash
// the model's translation per draw, so a moving dithered mesh, and its dithered shadow, got a
// new pattern every frame it moved: a standing fly's wing shadow flickered between two shades.
// The offset is now fixed at the mesh's first draw and handed to the device with the model
// matrix. What is checked:
//
//  - the CPU hash is the shader's (a function of the translation's bits alone, in [0, 64));
//  - the offset is fixed at the first request, set and wrapped explicitly, and forgotten;
//  - the draws hand the device that offset, the same in every pass, and keep it as the mesh
//    moves.
//
// That the CPU hash is bit-identical to the shader's is checked on the GPU by the dithering
// examples rendering exactly as before (a static mesh keeps its old offset).

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // Records the dither offset and the model translation of every draw.
    class RecordingDevice final : public StubGraphicsDevice
    {
    public:
        using StubGraphicsDevice::StubGraphicsDevice;

        struct Draw
        {
            Vector3 translation;
            float ditherId = -1.0f;
        };
        std::vector<Draw> draws;

        void setTransformUniforms(const Matrix4&, const Matrix4& model, const float ditherId) override
        {
            _translation = model.getTranslation();
            _ditherId = ditherId;
        }

        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer, const int numInstances,
            int indirectSlot, bool first, bool last) override
        {
            StubGraphicsDevice::draw(primitive, indexBuffer, numInstances, indirectSlot, first, last);
            draws.push_back({_translation, _ditherId});
        }

    private:
        Vector3 _translation;
        float _ditherId = -1.0f;
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

    bool sameTranslation(const Vector3& a, const Vector3& b)
    {
        return std::abs(a.getX() - b.getX()) < 1e-4f && std::abs(a.getY() - b.getY()) < 1e-4f &&
            std::abs(a.getZ() - b.getZ()) < 1e-4f;
    }
}

int main()
{
    std::cout << std::unitbuf;
    quietPasses();

    std::cout << "the hash\n";
    {
        const Matrix4 at = Matrix4::translation(Vector3(1.25f, -3.5f, 7.0f));
        const float id = MeshInstance::translationDitherId(at);
        check(id >= 0.0f && id < 64.0f, "the offset is in [0, 64)");
        check(MeshInstance::translationDitherId(at) == id, "and the same for the same matrix");
        const Matrix4 turned = Matrix4::trs(Vector3(1.25f, -3.5f, 7.0f),
            Quaternion::fromEulerAngles(30.0f, 60.0f, 10.0f), Vector3(2.0f, 2.0f, 2.0f));
        check(MeshInstance::translationDitherId(turned) == id,
            "it depends on the translation alone, not the rotation or scale");
        const Matrix4 nudged = Matrix4::translation(Vector3(std::nextafter(1.25f, 2.0f), -3.5f, 7.0f));
        check(MeshInstance::translationDitherId(nudged) != id,
            "one ulp of translation gives another offset (why it cannot be hashed per frame)");
    }

    std::cout << "fixed at the first request\n";
    {
        auto material = std::make_shared<StandardMaterial>();
        MeshInstance instance(nullptr, material.get(), nullptr);
        const Matrix4 first = Matrix4::translation(Vector3(0.5f, 0.0f, 0.0f));
        const Matrix4 moved = Matrix4::translation(Vector3(0.5f, 1e-6f, 0.0f));
        const float id = instance.ditherId(first);
        check(id == MeshInstance::translationDitherId(first), "the first request takes the model's hash");
        check(instance.ditherId(moved) == id, "and keeps it when the mesh has moved");
        instance.setDitherId(70.0f);
        check(instance.ditherId(moved) == 6.0f, "a set offset is taken modulo 64");
        instance.setDitherId(-1.0f);
        check(instance.ditherId(moved) == 63.0f, "a negative one wraps into [0, 64)");
        instance.resetDitherId();
        check(instance.ditherId(moved) == MeshInstance::translationDitherId(moved),
            "forgotten, the next request takes the hash again");
    }

    std::cout << "drawn\n";
    auto device = std::make_shared<RecordingDevice>(StubGraphicsDevice::Options{
        .size = {64, 64}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem, LightComponentSystem>(device);

    Entity* cameraEntity = addEntity(*engine, "Camera");
    cameraEntity->setLocalPosition(0.0f, 0.0f, 12.0f);
    cameraEntity->addComponent<CameraComponent>();

    Entity* lightEntity = addEntity(*engine, "Light");
    auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
    light->setCastShadows(true);
    lightEntity->setLocalEulerAngles(45.0f, 30.0f, 0.0f);

    auto material = std::make_shared<StandardMaterial>();
    material->setOpacity(0.4f);
    material->setOpacityDitherMode(DitherMode::DITHER_BAYER8);
    material->setOpacityShadowDitherMode(DitherMode::DITHER_BAYER8);
    Entity* boxEntity = addEntity(*engine, "Box");
    boxEntity->setLocalPosition(0.25f, 0.5f, 0.0f);
    auto* box = static_cast<RenderComponent*>(boxEntity->addComponent<RenderComponent>());
    box->setMaterial(material.get());
    box->setType("box");
    MeshInstance* instance = box->meshInstances().front();

    engine->start();
    const auto boxDraws = [&](const Vector3& at) {
        device->draws.clear();
        engine->update(1.0f / 60.0f);
        engine->render();
        std::vector<float> ids;
        for (const auto& draw : device->draws) {
            if (sameTranslation(draw.translation, at)) {
                ids.push_back(draw.ditherId);
            }
        }
        return ids;
    };

    const Vector3 start(0.25f, 0.5f, 0.0f);
    const auto first = boxDraws(start);
    const float expected = MeshInstance::translationDitherId(boxEntity->worldTransform());
    check(first.size() >= 2, "the box is drawn by the shadow pass and the forward pass");
    bool allExpected = !first.empty();
    for (const float id : first) {
        allExpected = allExpected && id == expected;
    }
    check(allExpected, "every draw of it, in every pass, hands the offset of its first position");
    check(instance->ditherId(Matrix4::identity()) == expected, "which the mesh instance keeps");

    const Vector3 moved(0.25f, 0.5f + 1e-3f, 0.0f);
    boxEntity->setLocalPosition(moved);
    const auto second = boxDraws(moved);
    bool kept = !second.empty();
    for (const float id : second) {
        kept = kept && id == expected;
    }
    check(kept, "moved, its draws keep that offset");

    return finish("dither id");
}
