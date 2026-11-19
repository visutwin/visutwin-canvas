// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// A probe volume: a regular grid of light probes covering a box, providing the ambient diffuse of
// the materials it is applied to. The box is divided into cells with a probe at the centre of
// each; a probe stores the incoming radiance at its position as L1 spherical harmonics, and a
// surface interpolates the eight probes around it. Outside the box the ambient blends back to the
// environment's. The probes are baked by ProbeVolumeBaker, at run time or ahead of time, and saved
// and restored with serialize / deserialize; restored probes work on any device.
//
// The baker can find the invalid probes, inside geometry, which would darken the surfaces around
// them: with leakReduction the surfaces ignore those and favour the probes in front of them. It
// can also capture the probes' visibility, the distances to the surfaces around them, which the
// surfaces use to ignore the probes they are hidden from (visibility).
//
// The spherical harmonics of the probes form a LIGHTING; a volume can hold several, baked under
// different lighting (the times of a day), and shows one (activeLighting). The validity, the
// offsets and the visibility depend only on the geometry and are shared by the lightings.
//
// The volume hands its data to the device (GraphicsDevice::setProbeVolumeBlocks), which the
// forward shaders of the materials with Material::probeVolume read, so one volume is active at
// a time: the last one created or changed.
//
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/math/vector3.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/constants.h"
#include "platform/graphics/probeVolumeBlock.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Material;
    class Texture;

    class ProbeVolume
    {
    public:
        /// The floats of a probe in a lighting: the L0 and the three L1 coefficients (x, y, z) of
        /// the red, green and blue channels.
        static constexpr int kFloatsPerProbe = 12;

        /// `aabb` is the box the probes cover, typically the interior of a building, divided into
        /// `resolution` cells (each at least 1) with a probe at the centre of each. `format` is the
        /// probe texture's, PIXELFORMAT_RGBA16F or PIXELFORMAT_RGBA8: 8-bit texels use half the
        /// memory but store the coefficients relative to L0, which the filtering interpolates only
        /// approximately.
        ProbeVolume(GraphicsDevice* device, const BoundingBox& aabb, const std::array<uint32_t, 3>& resolution,
            PixelFormat format = PixelFormat::PIXELFORMAT_RGBA16F);
        ~ProbeVolume();

        ProbeVolume(const ProbeVolume&) = delete;
        ProbeVolume& operator=(const ProbeVolume&) = delete;

        GraphicsDevice* device() const { return _device; }
        const BoundingBox& aabb() const { return _aabb; }
        /// The corners of the box, computed in double precision, so that a deserialized volume
        /// keeps the corners it was saved with to the bit.
        const Vector3& aabbMin() const { return _min; }
        const Vector3& aabbMax() const { return _max; }
        const std::array<uint32_t, 3>& resolution() const { return _resolution; }
        uint32_t probeCount() const { return _resolution[0] * _resolution[1] * _resolution[2]; }

        /// The probe texture's format; a change encodes the probes again from their data.
        PixelFormat format() const { return _format; }
        void setFormat(PixelFormat format);

        /// The 3D texture holding the probes: three sub-volumes stacked along its depth, for the
        /// red, green and blue channels.
        Texture* texture() const { return _texture.get(); }
        /// For each probe, in rgb its offset in units of its cell mapped from -0.5..0.5 to 1..255
        /// (128 none), in alpha the validity of the eight probes of the cell it is the first corner
        /// of, as the bits of a byte.
        Texture* validityTexture() const { return _validityTexture.get(); }
        /// The octahedral maps of the visibility, in tiles with a one-texel border.
        Texture* visibilityTexture() const { return _visibilityTexture.get(); }

        /// The lightings: a volume has one when created, and setData adds more.
        uint32_t lightingCount() const { return static_cast<uint32_t>(_lightings.size()); }
        /// The lighting shown, 0 by default.
        uint32_t activeLighting() const { return _activeLighting; }
        void setActiveLighting(uint32_t index);
        /// A lighting: the spherical harmonics of the probes (kFloatsPerProbe each), the probes
        /// ordered along x, then y, then z.
        std::span<const float> data(uint32_t index = 0) const;
        /// Sets a lighting; `index` lightingCount() adds one. The lighting shown is uploaded.
        void setData(std::span<const float> data, uint32_t index = 0);
        /// Whether the probes were set (setData, a bake or deserialize); until then the volume
        /// shows the environment's ambient.
        bool hasData() const { return _hasData; }

        /// The probes the cameras of a bake capture with (Camera::probeVolumeCapture), which differ
        /// from the probes shown while they are baked; enabled and at the baked intensity, so the
        /// volume's settings do not change the bake. Empty releases them.
        void setBakeData(std::span<const float> data);
        /// The bake fields of the capture block (ProbeVolumeBlock::bakeLanes, bakeSkyLane,
        /// bakeAtlas): the geometry a bake's lighting quad reads. Released with the bake data.
        void setBakeGeometry(const ProbeVolumeBlock& source);

        /// The validity of the probes, 1 valid, 0 invalid, in probe order.
        const std::vector<uint8_t>& validity() const { return _validity; }
        /// Empty makes every probe valid.
        void setValidity(std::span<const uint8_t> validity);
        uint32_t invalidProbeCount() const;

        /// The offsets of the positions the probes were captured at from their positions, three
        /// floats a probe, or empty when captured at their positions. The probes are interpolated
        /// at their positions, while the leak reduction and the visibility use the positions they
        /// were captured at. An offset is limited to half a cell along each axis.
        const std::vector<float>& offsets() const { return _offsets; }
        void setOffsets(std::span<const float> offsets);

        /// The visibility: for each probe an octahedral map (Cigolle et al., JCGT 2014) of `size`
        /// by `size` texels in rows along x, two floats a texel, the mean and the mean squared
        /// distance from the probe to the closest surface around its direction. Empty removes it.
        void setVisibility(std::span<const float> data, uint32_t size = 0);
        const std::vector<float>& visibilityData() const { return _visibility; }
        uint32_t visibilitySize() const { return _visibilitySize; }

        /// The distance, in world units, a surface is offset along its normal when sampling the
        /// probes, which lessens the darkening by the probes behind it. 0.5 by default.
        float normalBias() const { return _normalBias; }
        void setNormalBias(float value);
        /// The multiplier of the probes' ambient. 1 by default.
        float intensity() const { return _intensity; }
        void setIntensity(float value);
        /// Sharpens the lighting the L1 harmonics reconstruct with a quadratic zonal harmonic along
        /// the dominant direction of the light, estimated from how directional the L1 coefficients
        /// are (ZH3, Roughton et al., I3D 2024). On by default.
        bool shSharpening() const { return _shSharpening; }
        void setShSharpening(bool value);
        /// The surfaces ignore the invalid probes and favour the probes in front of them. On by default.
        bool leakReduction() const { return _leakReduction; }
        void setLeakReduction(bool value);
        /// The surfaces ignore the probes they are hidden from, by the Chebyshev bound of the
        /// visibility (as in DDGI); no effect without visibility. On by default.
        bool visibility() const { return _useVisibility; }
        void setVisibilityEnabled(bool value);
        /// The probes occlude the environment reflections where they are darker than the
        /// environment, by the ratio of the luminances, up to 1. On by default.
        bool reflectionOcclusion() const { return _reflectionOcclusion; }
        void setReflectionOcclusion(bool value);
        /// The distance over which the ambient blends from the probes to the environment's
        /// outside the box. 1 by default.
        float blendDistance() const { return _blendDistance; }
        void setBlendDistance(float value);
        /// Whether the probes provide the ambient; off, the materials take the environment's.
        bool enabled() const { return _enabled; }
        void setEnabled(bool value);

        /// The world position of a probe, the centre of its cell.
        Vector3 probePosition(uint32_t index) const;
        /// The size of a cell.
        Vector3 cellSize() const;

        /// Applies the probe lighting to a material (Material::setProbeVolume), or removes it.
        static void applyToMaterial(Material& material, bool apply = true);

        /// The placement, the validity, the offsets, the visibility (as half floats) and every
        /// lighting, in the format upstream's volumes are saved in: the data shared by the
        /// lightings first, each lighting after it.
        std::vector<uint8_t> serialize() const;
        /// A volume from serialize's data, showing its first lighting; null for data it cannot read.
        static std::unique_ptr<ProbeVolume> deserialize(GraphicsDevice* device, std::span<const uint8_t> buffer,
            PixelFormat format = PixelFormat::PIXELFORMAT_RGBA16F);

        /// The blocks the forward shaders read, as handed to the device: what every camera shows,
        /// and what the cameras of a bake capture with.
        const ProbeVolumeBlock& displayBlock() const { return _display; }
        const ProbeVolumeBlock& captureBlock() const { return _capture; }

    private:
        std::unique_ptr<Texture> createProbeTexture(const char* name, PixelFormat format) const;
        void updateTexture();
        void uploadData();
        void encodeProbes(Texture& texture, std::span<const float> data, bool eightBit) const;
        void uploadValidity();
        void updateVisibilityTexture();
        void updateUniforms();

        GraphicsDevice* _device = nullptr;
        BoundingBox _aabb;
        Vector3 _min;
        Vector3 _max;
        std::array<uint32_t, 3> _resolution{1, 1, 1};
        PixelFormat _format = PixelFormat::PIXELFORMAT_RGBA16F;

        std::vector<std::vector<float>> _lightings;
        uint32_t _activeLighting = 0;
        std::vector<uint8_t> _validity;
        std::vector<float> _offsets;
        std::vector<float> _visibility;
        uint32_t _visibilitySize = 0;
        uint32_t _visibilityTilesPerRow = 1;
        bool _hasData = false;
        // The scale of L0 of 8-bit texels: the brightest L0 of the probes.
        float _l0Scale = 1.0f;

        std::unique_ptr<Texture> _texture;
        // Half floats whatever the format; while a bake runs.
        std::unique_ptr<Texture> _captureTexture;
        // For each probe, in rgb its offset, in a the validity of the eight probes of the cell it
        // is the first corner of.
        std::unique_ptr<Texture> _validityTexture;
        // The octahedral maps of the visibility in tiles with a one-texel border, or one texel:
        // the mean distance in red, the mean squared distance in green.
        std::unique_ptr<Texture> _visibilityTexture;

        float _normalBias = 0.5f;
        float _intensity = 1.0f;
        bool _shSharpening = true;
        bool _leakReduction = true;
        bool _useVisibility = true;
        bool _reflectionOcclusion = true;
        float _blendDistance = 1.0f;
        bool _enabled = true;

        ProbeVolumeBlock _bakeGeometry;
        ProbeVolumeBlock _display;
        ProbeVolumeBlock _capture;
    };
}
