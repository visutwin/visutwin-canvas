// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.10.2026
//
// Gaussian splat shadow casters: the switch, the alpha clip, the parameter block a shadow
// draw uploads, and the caster collection a shadow pass draws from.
//
// A splat casts through the depth-only draw every shadow pass uses (drawDepthOnly), as an
// opaque footprint: the gsplat-render shadow variants read the alpha clip, the VSM mode and
// the far clip from the block built here. A wrong lane there renders as a shadow that is a
// little too large or too small, which no single frame shows to be wrong, so the block is
// pinned here; the pixels are compared against upstream by the gsplat-shadow-orientation and
// gsplat-shadows examples.

#include <memory>
#include <vector>

#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/gsplat/gsplatComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/camera.h"
#include "scene/graphNode.h"
#include "scene/gsplat/gsplatInstance.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/meshInstance.h"
#include "scene/renderer/depthOnlyDraw.h"
#include "scene/renderer/shadowCasterFiltering.h"
#include "scene/shader-lib/programLibrary.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-6f;

    std::shared_ptr<GSplatResource> makeResource(const std::shared_ptr<GraphicsDevice>& device, const int count)
    {
        GSplatData::ActivatedSplats splats;
        splats.count = static_cast<size_t>(count);
        for (int i = 0; i < count; ++i) {
            splats.positions.insert(splats.positions.end(), {static_cast<float>(i), 1.0f, 0.0f});
            splats.rotations.insert(splats.rotations.end(), {0.0f, 0.0f, 0.0f, 1.0f});
            splats.scales.insert(splats.scales.end(), {0.1f, 0.1f, 0.1f});
            splats.opacities.push_back(1.0f);
            splats.sh0.insert(splats.sh0.end(), {0.5f, 0.5f, 0.5f});
        }
        return std::make_shared<GSplatResource>(GSplatData::fromActivated(splats, "test"), device);
    }

    MeshInstance* splatInstance(Entity& entity)
    {
        const auto* render = entity.findComponent<RenderComponent>();
        return render && !render->meshInstances().empty() ? render->meshInstances().front() : nullptr;
    }
}

int main()
{
    quietPasses();
    const auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true});

    // The parameter block of a shadow draw.
    {
        const auto resource = makeResource(device, 3);
        GSplatInstance instance(resource);
        check(nearStrict(instance.alphaClip(), 0.3f, kTolerance), "the alpha clip defaults to 0.3");
        instance.setAlphaClip(0.1f);
        const Matrix4 model = Matrix4::translation(Vector3(1.0f, 2.0f, 3.0f));
        const Matrix4 view = Matrix4::translation(Vector3(0.0f, 0.0f, -5.0f));
        const Matrix4 projection = Matrix4::identity();
        const GpuGSplatParams params = instance.shadowParams(model, view, projection, 1024.0f, 512.0f, true, 1, 25.0f);
        check(params.splatCount == 3, "every splat is drawn (no sort decides the count)");
        check(params.shBands == 0, "no spherical harmonics in a shadow");
        check(params.cameraOrtho == 1, "an orthographic pass is marked so");
        check(nearStrict(params.viewport[0], 1024.0f, kTolerance) && nearStrict(params.viewport[1], 512.0f, kTolerance) &&
                  nearStrict(params.viewport[2], 1.0f / 1024.0f, kTolerance) &&
                  nearStrict(params.viewport[3], 1.0f / 512.0f, kTolerance),
            "the footprints are sized in the pass's pixels");
        check(nearStrict(params.output[0], 0.1f, kTolerance), "lane 0 of the output block is the alpha clip");
        check(nearStrict(params.output[1], 1.0f, kTolerance), "lane 1 the VSM mode");
        check(nearStrict(params.output[2], 25.0f, kTolerance), "lane 2 the far clip");
        Vector3 origin = (params.modelView).transformPoint(Vector3(0.0f, 0.0f, 0.0f));
        check(nearStrict(origin.getZ(), -2.0f, kTolerance), "the splats are placed by view x model");
    }

    // The component's switch and clip reach the splat instance, set before or after the resource.
    {
        Entity entity;
        auto* gsplat = static_cast<GSplatComponent*>(entity.addComponentInstance(
            std::make_unique<GSplatComponent>(nullptr, &entity), componentTypeID<GSplatComponent>()));
        check(!gsplat->castShadows(), "a splat casts nothing by default");
        gsplat->setCastShadows(true);
        gsplat->setAlphaClip(0.25f);
        gsplat->setResource(makeResource(device, 2));
        MeshInstance* mi = splatInstance(entity);
        check(mi && mi->castShadow(), "set before the resource, the switch reaches its instance");
        check(mi && mi->gsplatInstance() && nearStrict(mi->gsplatInstance()->alphaClip(), 0.25f, kTolerance),
            "and so does the clip");
        gsplat->setCastShadows(false);
        check(!mi->castShadow(), "switched off afterwards, the instance stops casting");
        gsplat->setAlphaClip(0.6f);
        check(nearStrict(mi->gsplatInstance()->alphaClip(), 0.6f, kTolerance), "the clip follows a change");
        gsplat->setResource(makeResource(device, 1));
        mi = splatInstance(entity);
        check(mi && !mi->castShadow() && nearStrict(mi->gsplatInstance()->alphaClip(), 0.6f, kTolerance),
            "a replaced resource's instance takes the component's settings");
    }

    // The shadow passes collect a casting splat like any mesh instance, and draw it only with a camera
    // and a viewport to size its footprints in.
    {
        auto engine = makeTestEngine<RenderComponentSystem, GSplatComponentSystem>(device);
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(engine.get());
        engine->root()->addChild(std::move(owned));
        auto* gsplat = static_cast<GSplatComponent*>(entity->addComponent<GSplatComponent>());
        gsplat->setResource(makeResource(device, 4));
        MeshInstance* mi = splatInstance(*entity);

        const auto collected = [&] {
            std::vector<MeshInstance*> casters;
            collectShadowCasters(casters, &engine->components());
            bool found = false;
            for (auto* caster : casters) {
                found = found || (caster == mi && shouldRenderShadowMeshInstanceIgnoringVisibility(caster));
            }
            return found;
        };
        check(!collected(), "a splat that does not cast is not a caster");
        gsplat->setCastShadows(true);
        check(collected(), "one that does is collected with the scene's casters");

        auto programLibrary = getProgramLibrary(device);
        DepthOnlyShaders shaders;
        const Matrix4 viewProjection = Matrix4::identity();
        check(!drawDepthOnly(device.get(), programLibrary.get(), mi, viewProjection, shaders),
            "a pass that gives no camera (the prepass) draws no splats");
        GraphNode cameraNode;
        Camera camera;
        camera.setNode(&cameraNode);
        device->setViewport(0.0f, 0.0f, 0.0f, 0.0f);
        check(!drawDepthOnly(device.get(), programLibrary.get(), mi, viewProjection, shaders, &camera),
            "nor does a pass that set no viewport, which would size every footprint to nothing");
    }

    return finish("gsplat shadow");
}
