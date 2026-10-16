// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.10.2026
//
// Auto exposure as a camera frame effect: the camera's exposure adapts to the brightness of
// what it sees, as the eye adapts to the dark or the daylight. Every frame a 64-bin histogram
// of the screen's brightness is measured (engine/shaders/slang/programs/auto-exposure.slang,
// three passes at the postResolve stage over the half-resolution scene) and read back without
// blocking (Texture::readAsync); the part of it between lowPercent and highPercent of the
// screen (leaving out deep shadows and the sun) is averaged, and the exposure eases towards the
// one that displays that average as middle grey, faster towards the light than the dark.
//
// What it adapts is the exposure the camera renders with (the scene's, or the physical
// camera's under physical units), by up to minEv / maxEv stops, through
// cameraExposureScale(): compose's tone mapping, TAA and SMAA all follow it. It does nothing
// under tone mapping NONE, which applies no exposure. The histogram arrives a frame or two
// late, which the gradual adaptation hides; a device that cannot read back adapts nothing.
// Opt-in:
//
//     auto autoExposure = std::make_shared<AutoExposureEffect>();
//     camera->addFrameEffect(autoExposure);
//
// The debug view "autoexposure" (RenderingSettings::composeDebug) shows the scene at the
// adapted exposure with the histogram along the bottom: the bins the average includes bright,
// the average in yellow, the brightness the camera exposes as middle grey in blue (the two meet
// once it has adapted).
//
#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include "cameraFrameEffect.h"

namespace visutwin::canvas
{
    class RenderPass;
    class RenderTarget;

    /// The average position, as a fractional bin index, of the part of `histogram` between the
    /// fractions `lowPercent` and `highPercent` of its total (the bins at the two fractions
    /// counting with the part of them that lies between, so the average moves gradually), or -1
    /// when nothing lies between them.
    float meterHistogram(const float* histogram, size_t bins, float lowPercent, float highPercent);

    class AutoExposureEffect final : public CameraFrameEffect
    {
    public:
        static constexpr int kBins = 64;

        AutoExposureEffect();
        ~AutoExposureEffect() override;

        /// The exposure compensation in EV: how much brighter than middle grey the average of
        /// the screen is shown, each step doubling or halving it.
        float compensation = 0.0f;
        /// The lowest and highest exposure it adapts to, in EV relative to the camera's.
        float minEv = -6.0f;
        float maxEv = 6.0f;
        /// How fast it adapts as the scene gets brighter, and darker: it covers about two thirds
        /// of a change in 1 / speed seconds.
        float speedDarkToLight = 3.0f;
        float speedLightToDark = 1.0f;
        /// The fractions of the screen, 0-1, below which the darkest part and above which the
        /// brightest part are left out of the average.
        float lowPercent = 0.1f;
        float highPercent = 0.9f;
        /// How much the centre of the screen counts over its edges, 0-1.
        float centerWeight = 0.5f;

        /// Enabled, and its camera tone maps (under NONE no exposure is applied).
        [[nodiscard]] bool active() const override;
        [[nodiscard]] bool ownsPasses() const override { return true; }
        void createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes) override;
        void destroyPasses() override;
        void update() override;
        void frameUpdate(const CameraFrameEffectContext& context) override;
        [[nodiscard]] float cameraExposureScale() const override { return _exposureScale; }

        /// The exposure the camera renders with this frame: its own, scaled by the adaptation.
        [[nodiscard]] float exposure() const { return _exposure; }
        /// The latest histogram read back: the weight of each bin, darkest first. The bins span
        /// the brightness the exposure limits show as middle grey, and 4 EV beyond either side.
        [[nodiscard]] const std::array<float, kBins>& histogram() const { return _histogram; }
        /// How many histograms have arrived.
        [[nodiscard]] int histogramsReceived() const { return _histogramsReceived; }

        /// Adapts to the next measurement at once rather than gradually: for a cut to another
        /// view. Histograms measured before the call are ignored.
        void reset();

        /// Hands a histogram to the metering as if read back, measured over `rangeMin` ..
        /// `rangeMax` (log2 luminance) for measurement `tag`; what the readback calls.
        void measure(const float* histogram, float rangeMin, float rangeMax, int tag);

    private:
        class Pass;
        void readHistogram();

        std::shared_ptr<Texture> _meter;
        std::shared_ptr<Texture> _rows;
        std::shared_ptr<Texture> _histogramTexture;
        std::array<std::shared_ptr<Pass>, 3> _passes{};
        Texture* _source = nullptr;
        std::shared_ptr<int> _alive = std::make_shared<int>(0);
        bool _reading = false;

        struct Uniforms
        {
            float source[4];
            float params[4];
        };
        Uniforms _uniforms{};

        std::array<float, kBins> _histogram{};
        int _histogramsReceived = 0;
        bool _hasAverage = false;
        float _averageLog2 = 0.0f;
        float _ev = 0.0f;
        bool _snap = true;
        int _measurement = 0;
        float _exposure = 1.0f;
        float _exposureScale = 1.0f;
        float _rangeMin = -8.0f;
        float _rangeMax = 8.0f;

        // The parameters frozen by update(), clamped.
        float _minEvFrozen = -6.0f;
        float _maxEvFrozen = 6.0f;
        float _lowFrozen = 0.1f;
        float _highFrozen = 0.9f;

        // The debug view: the bins relative to the largest, and the range they were measured
        // over; the markers (included range, average, middle grey), as positions 0-1.
        std::array<float, kBins> _bars{};
        float _barsMin = 0.0f;
        float _barsMax = 0.0f;
        std::array<float, 4> _markers{};
    };
}
