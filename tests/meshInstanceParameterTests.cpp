// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.10.2026
//
// Per-mesh-instance material parameter overrides (MeshInstance::setParameter).
//
// Instances sharing one material differ only in what they override: the forward draw of
// an instance uploads the material's packed block with its overrides applied, and every
// other draw of the material keeps the material's block. Two failures look like nothing
// at all in a render of one instance: an override block reused after the MATERIAL
// changed (the instance stops following the shared material's animation), and an
// override left on the device after the draw loop (the next pass, or the next instance,
// draws with it). Both are counted here through the real renderer on a stub device that
// records the block each forward draw was handed.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
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
    constexpr float kTolerance = 1e-5f;

    // Records, per draw, the base colour red of the block the forward draw would upload
    // (the instance's override block, or the bound material's own) and whether it was
    // an override.
    class RecordingDevice final : public StubGraphicsDevice
    {
    public:
        using StubGraphicsDevice::StubGraphicsDevice;

        struct Draw
        {
            bool overridden = false;
            float red = 0.0f;
            uint64_t version = 0;
        };
        std::vector<Draw> draws;

        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer, const int numInstances,
            int indirectSlot, bool first, bool last) override
        {
            StubGraphicsDevice::draw(primitive, indexBuffer, numInstances, indirectSlot, first, last);
            if (const MaterialUniforms* block = instanceMaterialUniforms()) {
                draws.push_back({true, block->baseColor[0], instanceMaterialUniformsVersion()});
            } else if (material()) {
                draws.push_back({false, material()->packedUniforms().baseColor[0], material()->uniformsVersion()});
            } else {
                draws.push_back({false, -1.0f, 0});
            }
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
}

int main()
{
    quietPasses();
    std::cout << std::unitbuf;

    // The block itself, without a renderer.
    {
        StandardMaterial material;
        material.setDiffuse(Color(0.2f, 0.3f, 0.4f, 1.0f));
        material.setGloss(0.4f);
        MeshInstance instance(static_cast<Mesh*>(nullptr), &material);

        check(instance.overriddenUniforms(&material) == nullptr, "no override: no block (the material's is used)");

        instance.setParameter("material_diffuse", Color(1.0f, 0.5f, 0.25f, 1.0f));
        instance.setParameter("material_gloss", 0.9f);
        const MaterialUniforms* block = instance.overriddenUniforms(&material);
        check(block != nullptr, "an override gives a block");
        check(nearStrict(block->baseColor[0], 1.0f, kTolerance) && nearStrict(block->baseColor[1], 0.5f, kTolerance) &&
                  nearStrict(block->baseColor[2], 0.25f, kTolerance),
            "material_diffuse replaces the base colour's rgb (authored sRGB, decoded in the shader like setDiffuse)");
        check(nearStrict(block->roughnessFactor, 0.1f, kTolerance), "material_gloss 0.9 packs roughness 0.1");
        check(nearStrict(material.packedUniforms().baseColor[0], 0.2f, kTolerance) &&
                  nearStrict(material.packedUniforms().roughnessFactor, 0.6f, kTolerance),
            "the material's own block is untouched");
        check(block->flags == material.packedUniforms().flags, "everything not overridden is the material's");

        const uint64_t version = instance.overriddenUniformsVersion();
        check(version != 0 && version != material.uniformsVersion(), "the block's version is its own");
        check(instance.overriddenUniforms(&material) == block && instance.overriddenUniformsVersion() == version,
            "unchanged: the same block and version");

        material.setDiffuse(Color(0.7f, 0.7f, 0.7f, 1.0f));
        material.setOpacity(0.5f);
        block = instance.overriddenUniforms(&material);
        check(instance.overriddenUniformsVersion() != version, "a material edit moves the block's version");
        check(nearStrict(block->baseColor[0], 1.0f, kTolerance) && nearStrict(block->baseColor[3], 0.5f, kTolerance),
            "the override survives the material edit, and the edit reaches what is not overridden");

        const uint64_t afterMaterialEdit = instance.overriddenUniformsVersion();
        instance.setParameter("material_emissive", Color(1.0f, 1.0f, 1.0f, 1.0f));
        block = instance.overriddenUniforms(&material);
        check(instance.overriddenUniformsVersion() != afterMaterialEdit, "a new override moves the version");
        check(nearStrict(block->emissiveColor[0], 1.0f, kTolerance), "material_emissive is decoded (white stays 1)");

        StandardMaterial other;
        block = instance.overriddenUniforms(&other);
        check(nearStrict(block->baseColor[3], 1.0f, kTolerance) && nearStrict(block->baseColor[0], 1.0f, kTolerance),
            "another material: its block under the same overrides");

        check(instance.deleteParameter("material_diffuse"), "deleteParameter answers true for an override");
        check(!instance.deleteParameter("material_diffuse"), "and false the second time");
        block = instance.overriddenUniforms(&material);
        check(nearStrict(block->baseColor[0], material.packedUniforms().baseColor[0], kTolerance),
            "a deleted override gives the material's value back");

        auto clone = instance.cloneFor(nullptr);
        check(clone->parameters().size() == instance.parameters().size() &&
                  clone->overriddenUniforms(&material) != nullptr &&
                  nearStrict(clone->overriddenUniforms(&material)->roughnessFactor, 0.1f, kTolerance),
            "a clone carries the overrides");

        instance.deleteParameter("material_gloss");
        instance.deleteParameter("material_emissive");
        check(instance.overriddenUniforms(&material) == nullptr, "nothing overridden: no block again");
    }

    // Material::removeParameter must re-pack: removal used to leave the override in the block.
    {
        StandardMaterial material;
        material.setParameter("material_gloss", 0.9f);
        check(nearStrict(material.packedUniforms().roughnessFactor, 0.1f, kTolerance), "a material override packs");
        check(material.removeParameter("material_gloss"), "removeParameter answers true");
        check(nearStrict(material.packedUniforms().roughnessFactor, 1.0f - material.gloss(), kTolerance),
            "removing it packs the material's own gloss again");
        check(!material.removeParameter("material_gloss"), "and false for an absent name");
    }

    // Through the renderer: three boxes sharing one material, the middle one overridden.
    auto device = std::make_shared<RecordingDevice>(StubGraphicsDevice::Options{
        .size = {64, 64}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(device);

    Entity* cameraEntity = addEntity(*engine, "Camera");
    cameraEntity->setLocalPosition(0.0f, 0.0f, 12.0f);
    cameraEntity->addComponent<CameraComponent>();

    auto shared = std::make_shared<StandardMaterial>();
    shared->setDiffuse(Color(0.2f, 0.2f, 0.2f, 1.0f));
    std::vector<RenderComponent*> renders;
    for (int i = 0; i < 3; ++i) {
        Entity* box = addEntity(*engine, "Box" + std::to_string(i));
        box->setLocalPosition(static_cast<float>(i - 1) * 2.5f, 0.0f, 0.0f);
        auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
        render->setMaterial(shared.get());
        render->setType("box");
        renders.push_back(render);
    }
    MeshInstance* middle = renders[1]->meshInstances().front();
    middle->setParameter("material_diffuse", Color(0.9f, 0.0f, 0.0f, 1.0f));

    engine->start();
    auto renderFrame = [&] {
        device->draws.clear();
        engine->update(1.0f / 60.0f);
        engine->render();
    };
    auto countDraws = [&](float red, bool overridden) {
        int n = 0;
        for (const auto& d : device->draws) {
            n += d.overridden == overridden && nearStrict(d.red, red, kTolerance) ? 1 : 0;
        }
        return n;
    };

    renderFrame();
    check(device->draws.size() == 3, "three forward draws");
    check(countDraws(0.9f, true) == 1, "one draw carries the override");
    check(countDraws(0.2f, false) == 2, "the other two draw with the shared material's block");

    shared->setDiffuse(Color(0.5f, 0.5f, 0.5f, 1.0f));
    shared->setOpacity(0.75f);
    renderFrame();
    check(countDraws(0.9f, true) == 1 && countDraws(0.5f, false) == 2,
        "the shared material changed: the override stays, the others follow");

    middle->deleteParameter("material_diffuse");
    renderFrame();
    check(countDraws(0.5f, false) == 3, "override removed: all three draw the material's block");

    check(device->instanceMaterialUniforms() == nullptr, "nothing is left on the device after the draw loop");

    return finish("mesh instance parameter");
}
