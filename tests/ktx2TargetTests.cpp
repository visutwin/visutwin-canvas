// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.09.2026
//
// A KTX2 (Basis) payload is transcoded to a format the DEVICE says it can create.
//
// ASTC is Apple-only and BC desktop-only, and creating an image in a format the GPU
// lacks FAILS rather than degrades. GraphicsDevice::preferredCompressedRgbaFormat picks
// ASTC -> BC7 -> DXT5 -> RGBA8 from supportsCompressedFormat, and there are FOUR
// transcode call sites that must each use it: the texture asset (every example), the
// async TextureResourceHandler, and the glTF KHR_texture_basisu images on the
// synchronous and prepared paths. A site that hard-codes ASTC fails on every desktop
// Vulkan GPU. Changing one call site changes nothing, so each one is driven here by a
// device that answers exactly one format.
//
// CPU only: the stub device creates no GPU objects; the textures record their format.

#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "framework/assets/asset.h"
#include "framework/handlers/resourceLoader.h"
#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/texture/ktx2Transcoder.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "support/check.h"
#include "support/gltfModel.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    /// Supports exactly the compressed formats it is given, and every uncompressed one.
    class FormatDevice final : public StubGraphicsDevice
    {
    public:
        explicit FormatDevice(std::set<PixelFormat> compressed)
            : StubGraphicsDevice(Options{.cpuBuffers = true}), _compressed(std::move(compressed))
        {
        }

        bool supportsCompressedFormat(const PixelFormat format) const override
        {
            return !isCompressedPixelFormat(format) || _compressed.count(format) != 0;
        }

    private:
        std::set<PixelFormat> _compressed;
    };

    const char* name(const PixelFormat format)
    {
        switch (format) {
        case PixelFormat::PIXELFORMAT_ASTC_4x4: return "ASTC 4x4";
        case PixelFormat::PIXELFORMAT_BC7: return "BC7";
        case PixelFormat::PIXELFORMAT_DXT5: return "DXT5";
        case PixelFormat::PIXELFORMAT_RGBA8: return "RGBA8";
        default: return "?";
        }
    }

    /// Bytes of a w x h level: 16 per 4x4 block for the three block formats.
    size_t levelBytes(const PixelFormat format, const uint32_t w, const uint32_t h)
    {
        if (format == PixelFormat::PIXELFORMAT_RGBA8) {
            return static_cast<size_t>(w) * h * 4;
        }
        return static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * 16;
    }

    const Vec3Bounds kVec3Bounds{{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};

    /// One textured triangle whose base colour image is the KTX2 file, as
    /// KHR_texture_basisu stores it: the raw bytes, never decoded by tinygltf.
    tinygltf::Model basisuModel(const std::vector<uint8_t>& ktx2)
    {
        tinygltf::Model model;
        model.asset.version = "2.0";
        tinygltf::Buffer buffer;
        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        primitive.material = 0;
        primitive.attributes["POSITION"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC3, kVec3Bounds);
        primitive.attributes["NORMAL"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f}, 3, TINYGLTF_TYPE_VEC3, kVec3Bounds);
        primitive.attributes["TEXCOORD_0"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f}, 3, TINYGLTF_TYPE_VEC2);
        model.buffers.push_back(buffer);
        tinygltf::Mesh mesh;
        mesh.primitives.push_back(primitive);
        model.meshes.push_back(mesh);
        tinygltf::Node node;
        node.mesh = 0;
        model.nodes.push_back(node);
        tinygltf::Scene scene;
        scene.nodes = {0};
        model.scenes.push_back(scene);
        model.defaultScene = 0;

        tinygltf::Image image;
        image.name = "checkboard.ktx2";
        image.mimeType = "image/ktx2";
        image.image.assign(ktx2.begin(), ktx2.end());
        model.images.push_back(image);
        tinygltf::Texture texture;
        texture.source = -1;
        tinygltf::Value::Object basisu;
        basisu["source"] = tinygltf::Value(0);
        texture.extensions["KHR_texture_basisu"] = tinygltf::Value(basisu);
        model.textures.push_back(texture);
        model.extensionsUsed = {"KHR_texture_basisu"};

        tinygltf::Material material;
        material.pbrMetallicRoughness.baseColorTexture.index = 0;
        model.materials.push_back(material);
        return model;
    }

    PixelFormat containerBaseColourFormat(const GlbContainerResource* container, bool& found)
    {
        found = container && !container->meshPayloads().empty() && container->meshPayloads()[0].material &&
                container->meshPayloads()[0].material->baseColorTexture();
        return found ? container->meshPayloads()[0].material->baseColorTexture()->format()
                     : PixelFormat::PIXELFORMAT_RGBA8;
    }
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "the preferred target, in upstream's order\n";
    {
        using PF = PixelFormat;
        check(FormatDevice({PF::PIXELFORMAT_ASTC_4x4, PF::PIXELFORMAT_BC7, PF::PIXELFORMAT_DXT5})
                  .preferredCompressedRgbaFormat() == PF::PIXELFORMAT_ASTC_4x4, "ASTC first where it exists (Apple)");
        check(FormatDevice({PF::PIXELFORMAT_BC7, PF::PIXELFORMAT_DXT5}).preferredCompressedRgbaFormat() ==
                  PF::PIXELFORMAT_BC7, "BC7 on a desktop GPU without ASTC");
        check(FormatDevice({PF::PIXELFORMAT_DXT5}).preferredCompressedRgbaFormat() == PF::PIXELFORMAT_DXT5,
            "DXT5 when BC7 is missing too");
        check(FormatDevice({}).preferredCompressedRgbaFormat() == PF::PIXELFORMAT_RGBA8,
            "uncompressed RGBA8 when the device answers nothing");
        check(FormatDevice({PF::PIXELFORMAT_ASTC_4x4}).preferredCompressedRgbaFormat() == PF::PIXELFORMAT_ASTC_4x4 &&
              FormatDevice({PF::PIXELFORMAT_BC7}).preferredCompressedRgbaFormat() == PF::PIXELFORMAT_BC7,
            "each format is chosen on its own answer, not implied by another");
    }

    const std::string path = std::string(VISUTWIN_SOURCE_DIR) + "/assets/textures/checkboard.ktx2";
    std::vector<uint8_t> ktx2;
    {
        std::ifstream in(path, std::ios::binary);
        ktx2.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    check(Ktx2Transcoder::isKtx2(ktx2.data(), ktx2.size()), "the test file is KTX2 (" + path + ")");

    for (const PixelFormat target : {PixelFormat::PIXELFORMAT_ASTC_4x4, PixelFormat::PIXELFORMAT_BC7,
                                     PixelFormat::PIXELFORMAT_DXT5, PixelFormat::PIXELFORMAT_RGBA8}) {
        const std::string label = name(target);
        std::cout << "\na device that can create only " << label << "\n";
        auto device = target == PixelFormat::PIXELFORMAT_RGBA8
            ? std::make_shared<FormatDevice>(std::set<PixelFormat>{})
            : std::make_shared<FormatDevice>(std::set<PixelFormat>{target});
        check(device->preferredCompressedRgbaFormat() == target, "prefers " + label);

        // The transcoder: the level data has to BE that format, not just be labelled it.
        const auto transcoded = Ktx2Transcoder::transcode(ktx2.data(), ktx2.size(), path, target);
        bool sizes = transcoded.valid && !transcoded.levels.empty();
        for (size_t level = 0; sizes && level < transcoded.levels.size(); ++level) {
            const uint32_t w = std::max(1u, transcoded.width >> level);
            const uint32_t h = std::max(1u, transcoded.height >> level);
            sizes = transcoded.levels[level].size() == levelBytes(target, w, h);
        }
        check(transcoded.valid && transcoded.format == target && sizes,
            "the transcoder writes " + label + " (" + std::to_string(transcoded.levels.size()) +
            " levels, each the size that format needs)");

        // 1. The texture asset: the path every example's KTX2 goes through.
        Asset::setDefaultGraphicsDevice(device);
        {
            Asset asset("checkboard", AssetType::TEXTURE, path);
            const auto resource = asset.resource();
            Texture* texture = resource && std::holds_alternative<Texture*>(*resource)
                ? std::get<Texture*>(*resource) : nullptr;
            check(texture && texture->format() == target, "the texture asset is created as " + label);
        }
        {
            // An RGBP texture type is honoured for block-compressed data, as the async path
            // always did: the synchronous path once forced every KTX2 texture to Default.
            AssetData data;
            data.type = TextureType::TEXTURETYPE_RGBP;
            Asset asset("checkboard-rgbp", AssetType::TEXTURE, path, data);
            const auto resource = asset.resource();
            Texture* texture = resource && std::holds_alternative<Texture*>(*resource)
                ? std::get<Texture*>(*resource) : nullptr;
            check(texture && texture->encoding() == TextureEncoding::RGBP,
                "a texture asset typed RGBP is sampled as RGBP");
        }

        // 2. The async texture handler, built as Engine builds it.
        {
            TextureResourceHandler handler(device->preferredCompressedRgbaFormat());
            const auto loaded = handler.load(path);
            check(loaded && loaded->pixelData && loaded->pixelData->isCompressed &&
                      static_cast<PixelFormat>(loaded->pixelData->compressedFormat) == target,
                "the async texture handler transcodes to " + label);
        }

        // 3. and 4. KHR_texture_basisu, through the synchronous and the prepared glTF paths.
        forEachLoadPath([&ktx2] { return basisuModel(ktx2); }, device, "ktx2TargetTests",
            [&](const GlbContainerResource* container, const LoadPath path) {
                bool found = false;
                const PixelFormat format = containerBaseColourFormat(container, found);
                check(found && format == target, path == LoadPath::CreateFromModel
                        ? "a KHR_texture_basisu image (createFromModel) is " + label
                        : std::string("and through prepareFromModel + createFromPrepared"));
            },
            device->preferredCompressedRgbaFormat());
        Asset::setDefaultGraphicsDevice(nullptr);
    }

    return finish("KTX2 target");
}
