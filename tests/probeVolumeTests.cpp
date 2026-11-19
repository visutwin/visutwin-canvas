// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// The probe volume (extras/probeVolume), on a stub device. Upstream's cases are ported first:
// the volume shows the environment's ambient until its probes are set and while it is disabled,
// a deserialized volume has its probes, setData adds a lighting at the index of the lighting
// count, activeLighting switches between them, and serialize restores every lighting. Then what
// upstream's cases leave to its implementation: the optional parts of the data (validity,
// offsets, visibility as half floats), the probes upstream's example ships, which must come back
// byte for byte, the texels of both encodings, the validity texture's corner masks and offsets,
// the half floats, the blocks the device hands each camera, and the material flag. Then the baker:
// upstream's cases (isSupported, a capture refused on a device that cannot bake, the shadow bounds
// of a bake), and the dilation, which upstream's cases leave untested.
//
// DEVIATION: upstream's two cases of a device rendering to full floats that it cannot filter or
// blend have no counterpart: this engine asks one capability per format
// (GraphicsDevice::textureFloatRenderable, which covers both).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

#include "core/math/halfFloat.h"
#include "extras/probeVolume/probeVolume.h"
#include "extras/probeVolume/probeVolumeBaker.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // A stub device with the float rendering capabilities given.
    class FloatDevice final : public StubGraphicsDevice
    {
    public:
        FloatDevice(const bool half, const bool full)
        {
            setTextureHalfFloatRenderable(half);
            setTextureFloatRenderable(full);
        }
    };

    // A device that draws nothing by design, as the null device says.
    class SilentDevice final : public StubGraphicsDevice
    {
    public:
        SilentDevice()
        {
            setTextureHalfFloatRenderable(true);
            setTextureFloatRenderable(true);
        }
        bool reportsUnsupportedFeatures() const override { return false; }
    };

    std::vector<float> lighting(const ProbeVolume& volume, const float value)
    {
        return std::vector<float>(static_cast<size_t>(volume.probeCount()) * ProbeVolume::kFloatsPerProbe, value);
    }

    // The weight of the probes the shaders read, 0 showing the environment's ambient.
    float probeWeight(const GraphicsDevice& device)
    {
        return device.probeVolumeBlock(false).blend[1];
    }

    std::unique_ptr<ProbeVolume> makeVolume(GraphicsDevice* device)
    {
        return std::make_unique<ProbeVolume>(device, BoundingBox(Vector3(0.0f, 1.0f, 0.0f), Vector3(2.0f, 1.0f, 2.0f)),
            std::array<uint32_t, 3>{2, 1, 2});
    }

    bool sameFloats(std::span<const float> a, std::span<const float> b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
    }

    std::vector<uint8_t> readFile(const char* path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
    }
}

int main()
{
    std::cout << std::unitbuf;
    const auto device = std::make_shared<StubGraphicsDevice>();

    std::cout << "#hasData\n";
    {
        auto volume = makeVolume(device.get());
        check(!volume->hasData(), "a new volume has no probes");
        check(probeWeight(*device) == 0.0f, "it shows the environment's ambient until the probes are set");
        volume->setData(lighting(*volume, 0.5f));
        check(volume->hasData(), "setData sets the probes");
        check(probeWeight(*device) == 1.0f, "which the shaders then read");

        volume->setEnabled(false);
        check(probeWeight(*device) == 0.0f, "a disabled volume shows the environment's ambient, with probes");
        volume->setEnabled(true);
        check(probeWeight(*device) == 1.0f, "and its probes again when enabled");

        const auto loaded = ProbeVolume::deserialize(device.get(), volume->serialize());
        check(loaded && loaded->hasData(), "a deserialized volume has its probes");
        check(loaded && loaded->lightingCount() == 1, "and its one lighting");
        check(loaded && sameFloats(loaded->data(), volume->data()), "equal to the volume's");
    }

    std::cout << "#setData\n";
    {
        auto volume = makeVolume(device.get());
        check(volume->lightingCount() == 1, "a volume has one lighting when created");
        volume->setData(lighting(*volume, 0.25f));
        volume->setData(lighting(*volume, 0.75f), 1);
        check(volume->lightingCount() == 2, "setting the lighting at the lighting count adds one");
        check(volume->data(0)[0] == 0.25f && volume->data(1)[0] == 0.75f, "each lighting keeps its data");
        volume->setData(lighting(*volume, 1.0f), 3);
        check(volume->lightingCount() == 2, "an index beyond the lighting count adds nothing");
        volume->setData(std::vector<float>(5, 1.0f), 0);
        check(volume->data(0)[0] == 0.25f, "data of the wrong size changes nothing");
    }

    std::cout << "#activeLighting\n";
    {
        auto volume = makeVolume(device.get());
        volume->setData(lighting(*volume, 0.25f));
        volume->setData(lighting(*volume, 0.75f), 1);
        check(volume->activeLighting() == 0, "defaults to the first lighting");
        volume->setActiveLighting(1);
        check(volume->activeLighting() == 1, "switches to another one");
        volume->setActiveLighting(2);
        check(volume->activeLighting() == 1, "and not to a lighting it does not have");
    }

    std::cout << "#serialize\n";
    {
        auto volume = makeVolume(device.get());
        volume->setData(lighting(*volume, 0.25f));
        volume->setData(lighting(*volume, 0.75f), 1);
        const auto loaded = ProbeVolume::deserialize(device.get(), volume->serialize());
        check(loaded && loaded->lightingCount() == 2, "restores every lighting");
        check(loaded && loaded->activeLighting() == 0, "showing the first one");
        check(loaded && sameFloats(loaded->data(0), volume->data(0)) && sameFloats(loaded->data(1), volume->data(1)),
            "with their data");
        check(loaded && loaded->resolution() == volume->resolution(), "and the resolution");
        check(loaded && near(loaded->aabbMin(), volume->aabbMin(), 0.0f) &&
            near(loaded->aabbMax(), volume->aabbMax(), 0.0f), "and the box");

        // The optional parts: validity, offsets and visibility, the last as half floats.
        const std::vector<uint8_t> validity = {1, 0, 1, 0};
        volume->setValidity(validity);
        std::vector<float> offsets(12, 0.0f);
        offsets[3] = 0.25f;
        offsets[7] = -0.5f;
        volume->setOffsets(offsets);
        std::vector<float> visibility(4 * 2 * 2 * 2);
        for (size_t i = 0; i < visibility.size(); ++i) {
            visibility[i] = 0.1f * static_cast<float>(i) + 0.013f;
        }
        volume->setVisibility(visibility, 2);
        const std::vector<uint8_t> bytes = volume->serialize();
        const auto full = ProbeVolume::deserialize(device.get(), bytes);
        check(full && full->validity() == volume->validity() && full->invalidProbeCount() == 2, "restores the validity");
        check(full && full->offsets() == offsets, "restores the offsets");
        bool halves = full && full->visibilitySize() == 2 && full->visibilityData().size() == visibility.size();
        for (size_t i = 0; halves && i < visibility.size(); ++i) {
            halves = full->visibilityData()[i] == halfToFloat(floatToHalf(visibility[i]));
        }
        check(halves, "restores the visibility, rounded to half floats");
        check(full && full->serialize() == bytes, "and serializes the same bytes again");

        std::vector<uint8_t> broken = bytes;
        broken[0] ^= 0xffu;
        check(!ProbeVolume::deserialize(device.get(), broken), "refuses data of another kind");
        broken = bytes;
        broken.resize(bytes.size() - 4);
        check(!ProbeVolume::deserialize(device.get(), broken), "and truncated data");
    }

    std::cout << "the probes upstream's example ships\n";
    {
        const std::vector<uint8_t> bytes = readFile(VISUTWIN_ASSETS_DIR "/models/concrete-pavilion-probes.bin");
        const auto volume = ProbeVolume::deserialize(device.get(), bytes);
        check(volume != nullptr, "load");
        if (volume) {
            check(volume->lightingCount() == 3, "with the three lightings of the presets");
            check(volume->visibilitySize() > 0, "and the visibility");
            check(volume->serialize() == bytes, "and serialize to the same bytes");
        }
    }

    std::cout << "the texels\n";
    {
        auto volume = makeVolume(device.get());
        std::vector<float> data = lighting(*volume, 0.0f);
        // probe 3 (x 1, y 0, z 1), green: L0 2, L1 (1, -1, 0.5)
        const size_t green = (3 * 3 + 1) * 4;
        data[green] = 2.0f;
        data[green + 1] = 1.0f;
        data[green + 2] = -1.0f;
        data[green + 3] = 0.5f;
        // probe 0, red, the brightest L0 of the 8-bit encoding
        data[0] = 4.0f;
        volume->setData(data);
        // texel (x 1, y 0, z 1 + 2 * 1) of the half float texture, the green sub-volume
        const size_t dst = (1 + 2 * (0 + 1 * (1 + 2 * 1))) * 4;
        const auto* halves = reinterpret_cast<const uint16_t*>(volume->texture()->getLevel(0));
        check(halves && halfToFloat(halves[dst]) == 2.0f && halfToFloat(halves[dst + 1]) == 1.0f &&
            halfToFloat(halves[dst + 2]) == -1.0f && halfToFloat(halves[dst + 3]) == 0.5f,
            "half floats keep the coefficients as they are, in the channel's sub-volume");

        volume->setFormat(PixelFormat::PIXELFORMAT_RGBA8);
        const auto* bytes = static_cast<const uint8_t*>(volume->texture()->getLevel(0));
        // sqrt(2 / 4) and L1 / (sqrt(3) * 2) mapped to 0..1
        check(bytes && bytes[dst] == static_cast<uint8_t>(std::lround(std::sqrt(0.5f) * 255.0f)),
            "8-bit texels store sqrt(L0 / the brightest L0)");
        check(bytes && bytes[dst + 1] == static_cast<uint8_t>(std::lround((1.0f / (std::sqrt(3.0f) * 2.0f) * 0.5f + 0.5f) * 255.0f)) &&
            bytes[dst + 3] == static_cast<uint8_t>(std::lround((0.5f / (std::sqrt(3.0f) * 2.0f) * 0.5f + 0.5f) * 255.0f)),
            "and L1 relative to sqrt(3) L0");
        const ProbeVolumeBlock& block = device->probeVolumeBlock(false);
        check(block.encoding[0] == 1.0f && block.encoding[1] == 4.0f, "and the shaders read the scale of L0");
        check(sameFloats(volume->data(), data), "a format change keeps the probes' data");
    }

    std::cout << "the validity texture\n";
    {
        auto volume = makeVolume(device.get());
        volume->setValidity(std::vector<uint8_t>{1, 0, 1, 1});
        std::vector<float> offsets(12, 0.0f);
        offsets[0] = 0.5f;   // a quarter of probe 0's cell of 2
        offsets[2] = -2.0f;  // beyond half a cell, clamped
        volume->setOffsets(offsets);
        const auto* texels = static_cast<const uint8_t*>(volume->validityTexture()->getLevel(0));
        // probe 0's cell has corners 0..7 as probes (x, y, z) + bits; probe 1 (x 1) is invalid,
        // and the corners outside the grid count as valid
        check(texels && texels[3] == static_cast<uint8_t>(0xff & ~0x02), "a probe holds the validity of its cell's corners");
        check(texels && texels[3 * 4 + 3] == 0xff, "a cell at the border counts the probes outside the grid as valid");
        check(texels && texels[0] == 128 + static_cast<int>(std::lround(0.25f * 254.0f)) && texels[1] == 128 &&
            texels[2] == 128 - 127, "and its offset in units of its cell, limited to half a cell");
    }

    std::cout << "the blocks of the cameras\n";
    {
        auto volume = makeVolume(device.get());
        volume->setData(lighting(*volume, 0.5f));
        volume->setIntensity(3.0f);
        volume->setEnabled(false);
        check(device->probeVolumeBlock(true).blend[1] == 0.0f, "without bake data a capture camera reads what the others read");
        volume->setBakeData(lighting(*volume, 0.25f));
        const ProbeVolumeBlock& capture = device->probeVolumeBlock(true);
        check(capture.blend[1] == 1.0f && capture.params[1] == 1.0f, "with it, enabled and at the baked intensity");
        check(capture.lanes[0] != device->probeVolumeBlock(false).lanes[0], "from a texture of its own");
        check(device->probeVolumeBlock(false).params[1] == 3.0f, "while the other cameras read the volume's settings");
        volume->setBakeData({});
        check(device->probeVolumeBlock(true).blend[1] == 0.0f, "released, the capture cameras read the volume again");

        const BoundingBox box(Vector3(1.0f, 2.0f, 3.0f), Vector3(2.0f, 1.0f, 0.0f));
        ProbeVolume flat(device.get(), box, {4, 2, 1});
        const ProbeVolumeBlock& block = device->probeVolumeBlock(false);
        check(block.invSize[0] == 0.25f && block.invSize[2] == 0.0f, "an axis of no extent has an inverse size of 0");
        check(block.cellSize[0] == 1.0f && block.count[1] == 2.0f, "with the counts and the cell size");
        check(near(flat.probePosition(5), Vector3(0.5f, 2.5f, 3.0f), 1e-6f), "a probe sits at the centre of its cell");
        volume.reset();
        check(device->probeVolumeBlock(false).count[0] == 4.0f, "a volume freed does not clear a newer one");
    }
    check(device->probeVolumeBlock(false).blend[1] == 0.0f, "the last volume freed leaves no probes");

    std::cout << "the material\n";
    {
        StandardMaterial material;
        const uint64_t version = material.uniformsVersion();
        ProbeVolume::applyToMaterial(material);
        check(material.probeVolume(), "applyToMaterial makes a material take its ambient from the probes");
        check(material.uniformsVersion() != version, "which resolves its shader again");
        ProbeVolume::applyToMaterial(material, false);
        check(!material.probeVolume(), "and removes it");
    }

    std::cout << "half floats\n";
    {
        check(floatToHalf(1.0f) == 0x3c00 && floatToHalf(-2.0f) == 0xc000, "the normal halves");
        check(floatToHalf(65520.0f) == 0x7c00 && floatToHalf(1e-8f) == 0, "overflow and underflow");
        check(floatToHalf(std::ldexp(1.0f, -24)) == 1 && halfToFloat(1) == std::ldexp(1.0f, -24), "the smallest denormal");
        check(floatToHalf(1.0f + std::ldexp(1.0f, -11)) == 0x3c01, "a tie rounds up, as the saved data was rounded");
        bool exact = true;
        for (uint32_t bits = 0; bits < 0x7c00; ++bits) {
            exact = exact && floatToHalf(halfToFloat(static_cast<uint16_t>(bits))) == bits;
        }
        check(exact, "every finite half comes back");
    }

    std::cout << "ProbeVolumeBaker::isSupported\n";
    {
        const FloatDevice half(true, false);
        const FloatDevice full(false, true);
        const FloatDevice none(false, false);
        const SilentDevice silent;
        check(ProbeVolumeBaker::isSupported(&half), "true for a device rendering to half floats");
        check(ProbeVolumeBaker::isSupported(&full), "true for a device rendering to full floats, without half floats");
        check(!ProbeVolumeBaker::isSupported(&none), "false for a device rendering to no float textures");
        check(!ProbeVolumeBaker::isSupported(&silent), "false for a device that draws nothing, as the null device");
    }

    std::cout << "ProbeVolumeBaker::captureGeometry\n";
    {
        auto none = std::make_shared<FloatDevice>(false, false);
        auto engine = makeTestEngine<LightComponentSystem>(none);
        auto volume = makeVolume(none.get());
        ProbeVolumeBaker baker(engine.get(), volume.get());
        check(!baker.captureGeometry(), "refused on a device that cannot bake");
        check(!baker.baking(), "without starting a capture");
        check(!baker.bakeLighting(), "and so is a bake");
        engine->destroy();
    }

    std::cout << "ProbeVolumeBaker::setShadowBounds\n";
    {
        auto engine = makeTestEngine<LightComponentSystem>(device);
        auto volume = makeVolume(device.get());
        const BoundingBox bounds(Vector3(0.0f, 1.0f, 0.0f), Vector3(4.0f, 1.0f, 3.0f));
        const auto addLight = [&](const LightType type, const bool castShadows, const std::vector<int>& layers) {
            Entity* entity = addTo(engine->root(), newEntity(engine.get(), "light"));
            auto* light = static_cast<LightComponent*>(entity->addComponent<LightComponent>());
            light->setType(type);
            light->setCastShadows(castShadows);
            if (!layers.empty()) {
                light->setLayers(layers);
            }
            return light;
        };
        const BoundingBox own(Vector3(5.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
        LightComponent* sun = addLight(LightType::LIGHTTYPE_DIRECTIONAL, true, {});
        sun->setShadowBounds(own);
        LightComponent* unshadowed = addLight(LightType::LIGHTTYPE_DIRECTIONAL, false, {});
        LightComponent* omni = addLight(LightType::LIGHTTYPE_OMNI, true, {});
        LightComponent* uncaptured = addLight(LightType::LIGHTTYPE_DIRECTIONAL, true, {LAYERID_UI});

        ProbeVolumeBaker baker(engine.get(), volume.get());
        const auto lights = baker.setShadowBounds({LAYERID_WORLD, LAYERID_SKYBOX}, bounds);
        check(lights.size() == 1 && lights[0].light == sun, "fits the shadow-casting directional lights of the captured layers");
        check(lights.size() == 1 && lights[0].bounds && lights[0].bounds->sameAs(own), "and returns their own bounds");
        check(sun->shadowBounds() && sun->shadowBounds()->sameAs(bounds), "which now have the bake's");
        check(!unshadowed->shadowBounds() && !omni->shadowBounds() && !uncaptured->shadowBounds(),
            "leaving an unshadowed, an omni and an uncaptured light alone");

        LightComponent* both = addLight(LightType::LIGHTTYPE_DIRECTIONAL, true, {LAYERID_WORLD, LAYERID_UI});
        uncaptured->setCastShadows(false);
        sun->setCastShadows(false);
        const auto once = baker.setShadowBounds({LAYERID_WORLD, LAYERID_UI}, bounds);
        check(once.size() == 1 && once[0].light == both && !once[0].bounds, "fits a light of several captured layers once");

        both->setShadowBounds(std::nullopt);
        check(baker.setShadowBounds({LAYERID_WORLD}, std::nullopt).empty() && !both->shadowBounds(),
            "and keeps the shadows fitted to each camera without bounds");
        engine->destroy();
    }

    std::cout << "ProbeVolumeBaker::dilate\n";
    {
        // 4 x 1 x 1: probes 1 and 2 invalid, filled from 0 and 3 in the first pass, then each
        // other; and 3 x 3 x 1 with only the centre valid, filled in one pass.
        std::vector<float> data(4 * ProbeVolume::kFloatsPerProbe, 0.0f);
        std::fill_n(data.begin(), ProbeVolume::kFloatsPerProbe, 1.0f);
        std::fill_n(data.begin() + 3 * ProbeVolume::kFloatsPerProbe, ProbeVolume::kFloatsPerProbe, 3.0f);
        const uint32_t filled = ProbeVolumeBaker::dilate(data, {1, 0, 0, 1}, {4, 1, 1});
        check(filled == 2, "fills the invalid probes");
        check(data[ProbeVolume::kFloatsPerProbe] == 1.0f && data[2 * ProbeVolume::kFloatsPerProbe] == 3.0f,
            "from their valid neighbours, in one pass when each has one");

        std::vector<float> line(3 * ProbeVolume::kFloatsPerProbe, 0.0f);
        std::fill_n(line.begin(), ProbeVolume::kFloatsPerProbe, 2.0f);
        check(ProbeVolumeBaker::dilate(line, {1, 0, 0}, {3, 1, 1}) == 2 && line[2 * ProbeVolume::kFloatsPerProbe] == 2.0f,
            "and a probe behind another from the one filled before it");

        std::vector<float> grid(9 * ProbeVolume::kFloatsPerProbe, 0.0f);
        std::vector<uint8_t> valid(9, 0);
        valid[1] = 1;
        valid[3] = 1;
        std::fill_n(grid.begin() + 1 * ProbeVolume::kFloatsPerProbe, ProbeVolume::kFloatsPerProbe, 1.0f);
        std::fill_n(grid.begin() + 3 * ProbeVolume::kFloatsPerProbe, ProbeVolume::kFloatsPerProbe, 4.0f);
        ProbeVolumeBaker::dilate(grid, valid, {3, 3, 1});
        // probe 0 sees 1 (x + 1, weight 1) and 3 (y + 1, weight 1); probe 4 sees both at weight 1
        check(near(grid[0], 2.5f, 1e-6f), "weighted by the inverse square of the distance");
        check(std::count(valid.begin(), valid.end(), uint8_t{0}) == 7, "leaving the validity as it was");
        std::vector<float> none(2 * ProbeVolume::kFloatsPerProbe, 0.0f);
        check(ProbeVolumeBaker::dilate(none, {0, 0}, {2, 1, 1}) == 0, "and nothing without a valid probe");
    }

    return finish("probe-volume");
}
