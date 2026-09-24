// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A GLB loads through three paths — the synchronous parse(), createFromModel and
// prepareFromModel + createFromPrepared (the last two are what loadAsync uses) — and
// each used to build its materials with its own copy of the same code. The two async
// copies had drifted: no occlusion texture, no emissive texture, no metallic-roughness
// UV set and no KHR_materials_unlit, so a model loaded asynchronously lost its baked AO
// and its glow and an unlit model came out lit, while the synchronous load was right.
// No example loads asynchronously, which is how it lived.
//
// All three now call one createGltfMaterial. This builds a model in memory that uses
// every one of those features and checks the material each ASYNC path produces; the
// synchronous path shares the function, and loads from disk in every example.

#include <tiny_gltf.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
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

    // Mesh payloads are only created when the device hands back a vertex buffer, so
    // these keep their bytes on the CPU; textures get no GPU object, which upload()
    // tolerates.
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

    class StubDevice final : public GraphicsDevice
    {
    public:
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
    };

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
            accessor.maxValues = {1.0, 1.0, 0.0};
        }
        model.accessors.push_back(accessor);
        return static_cast<int>(model.accessors.size()) - 1;
    }

    // One triangle with two UV sets, and one material using every feature the async
    // copies dropped: occlusion (strength 0.5), emissive texture, metallic-roughness on
    // UV set 1, and KHR_materials_unlit. Four decoded 2x2 images, one per texture.
    tinygltf::Model buildModel()
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
        primitive.attributes["TEXCOORD_1"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f}, 3, TINYGLTF_TYPE_VEC2);
        model.buffers.push_back(buffer);

        tinygltf::Mesh mesh;
        mesh.name = "tri";
        mesh.primitives.push_back(primitive);
        model.meshes.push_back(mesh);

        tinygltf::Node node;
        node.name = "Tri";
        node.mesh = 0;
        model.nodes.push_back(node);
        tinygltf::Scene scene;
        scene.nodes = {0};
        model.scenes.push_back(scene);
        model.defaultScene = 0;

        for (int i = 0; i < 4; ++i) {
            tinygltf::Image image;
            image.name = "image" + std::to_string(i);
            image.width = 2;
            image.height = 2;
            image.component = 4;
            image.bits = 8;
            image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
            image.image.assign(2 * 2 * 4, static_cast<unsigned char>(60 * (i + 1)));
            model.images.push_back(image);
            tinygltf::Texture texture;
            texture.source = i;
            model.textures.push_back(texture);
        }

        tinygltf::Material material;
        material.name = "everything";
        material.pbrMetallicRoughness.baseColorTexture.index = 0;
        material.pbrMetallicRoughness.metallicRoughnessTexture.index = 1;
        material.pbrMetallicRoughness.metallicRoughnessTexture.texCoord = 1;
        material.occlusionTexture.index = 2;
        material.occlusionTexture.strength = 0.5;
        material.emissiveTexture.index = 3;
        material.emissiveFactor = {1.0, 0.5, 0.25};
        material.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object());
        model.materials.push_back(material);
        return model;
    }

    void checkMaterial(const GlbContainerResource* container, const std::string& path)
    {
        check(container != nullptr && !container->meshPayloads().empty() &&
                  container->meshPayloads()[0].material != nullptr,
            path + ": produces a mesh with a material");
        if (!container || container->meshPayloads().empty() || !container->meshPayloads()[0].material) {
            return;
        }
        const Material& m = *container->meshPayloads()[0].material;
        check(m.hasOcclusionTexture() && m.occlusionTexture() != nullptr, path + ": binds the occlusion texture");
        check(m.occlusionStrength() == 0.5f, path + ": keeps the occlusion strength");
        check(m.hasEmissiveTexture() && m.emissiveTexture() != nullptr, path + ": binds the emissive texture");
        check(m.hasMetallicRoughnessTexture() && m.metallicRoughnessUvSet() == 1,
            path + ": reads metallic-roughness from UV set 1");
        const uint64_t key = m.shaderVariantKey();
        check((key & (1ull << 6)) != 0 && (key & (1ull << 7)) != 0,
            path + ": occlusion and emissive reach the shader variant");
        check((key & (1ull << 32)) != 0, path + ": KHR_materials_unlit makes it unlit");
    }
}

int main()
{
    auto device = std::make_shared<StubDevice>();

    std::cout << "createFromModel\n";
    {
        tinygltf::Model model = buildModel();
        const auto container = GlbParser::createFromModel(model, device, "glbMaterialPathsTests");
        checkMaterial(container.get(), "createFromModel");
    }

    std::cout << "\nprepareFromModel + createFromPrepared\n";
    {
        tinygltf::Model model = buildModel();
        auto prepared = GlbParser::prepareFromModel(model, PixelFormat::PIXELFORMAT_RGBA8, "glbMaterialPathsTests");
        const auto container = GlbParser::createFromPrepared(model, std::move(prepared), device,
            "glbMaterialPathsTests");
        checkMaterial(container.get(), "createFromPrepared");
    }

    std::cout << (failures == 0 ? "\nAll GLB material path tests passed\n" : "\nGLB material path tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
