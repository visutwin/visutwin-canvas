// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
// Visibility ranges (MeshInstance::setVisibilityRange): levels of detail chosen by the camera
// distance, crossfaded by complementary dithers. What a render cannot show is held here: two
// levels meeting at a band split every pixel between them exactly once (no hole, no pixel
// drawn twice), the cull drops a level outside its range, only a draw inside a band takes the
// dithered variant and carries its fade in its block, the shadow of each camera's level is the
// only one cast, and changing a range moves the cull version (a reused cull set would keep the
// old level otherwise).

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/applicationStats.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/shader.h"
#include "scene/cullVersion.h"
#include "scene/light.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/programLibrary.h"
#include "scene/visibilityRange.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // Whether a draw with this fade keeps the fragment with this noise: the shader's test
    // (applyLodFade in forward/common-clip-planes.slang) for a fraction, everything for a draw
    // whole (which takes no dithered variant) and nothing for one hidden (which is culled).
    bool kept(const float fade, const float noise)
    {
        if (!visibilityFadePartial(fade)) {
            return fade == 1.0f;
        }
        return fade >= 0.0f ? noise < fade : noise >= 1.0f + fade;
    }

    // Records, per draw, whether the bound shader fades and the fade its block carries.
    class RecordingDevice final : public StubGraphicsDevice
    {
    public:
        explicit RecordingDevice(const Options& options) : StubGraphicsDevice(options)
        {
            setTextureHalfFloatRenderable(true);
        }

        struct Draw
        {
            bool fadeVariant = false;
            float fade = 1.0f;   // 1: no block of the instance's own
        };
        std::vector<Draw> draws;

        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer, const int numInstances,
            int indirectSlot, bool first, bool last) override
        {
            StubGraphicsDevice::draw(primitive, indexBuffer, numInstances, indirectSlot, first, last);
            Draw d;
            d.fadeVariant = _shader && _shader->features().test(ShaderFeature::LodFade);
            if (const MaterialUniforms* block = instanceMaterialUniforms()) {
                d.fade = block->lodFadeParams[0];
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
}

int main()
{
    std::cout << std::unitbuf;

    const VisibilityRange nearLevel{0.0f, 0.0f, 10.0f, 12.0f};
    const VisibilityRange farLevel{10.0f, 12.0f, VisibilityRange::kInfinity, VisibilityRange::kInfinity};

    std::cout << "the range\n";
    {
        const VisibilityRange always;
        check(!always.limits() && always.fadeAt(0.0f) == 1.0f && always.fadeAt(1e9f) == 1.0f,
            "the default draws at every distance and limits nothing");
        check(nearLevel.limits() && farLevel.limits(), "a range with a band limits");
        check(nearLevel.fadeAt(5.0f) == 1.0f && farLevel.fadeAt(5.0f) == 0.0f, "nearer than the band: the near level");
        check(nearLevel.fadeAt(10.0f) == 1.0f && farLevel.fadeAt(10.0f) == 0.0f, "at the band's start, still it alone");
        check(nearLevel.fadeAt(11.0f) == 0.5f && farLevel.fadeAt(11.0f) == -0.5f,
            "half way: half of each, fading out and in");
        check(nearLevel.fadeAt(12.0f) == 0.0f && farLevel.fadeAt(12.0f) == 1.0f, "at the band's end, the far level");
        check(nearLevel.fadeAt(1e9f) == 0.0f && farLevel.fadeAt(1e9f) == 1.0f, "and beyond");

        const VisibilityRange hardNear{0.0f, 0.0f, 10.0f, 10.0f};
        const VisibilityRange hardFar{10.0f, 10.0f, VisibilityRange::kInfinity, VisibilityRange::kInfinity};
        check(hardNear.fadeAt(9.999f) == 1.0f && hardFar.fadeAt(9.999f) == 0.0f &&
                  hardNear.fadeAt(10.0f) == 0.0f && hardFar.fadeAt(10.0f) == 1.0f,
            "a band of zero width switches at one distance, with neither gap nor overlap");

        // Every noise value the shader can read, at distances across the band: each is kept by
        // exactly one of the two levels.
        int doubled = 0;
        int missing = 0;
        for (int step = 0; step <= 2000; ++step) {
            const float distance = 9.5f + 3.0f * static_cast<float>(step) / 2000.0f;
            const float a = nearLevel.fadeAt(distance);
            const float b = farLevel.fadeAt(distance);
            for (int n = 0; n < 256; ++n) {
                const float noise = static_cast<float>(n) / 255.0f;
                const int count = kept(a, noise) + kept(b, noise);
                doubled += count > 1;
                missing += count < 1;
            }
        }
        check(doubled == 0 && missing == 0, "two levels meeting at a band cover every pixel once (" +
            std::to_string(doubled) + " doubled, " + std::to_string(missing) + " missing)");
        check(visibilityFadePartial(0.5f) && visibilityFadePartial(-0.25f) && !visibilityFadePartial(1.0f) &&
                  !visibilityFadePartial(0.0f), "only a fraction takes the dithered variant");
    }

    std::cout << "the mesh instance\n";
    {
        StandardMaterial material;
        MeshInstance instance(static_cast<Mesh*>(nullptr), &material);
        check(instance.visibilityFadeFrom(Vector3(0.0f, 0.0f, 100.0f)) == 1.0f, "no range: drawn from anywhere");
        const uint64_t before = CullVersion::current();
        instance.setVisibilityRange(nearLevel);
        check(CullVersion::current() != before, "a new range moves the cull version");
        const uint64_t after = CullVersion::current();
        instance.setVisibilityRange(nearLevel);
        check(CullVersion::current() == after, "the same range does not");

        check(instance.overriddenUniforms(&material, nullptr, 1.0f) == nullptr, "drawn whole: no block");
        const MaterialUniforms* block = instance.overriddenUniforms(&material, nullptr, 0.25f);
        check(block && block->lodFadeParams[0] == 0.25f, "in a band: a block carrying the fade");
        const uint64_t version = instance.overriddenUniformsVersion();
        block = instance.overriddenUniforms(&material, nullptr, -0.75f);
        check(block && block->lodFadeParams[0] == -0.75f && instance.overriddenUniformsVersion() != version,
            "another fade moves the version");
        instance.setClippingPlanes(std::vector<Plane>{Plane(Vector3(1.0f, 0.0f, 0.0f), 0.0f)});
        block = instance.overriddenUniforms(&material, nullptr, 1.0f);
        check(block && block->lodFadeParams[0] == 0.0f && block->clipParams[0] == 1.0f,
            "a clipped instance drawn whole carries no fade");
        auto clone = instance.cloneFor(nullptr);
        check(clone->visibilityRange() == nearLevel, "a clone carries the range");
    }

    std::cout << "variants\n";
    {
        auto device = std::make_shared<StubGraphicsDevice>();
        ProgramLibrary library(device);
        StandardMaterial material;
        const auto plain = library.getForwardShader(&material, false);
        const auto fading = library.getForwardShader(&material, false, false, false, false, false, false, false,
            false, CLIP_LOD_FADE);
        check(plain && !plain->features().test(ShaderFeature::LodFade), "a whole draw: no fade (no discard)");
        check(fading && fading->features().test(ShaderFeature::LodFade) && fading != plain, "a fading draw's variant");
        const auto shadow = library.getShadowShader(nullptr, false, false, false, false, false, false, CLIP_LOD_FADE);
        check(shadow && shadow->features().test(ShaderFeature::LodFade) && !shadow->fragmentEntry().empty(),
            "a fading caster fades its shadow, in a fragment stage");
    }

    std::cout << "two levels in a scene\n";
    auto device = std::make_shared<RecordingDevice>(StubGraphicsDevice::Options{
        .size = {64, 64}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, LightComponentSystem, CameraComponentSystem>(device);

    Entity* cameraEntity = addEntity(*engine, "Camera");
    cameraEntity->addComponent<CameraComponent>();

    auto nearMaterial = std::make_shared<StandardMaterial>();
    auto farMaterial = std::make_shared<StandardMaterial>();
    const auto addLevel = [&](const std::string& name, StandardMaterial* material, const VisibilityRange& range) {
        Entity* entity = addEntity(*engine, name);
        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        render->setMaterial(material);
        render->setType("box");
        render->meshInstances().front()->setVisibilityRange(range);
        return render->meshInstances().front();
    };
    addLevel("Near", nearMaterial.get(), nearLevel);
    addLevel("Far", farMaterial.get(), farLevel);

    engine->start();
    auto& stats = const_cast<ApplicationStats&>(*engine->stats());
    const auto frameAt = [&](const float distance) {
        cameraEntity->setLocalPosition(0.0f, 0.0f, distance);
        engine->update(1.0f / 60.0f);
        device->draws.clear();
        engine->render();
    };

    frameAt(5.0f);
    check(device->draws.size() == 1 && !device->draws[0].fadeVariant && device->draws[0].fade == 1.0f,
        "near: one draw, the near level whole");
    frameAt(11.0f);
    check(device->draws.size() == 2 && device->draws[0].fadeVariant && device->draws[1].fadeVariant,
        "in the band: both levels, dithered");
    check(device->draws.size() == 2 && device->draws[0].fade * device->draws[1].fade < 0.0f &&
              std::abs(std::abs(device->draws[0].fade) + std::abs(device->draws[1].fade) - 1.0f) < 1e-5f,
        "with complementary fades that add up to the whole");
    frameAt(20.0f);
    check(device->draws.size() == 1 && !device->draws[0].fadeVariant, "far: one draw, the far level whole");
    frameAt(20.0f);
    check(device->draws.size() == 1, "a frame with the camera still (a reused cull set) agrees");
    frameAt(5.0f);
    check(device->draws.size() == 1 && !device->draws[0].fadeVariant, "and back near, the near level");

    std::cout << "shadows\n";
    Entity* lightEntity = addEntity(*engine, "Light");
    lightEntity->setLocalEulerAngles(-60.0f, 0.0f, 0.0f);
    auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
    light->setCastShadows(true);
    light->setShadowResolution(256);
    frameAt(5.0f);
    frameAt(5.0f);
    const int nearShadowDraws = stats.drawCalls().shadow;
    check(nearShadowDraws == 1, "near: only the near level casts (" + std::to_string(nearShadowDraws) + ")");
    frameAt(11.0f);
    int fadingShadowDraws = 0;
    for (const auto& d : device->draws) {
        fadingShadowDraws += d.fadeVariant;
    }
    check(stats.drawCalls().shadow == 2 && fadingShadowDraws == 4,
        "in the band both cast, each dithered as it is drawn (" + std::to_string(stats.drawCalls().shadow) +
        " shadow draws, " + std::to_string(fadingShadowDraws) + " dithered draws)");
    frameAt(20.0f);
    check(stats.drawCalls().shadow == 1, "far: only the far level casts");

    return finish("visibility range");
}
