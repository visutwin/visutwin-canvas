// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A KTX2 (Basis) payload is transcoded to a format the DEVICE says it can create.
//
// ASTC is Apple-only and BC desktop-only, and creating an image in a format the GPU
// lacks FAILS rather than degrades. GraphicsDevice::preferredCompressedRgbaFormat picks
// ASTC -> BC7 -> DXT5 -> RGBA8 from supportsCompressedFormat, and there are FOUR
// transcode call sites that must each use it: the texture asset (every example), the
// async TextureResourceHandler, and the glTF KHR_texture_basisu images on the
// synchronous and prepared paths. Until 2026-09-06 they hard-coded ASTC, which no
// desktop Vulkan GPU can create. Changing one call site changes nothing, so each one
// is driven here by a device that answers exactly one format.
//
// CPU only: the stub device creates no GPU objects; the textures record their format.

#include <tiny_gltf.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "framework/assets/asset.h"
#include "framework/handlers/resourceLoader.h"
#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "framework/parsers/texture/ktx2Transcoder.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/materials/material.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    class CpuVertexBuffer final : public VertexBuffer
    {
    public:
        using VertexBuffer::VertexBuffer;
        void unlock() override {}
    };

    class CpuIndexBuffer final : public IndexBuffer
    {
    public:
        using IndexBuffer::IndexBuffer;
        bool setData(const std::vector<uint8_t>&) override { return true; }
    };

    /// Supports exactly the compressed formats it is given, and every uncompressed one.
    class FormatDevice final : public GraphicsDevice
    {
    public:
        explicit FormatDevice(std::set<PixelFormat> compressed) : _compressed(std::move(compressed)) {}
        bool supportsCompressedFormat(const PixelFormat format) const override
        {
            return !isCompressedPixelFormat(format) || _compressed.count(format) != 0;
        }

        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            const int numVertices, const VertexBufferOptions& options) override
        {
            return std::make_shared<CpuVertexBuffer>(this, format, numVertices, options);
        }
        std::shared_ptr<IndexBuffer> createIndexBuffer(const IndexFormat format, const int numIndices,
            const std::vector<uint8_t>& data) override
        {
            auto buffer = std::make_shared<CpuIndexBuffer>(this, format, numIndices);
            buffer->setData(data);
            return buffer;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }

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

    int addAccessor(tinygltf::Model& model, std::vector<unsigned char>& bytes, const std::vector<float>& values,
        const int count, const int type)
    {
        const size_t at = bytes.size();
        bytes.resize(at + values.size() * sizeof(float));
        std::memcpy(bytes.data() + at, values.data(), values.size() * sizeof(float));
        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = at;
        view.byteLength = values.size() * sizeof(float);
        model.bufferViews.push_back(view);
        tinygltf::Accessor accessor;
        accessor.bufferView = static_cast<int>(model.bufferViews.size()) - 1;
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        accessor.count = static_cast<size_t>(count);
        accessor.type = type;
        if (type == TINYGLTF_TYPE_VEC3) {
            accessor.minValues = {0.0, 0.0, 0.0};
            accessor.maxValues = {1.0, 1.0, 1.0};
        }
        model.accessors.push_back(accessor);
        return static_cast<int>(model.accessors.size()) - 1;
    }

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
            {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC3);
        primitive.attributes["NORMAL"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f}, 3, TINYGLTF_TYPE_VEC3);
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

        // 2. The async texture handler, built as Engine builds it.
        {
            TextureResourceHandler handler(device->preferredCompressedRgbaFormat());
            const auto loaded = handler.load(path);
            check(loaded && loaded->pixelData && loaded->pixelData->isCompressed &&
                      static_cast<PixelFormat>(loaded->pixelData->compressedFormat) == target,
                "the async texture handler transcodes to " + label);
        }

        // 3. and 4. KHR_texture_basisu, through the synchronous and the prepared glTF paths.
        {
            tinygltf::Model model = basisuModel(ktx2);
            const auto container = GlbParser::createFromModel(model, device, "ktx2TargetTests");
            bool found = false;
            const PixelFormat format = containerBaseColourFormat(container.get(), found);
            check(found && format == target, "a KHR_texture_basisu image (createFromModel) is " + label);
        }
        {
            tinygltf::Model model = basisuModel(ktx2);
            auto prepared = GlbParser::prepareFromModel(model, device->preferredCompressedRgbaFormat(), "ktx2");
            const auto container = GlbParser::createFromPrepared(model, std::move(prepared), device, "ktx2");
            bool found = false;
            const PixelFormat format = containerBaseColourFormat(container.get(), found);
            check(found && format == target, "and through prepareFromModel + createFromPrepared");
        }
        Asset::setDefaultGraphicsDevice(nullptr);
    }

    std::cout << (failures == 0 ? "\nAll KTX2 target tests passed\n" : "\nKTX2 target tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
