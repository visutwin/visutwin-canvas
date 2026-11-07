// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.11.2026
//
// The process-wide side of the bindless table (platform/graphics/bindlessResidency.h) on
// the stub device: a texture takes an index on first use and keeps it, its entry's version
// moves when its GPU object is recreated and when it dies, a freed index is reused by the
// next texture, the sampler registry hands index 0 to the default state and a new index to
// another; and the lanes a material packs (materialMapLanes.h): a map on a combined row
// keeps its texture's sampler state, a map on any other row reads through sampler 0, and a
// mesh instance's own lightmap lands in the lightmap lane of its override block.
#include <memory>
#include <vector>

#include "platform/graphics/bindlessResidency.h"
#include "platform/graphics/gpu.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/textureSamplerState.h"
#include "scene/materials/material.h"
#include "scene/materials/materialMapLanes.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // A device that gives its textures a GPU object, so a render-target use recreates one.
    class GpuTexture final : public gpu::HardwareTexture
    {
    public:
        void uploadImmediate(GraphicsDevice*) override {}
        void propertyChanged(uint32_t) override {}
    };

    class TextureDevice final : public StubGraphicsDevice
    {
    public:
        ~TextureDevice() override { releaseGpuReferences(); }
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override
        {
            return std::make_unique<GpuTexture>();
        }
    };

    TextureOptions options()
    {
        TextureOptions o;
        o.name = "residency";
        o.width = 2;
        o.height = 2;
        o.mipmaps = false;
        return o;
    }

    std::unique_ptr<Texture> makeTexture(GraphicsDevice* device, const AddressMode address = ADDRESS_REPEAT)
    {
        auto texture = std::make_unique<Texture>(device, options());
        texture->setAddressU(address);
        texture->setAddressV(address);
        return texture;
    }

    uint64_t versionOf(const uint32_t index, const BindlessKind kind = BindlessKind::Texture2D)
    {
        std::vector<BindlessResidency::Entry> entries;
        BindlessResidency::snapshot(entries, kind);
        return index < entries.size() ? entries[index].version : 0;
    }

    const Texture* textureOf(const uint32_t index, const BindlessKind kind = BindlessKind::Texture2D)
    {
        std::vector<BindlessResidency::Entry> entries;
        BindlessResidency::snapshot(entries, kind);
        return index < entries.size() ? entries[index].texture : nullptr;
    }

    uint32_t lane(const MaterialMapLanes& lanes, const int slot)
    {
        const int index = materialMapLaneForSlot(slot);
        return index < 0 ? 0u : lanes.at(static_cast<uint32_t>(index));
    }
}

int main()
{
    auto device = std::make_unique<TextureDevice>();

    // Indices: taken on first use, kept, distinct, never 0.
    {
        auto a = std::make_unique<Texture>(device.get(), options());
        auto b = std::make_unique<Texture>(device.get(), options());
        const uint32_t ia = a->bindlessIndex();
        const uint32_t ib = b->bindlessIndex();
        check(ia != BindlessResidency::kNone && ib != BindlessResidency::kNone, "a texture's index is never 0");
        check(ia != ib, "two textures take distinct indices");
        check(a->bindlessIndex() == ia, "the index is kept");
        check(textureOf(ia) == a.get(), "the registry names the texture at its index");

        // The version moves when the GPU object is recreated.
        const uint64_t before = versionOf(ia);
        a->setRenderTargetUse(true);   // an empty texture becoming a render target recreates its GPU object
        check(versionOf(ia) > before, "a recreated GPU object moves the entry's version");
        const uint64_t afterRecreate = versionOf(ia);
        a->noteGpuObjectChanged();
        check(versionOf(ia) > afterRecreate, "noteGpuObjectChanged moves the version");

        // Release: the entry loses its texture, moves its version, and the index is reused.
        const uint64_t last = versionOf(ia);
        a.reset();
        check(textureOf(ia) == nullptr, "a destroyed texture leaves its entry");
        check(versionOf(ia) > last, "a release moves the version");
        auto c = std::make_unique<Texture>(device.get(), options());
        check(c->bindlessIndex() == ia, "a freed index is reused by the next texture");
        check(textureOf(ia) == c.get(), "the reused index names the new texture");
    }

    // Kinds: an array and a volume take indices in spaces of their own, a cube map none.
    {
        TextureOptions arrayOptions = options();
        arrayOptions.arrayLength = 2;
        auto array = std::make_unique<Texture>(device.get(), arrayOptions);
        TextureOptions volumeOptions = options();
        volumeOptions.volume = true;
        volumeOptions.depth = 2;
        auto volume = std::make_unique<Texture>(device.get(), volumeOptions);
        TextureOptions cubeOptions = options();
        cubeOptions.cubemap = true;
        auto cube = std::make_unique<Texture>(device.get(), cubeOptions);
        auto plain = makeTexture(device.get());
        check(array->bindlessKind() == BindlessKind::Texture2DArray && volume->bindlessKind() == BindlessKind::Texture3D &&
            plain->bindlessKind() == BindlessKind::Texture2D && cube->bindlessKind() == BindlessKind::None,
            "a texture's kind follows its shape");
        const uint32_t ia = array->bindlessIndex();
        const uint32_t iv = volume->bindlessIndex();
        check(ia != 0 && iv != 0 && textureOf(ia, BindlessKind::Texture2DArray) == array.get() &&
            textureOf(iv, BindlessKind::Texture3D) == volume.get(), "an array and a volume are registered in their own spaces");
        check(textureOf(ia, BindlessKind::Texture2D) != array.get(), "an array's index means nothing in the 2D space");
        check(cube->bindlessIndex() == 0, "a cube map packs lane 0");
    }

    // Sampler registry: the default state is index 0, another state a new index, the same
    // state the same index.
    {
        const TextureSamplerState defaultState = BindlessSamplers::defaultState();
        check(BindlessSamplers::indexOf(defaultState) == 0, "the default sampler state is index 0");
        TextureSamplerState clamp = defaultState;
        clamp.addressU = ADDRESS_CLAMP_TO_EDGE;
        clamp.addressV = ADDRESS_CLAMP_TO_EDGE;
        const uint32_t clampIndex = BindlessSamplers::indexOf(clamp);
        check(clampIndex != 0, "a clamping state gets an index of its own");
        check(BindlessSamplers::indexOf(clamp) == clampIndex, "the same state answers the same index");
    }

    // Lanes: a material packs its maps' indices and sampler indices.
    {
        auto repeatMap = makeTexture(device.get());
        auto clampMap = makeTexture(device.get(), ADDRESS_CLAMP_TO_EDGE);
        auto glossMap = makeTexture(device.get(), ADDRESS_CLAMP_TO_EDGE);
        StandardMaterial material;
        material.setDiffuseMap(repeatMap.get());
        material.setNormalMap(clampMap.get());
        material.setGlossMap(glossMap.get());
        const MaterialMapLanes& lanes = material.packedMapLanes();
        check(bindlessLaneTexture(lane(lanes, 0)) == repeatMap->bindlessIndex(), "the diffuse map's lane carries its index");
        check(bindlessLaneSampler(lane(lanes, 0)) == 0, "a repeating map on a combined row reads through sampler 0");
        check(bindlessLaneTexture(lane(lanes, 1)) == clampMap->bindlessIndex(), "the normal map's lane carries its index");
        check(bindlessLaneSampler(lane(lanes, 1)) != 0, "a clamping map on a combined row keeps its own sampler state");
        check(bindlessLaneTexture(lane(lanes, 31)) == glossMap->bindlessIndex(), "the gloss map's lane carries its index");
        check(bindlessLaneSampler(lane(lanes, 31)) == 0, "a map on a separate-image row reads through sampler 0 whatever its state");
        check(lane(lanes, 5) == 0, "a map the material does not set packs 0");
        material.setDiffuseMap(nullptr);
        check(lane(material.packedMapLanes(), 0) == 0, "clearing a map clears its lane");

        // A material with a block of its own packs lanes the same way, from its slots.
        class SlotMaterial final : public Material
        {
        public:
            const void* customUniformData(size_t& outSize) const override
            {
                outSize = sizeof(_block);
                return _block.data();
            }
        private:
            std::array<float, 4> _block{};
        };
        SlotMaterial custom;
        custom.setBaseColorTexture(clampMap.get());
        check(bindlessLaneTexture(lane(custom.packedMapLanes(), 0)) == clampMap->bindlessIndex(),
            "a custom-block material's slots pack lanes too");

        // The draw's lanes: the bound material's, with the instance lightmap written over the
        // lightmap lane, and nothing for no material.
        auto lightMap = std::make_shared<Texture>(device.get(), options());
        auto shared = std::make_shared<StandardMaterial>();
        device->setMaterial(nullptr);
        check(device->drawMapLanes() == MaterialMapLanes{}, "a draw without a material carries zero lanes");
        device->setMaterial(shared.get());
        device->setInstanceLightMap(lightMap.get());
        check(bindlessLaneTexture(lane(device->drawMapLanes(), kLightMapTextureSlot)) == lightMap->bindlessIndex(),
            "an instance lightmap is written into the draw's lightmap lane");
        const uint64_t withLightMap = device->drawMapLanesVersion();
        device->drawMapLanes();
        check(device->drawMapLanesVersion() == withLightMap, "unchanged inputs keep the lanes' version");
        device->setInstanceLightMap(nullptr);
        check(lane(device->drawMapLanes(), kLightMapTextureSlot) == 0 && device->drawMapLanesVersion() != withLightMap,
            "clearing the instance lightmap clears the lane and moves the version");
        check(lane(shared->packedMapLanes(), kLightMapTextureSlot) == 0, "the shared material's own lane stays 0");
        device->setMaterial(nullptr);
    }

    return finish("bindless residency");
}
