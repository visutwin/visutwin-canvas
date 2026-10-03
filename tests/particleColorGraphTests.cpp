// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// A particle's colour graph is authored in gamma space, like its colour map, and the
// shaders multiply it into LINEAR colour that the output stage tone-maps and encodes.
// So the colour LUT the shaders read holds each rgb sample DECODED (clamped to [0, 1],
// then raised to 2.2) and the alpha as authored. A raw ramp draws every mid-tone of
// the graph brighter than authored: a 0.5 grey comes out at about 0.73 once encoded.

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include "core/math/curve.h"
#include "core/math/curveSet.h"
#include "scene/particles/particleEmitter.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

int main()
{
    // CPU buffers: the emitter writes its particle pool at construction.
    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true});

    ParticleEmitterOptions options;
    options.numParticles = 4;
    // Constant graphs: r 0.5, g 0.25, b above white, alpha 0.5.
    options.colorGraph = CurveSet({{0.0f, 0.5f}, {0.0f, 0.25f}, {0.0f, 1.5f}});
    options.alphaGraph = Curve(std::vector<float>{0.0f, 0.5f});

    std::cout << "the colour graph is decoded, the alpha graph is not\n";
    {
        const auto emitter = std::make_shared<ParticleEmitter>(device, options);
        const auto& lut = emitter->renderParams().colorLut;
        bool decoded = true;
        bool clamped = true;
        bool alphaRaw = true;
        for (int i = 0; i < ParticleEmitter::kCurveSamples; ++i) {
            decoded = decoded && near(lut[i][0], std::pow(0.5f, 2.2f), 1e-5f) &&
                near(lut[i][1], std::pow(0.25f, 2.2f), 1e-5f);
            clamped = clamped && near(lut[i][2], 1.0f, 1e-6f);
            alphaRaw = alphaRaw && near(lut[i][3], 0.5f, 1e-6f);
        }
        check(decoded, "each rgb sample is the graph value decoded from gamma space");
        check(clamped, "a value above white is clamped to 1 before the decode");
        check(alphaRaw, "the alpha sample is the graph value as authored");
    }

    std::cout << "\nthe default graph stays white\n";
    {
        ParticleEmitterOptions defaults;
        defaults.numParticles = 4;
        const auto emitter = std::make_shared<ParticleEmitter>(device, defaults);
        const auto& lut = emitter->renderParams().colorLut;
        check(near(lut[0][0], 1.0f, 1e-6f) && near(lut[0][1], 1.0f, 1e-6f) && near(lut[0][2], 1.0f, 1e-6f) &&
                  near(lut[ParticleEmitter::kCurveSamples - 1][0], 1.0f, 1e-6f),
            "white decodes to white");
    }

    return finish("particle colour graph");
}
