// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.10.2026
//
// AutoExposureEffect through the real engine on a stub device, the histograms handed in as the
// readback would (the stub reads nothing back): the metering between the two fractions of the
// screen, the first measurement adapted to at once, later ones eased by the world's time and
// faster towards the light, the limits, a reset ignoring histograms measured before it, the
// exposure scale reaching the camera and Scene::exposureFor, and none under tone mapping NONE
// or once the effect is disabled. Its three passes sit at the postResolve stage.
//
// A wrong exposure renders a frame too bright or too dark but plausible, and the adaptation is
// gradual, so a frame compared with nothing does not show it.

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/graphics/autoExposureEffect.h"
#include "scene/graphics/renderPassCameraFrame.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    using Histogram = std::array<float, AutoExposureEffect::kBins>;

    Histogram oneBin(const int bin, const float weight = 100.0f)
    {
        Histogram h{};
        h[static_cast<size_t>(bin)] = weight;
        return h;
    }

    // The histogram counts in 32-bit float targets, which the effect asks the device for.
    class FloatTargetDevice final : public StubGraphicsDevice
    {
    public:
        explicit FloatTargetDevice(const Options& options) : StubGraphicsDevice(options)
        {
            setTextureFloatRenderable(true);
        }
    };
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "metering\n";
    {
        Histogram h{};
        check(meterHistogram(h.data(), h.size(), 0.1f, 0.9f) == -1.0f, "an empty histogram meters nothing");
        h = oneBin(20);
        check(near(meterHistogram(h.data(), h.size(), 0.1f, 0.9f), 20.0f, 1e-5f), "one bin meters at that bin");
        h = {};
        h[10] = 50.0f;   // the darkest half
        h[40] = 50.0f;   // the brightest half
        check(near(meterHistogram(h.data(), h.size(), 0.5f, 1.0f), 40.0f, 1e-5f),
            "the part below the low fraction is left out");
        check(near(meterHistogram(h.data(), h.size(), 0.0f, 0.5f), 10.0f, 1e-5f),
            "and the part above the high fraction");
        check(near(meterHistogram(h.data(), h.size(), 0.25f, 0.75f), 25.0f, 1e-4f),
            "bins at the fractions count with the part of them between: (25 x 10 + 25 x 40) / 50");
    }

    auto device = std::make_shared<FloatTargetDevice>(StubGraphicsDevice::Options{
        .size = {64, 48}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<CameraComponentSystem>(device);
    Scene& scene = *engine->scene();
    scene.setExposure(0.5f);

    auto owned = std::make_unique<Entity>();
    Entity* cameraEntity = owned.get();
    cameraEntity->setEngine(engine.get());
    engine->root()->addChild(std::move(owned));
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    camera->setToneMapping(TONEMAP_ACES);

    auto ae = std::make_shared<AutoExposureEffect>();
    ae->minEv = -6.0f;
    ae->maxEv = 4.0f;
    ae->lowPercent = 0.0f;
    ae->highPercent = 1.0f;
    camera->addFrameEffect(ae);

    engine->start();
    constexpr float kDt = 1.0f / 60.0f;
    const auto frame = [&] {
        engine->update(kDt);
        engine->render();
    };
    frame();

    std::cout << "passes\n";
    {
        const auto& passes = camera->cameraFrame()->effectPasses().postResolve;
        check(passes.size() == 3, "three passes at the postResolve stage");
        check(passes.size() == 3 && passes[0]->name() == "RenderPassAutoExposure-meter" &&
              passes[1]->name() == "RenderPassAutoExposure-rows" && passes[2]->name() == "RenderPassAutoExposure-sum",
            "meter, rows, sum");
        check(ae->histogramsReceived() == 0 && camera->camera()->exposureScale() == 1.0f,
            "no histogram yet: the camera keeps its own exposure");
    }

    // The bin whose brightness is shown as middle grey (+ compensation 0) at the camera's own
    // exposure: the range runs from key - log2(base) - maxEv - 4 to key - log2(base) - minEv + 4.
    const float log2Base = std::log2(0.5f);
    const float key = std::log2(0.18f);
    const float rangeMin = key - log2Base - ae->maxEv - 4.0f;
    const float rangeMax = key - log2Base - ae->minEv + 4.0f;

    std::cout << "the first measurement is adapted to at once\n";
    {
        // A scene 2 EV darker than the camera shows as middle grey: +2 EV.
        const float darkLog2 = key - log2Base - 2.0f;
        const float bin = (darkLog2 - rangeMin) / (rangeMax - rangeMin) * 63.0f;
        Histogram h{};
        const int lo = static_cast<int>(std::floor(bin));
        h[static_cast<size_t>(lo)] = (1.0f - (bin - lo)) * 100.0f;
        h[static_cast<size_t>(lo + 1)] = (bin - lo) * 100.0f;
        ae->measure(h.data(), rangeMin, rangeMax, 0);
        frame();
        check(near(std::log2(camera->camera()->exposureScale()), 2.0f, 1e-3f), "+2 EV at once");
        check(near(scene.exposureFor(camera->camera()), 0.5f * 4.0f, 1e-3f), "Scene::exposureFor carries it");
        check(near(ae->exposure(), 2.0f, 1e-3f), "and the effect reports it");
    }

    std::cout << "later ones are eased\n";
    {
        // To 0 EV: the scene brighter, at speedDarkToLight 3.
        const float bin = (key - log2Base - rangeMin) / (rangeMax - rangeMin) * 63.0f;
        const int lo = static_cast<int>(std::floor(bin));
        Histogram h{};
        h[static_cast<size_t>(lo)] = (1.0f - (bin - lo)) * 100.0f;
        h[static_cast<size_t>(lo + 1)] = (bin - lo) * 100.0f;
        ae->measure(h.data(), rangeMin, rangeMax, 0);
        frame();
        const float expected = 2.0f - 2.0f * (1.0f - std::exp(-kDt * 3.0f));
        check(near(std::log2(camera->camera()->exposureScale()), expected, 1e-3f),
            "one frame covers 1 - exp(-dt x speedDarkToLight) of the way");
        for (int i = 0; i < 300; ++i) {
            frame();
        }
        check(near(std::log2(camera->camera()->exposureScale()), 0.0f, 1e-3f), "and arrives");

        // Back to +2 EV, slower: speedLightToDark 1.
        const float darkBin = (key - log2Base - 2.0f - rangeMin) / (rangeMax - rangeMin) * 63.0f;
        const int dlo = static_cast<int>(std::floor(darkBin));
        Histogram dark{};
        dark[static_cast<size_t>(dlo)] = (1.0f - (darkBin - dlo)) * 100.0f;
        dark[static_cast<size_t>(dlo + 1)] = (darkBin - dlo) * 100.0f;
        ae->measure(dark.data(), rangeMin, rangeMax, 0);
        frame();
        check(near(std::log2(camera->camera()->exposureScale()), 2.0f * (1.0f - std::exp(-kDt * 1.0f)), 1e-3f),
            "towards the dark at speedLightToDark");
    }

    std::cout << "limits and reset\n";
    {
        ae->reset();
        Histogram black = oneBin(0);
        ae->measure(black.data(), rangeMin, rangeMax, 0);
        frame();
        check(ae->histogramsReceived() == 3, "a histogram measured before a reset is ignored");
        ae->measure(black.data(), rangeMin, rangeMax, 1);
        frame();
        check(near(std::log2(camera->camera()->exposureScale()), ae->maxEv, 1e-3f),
            "after a reset the next one is adapted to at once, within maxEv");
    }

    std::cout << "no tone mapping, disabled\n";
    {
        camera->setToneMapping(TONEMAP_NONE);
        frame();
        check(!ae->active() && camera->camera()->exposureScale() == 1.0f,
            "under tone mapping NONE it takes no part and the camera has its own exposure");
        camera->setToneMapping(TONEMAP_ACES);
        frame();
        check(ae->active() && camera->camera()->exposureScale() != 1.0f, "back again with tone mapping");
        ae->setEnabled(false);
        frame();
        check(camera->camera()->exposureScale() == 1.0f, "disabled, the camera has its own exposure");
    }

    return finish("auto exposure");
}
