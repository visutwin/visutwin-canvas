// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The glTF extensions added on 2026-09-25, checked through BOTH halves of the load
// pipeline on a model built in memory:
//
//  - KHR_materials_sheen / _specular / _iridescence / _anisotropy land on the
//    StandardMaterial, and the two uniforms they feed pack as the shaders expect:
//    the metalness workflow's non-metal F0 (upstream getSpecularModulate) and the
//    anisotropy direction (cos, sin). The defaults must pack to EXACTLY 0.04 and
//    (1, 0), the constants the shaders used before, or every existing frame moves.
//  - EXT_mesh_gpu_instancing: one TRS matrix per instance, in the NODE's space, so
//    the instanced bounds follow the node.
//  - KHR_materials_variants: names, apply, unmapped primitives untouched, reset.
//  - KHR_gaussian_splatting: splat primitives become a GSplatComponent, with the
//    activated values packed as the PLY loader packs its own.
//
// Before these existed the parser read none of the four extensions.

#include <tiny_gltf.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/entity.h"
#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"

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

    bool near(const float a, const float b, const float eps = 1e-4f) { return std::fabs(a - b) < eps; }

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
        model.accessors.push_back(accessor);
        return static_cast<int>(model.accessors.size()) - 1;
    }

    using Value = tinygltf::Value;

    Value object(std::initializer_list<std::pair<const std::string, Value>> members)
    {
        return Value(Value::Object(members));
    }

    Value array(std::initializer_list<Value> items) { return Value(Value::Array(items)); }

    Value number(const double v) { return Value(v); }

    constexpr float kPi = 3.14159265358979f;

    tinygltf::Model buildModel()
    {
        tinygltf::Model model;
        model.asset.version = "2.0";
        tinygltf::Buffer buffer;

        // Materials: 0 carries the four material extensions, 1 is what variant "red"
        // swaps in, 2 is plain.
        tinygltf::Material base;
        base.name = "Base";
        base.extensions["KHR_materials_sheen"] = object({
            {"sheenColorFactor", array({number(0.5), number(0.25), number(1.0)})},
            {"sheenRoughnessFactor", number(0.3)}});
        base.extensions["KHR_materials_specular"] = object({
            {"specularColorFactor", array({number(0.5), number(1.0), number(1.0)})},
            {"specularFactor", number(0.5)}});
        base.extensions["KHR_materials_iridescence"] = object({
            {"iridescenceFactor", number(0.8)},
            {"iridescenceIor", number(1.4)},
            {"iridescenceThicknessMaximum", number(500.0)}});
        base.extensions["KHR_materials_anisotropy"] = object({
            {"anisotropyStrength", number(0.6)},
            {"anisotropyRotation", number(kPi / 6.0)}});
        tinygltf::Material red;
        red.name = "VariantRed";
        tinygltf::Material plain;
        plain.name = "Plain";
        model.materials = {base, red, plain};
        model.extensions["KHR_materials_variants"] = object({
            {"variants", array({object({{"name", Value(std::string("red"))}}),
                                object({{"name", Value(std::string("blue"))}})})}});

        // Mesh 0: one triangle, material 0, variant 0 ("red") -> material 1.
        tinygltf::Primitive triangle;
        triangle.mode = TINYGLTF_MODE_TRIANGLES;
        triangle.material = 0;
        triangle.attributes["POSITION"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC3);
        triangle.extensions["KHR_materials_variants"] = object({
            {"mappings", array({object({{"material", Value(1)}, {"variants", array({Value(0)})}})})}});
        tinygltf::Mesh triangleMesh;
        triangleMesh.primitives.push_back(triangle);
        model.meshes.push_back(triangleMesh);

        // Mesh 1: two gaussian splats with one band of SH. Splat 0 is turned 90
        // degrees about +Z with scale (1, 2, 3), so its covariance diagonal is
        // (4, 1, 9); both are half opaque with a zero DC coefficient (grey 0.5).
        const float h = std::sqrt(0.5f);
        tinygltf::Primitive splat;
        splat.mode = TINYGLTF_MODE_POINTS;
        splat.attributes["POSITION"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  3.0f, 0.0f, 0.0f}, 2, TINYGLTF_TYPE_VEC3);
        splat.attributes["KHR_gaussian_splatting:ROTATION"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, h, h,  0.0f, 0.0f, 0.0f, 1.0f}, 2, TINYGLTF_TYPE_VEC4);
        splat.attributes["KHR_gaussian_splatting:SCALE"] = addAccessor(model, buffer.data,
            {1.0f, 2.0f, 3.0f,  1.0f, 1.0f, 1.0f}, 2, TINYGLTF_TYPE_VEC3);
        splat.attributes["KHR_gaussian_splatting:OPACITY"] = addAccessor(model, buffer.data,
            {0.5f, 0.5f}, 2, TINYGLTF_TYPE_SCALAR);
        splat.attributes["KHR_gaussian_splatting:SH_DEGREE_0_COEF_0"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f}, 2, TINYGLTF_TYPE_VEC3);
        for (int c = 0; c < 3; ++c) {
            const float v = static_cast<float>(c + 1);
            splat.attributes["KHR_gaussian_splatting:SH_DEGREE_1_COEF_" + std::to_string(c)] = addAccessor(model,
                buffer.data, {v, v * 10.0f, v * 100.0f,  -v, -v, -v}, 2, TINYGLTF_TYPE_VEC3);
        }
        splat.extensions["KHR_gaussian_splatting"] = object({{"kernel", Value(std::string("ellipse"))}});
        tinygltf::Mesh splatMesh;
        splatMesh.primitives.push_back(splat);
        model.meshes.push_back(splatMesh);

        // Node 0: the triangle at x = 10, instanced twice in its own space: at the
        // origin, and 5 up at scale 2.
        tinygltf::Node inst;
        inst.name = "Inst";
        inst.mesh = 0;
        inst.translation = {10.0, 0.0, 0.0};
        const int translations = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  0.0f, 5.0f, 0.0f}, 2, TINYGLTF_TYPE_VEC3);
        const int scales = addAccessor(model, buffer.data,
            {1.0f, 1.0f, 1.0f,  2.0f, 2.0f, 2.0f}, 2, TINYGLTF_TYPE_VEC3);
        inst.extensions["EXT_mesh_gpu_instancing"] = object({
            {"attributes", object({{"TRANSLATION", Value(translations)}, {"SCALE", Value(scales)}})}});
        tinygltf::Node splats;
        splats.name = "Splats";
        splats.mesh = 1;
        model.nodes = {inst, splats};
        model.buffers.push_back(buffer);

        tinygltf::Scene scene;
        scene.nodes = {0, 1};
        model.scenes.push_back(scene);
        model.defaultScene = 0;
        return model;
    }

    MeshInstance* firstMeshInstance(Entity* entity)
    {
        auto* render = entity ? entity->findComponent<RenderComponent>() : nullptr;
        return render && !render->meshInstances().empty() ? render->meshInstances()[0] : nullptr;
    }

    void checkContainer(GlbContainerResource& container, const std::string& label)
    {
        std::cout << label << '\n';
        std::unique_ptr<Entity> root(container.instantiateRenderEntity());
        Entity* inst = root ? dynamic_cast<Entity*>(root->findByName("Inst")) : nullptr;
        MeshInstance* mi = firstMeshInstance(inst);
        check(mi != nullptr, "the instanced node has its mesh instance");
        if (!mi) {
            return;
        }

        // ── Material extensions ──
        auto* material = dynamic_cast<StandardMaterial*>(mi->material());
        check(material != nullptr, "a StandardMaterial");
        if (material) {
            const Color& sheen = material->sheenColor();
            check(near(sheen.r, std::pow(0.5f, 1.0f / 2.2f)) && near(sheen.b, 1.0f) &&
                  near(material->sheenRoughness(), 0.3f), "sheen: colour stored gamma-encoded, roughness");
            check(material->useMetalnessSpecularColor() && near(material->specularityFactor(), 0.5f),
                "specular: metalness specular colour on, factor");
            check(near(material->iridescenceIntensity(), 0.8f) && near(material->iridescenceIOR(), 1.4f) &&
                  near(material->iridescenceThicknessMax(), 500.0f), "iridescence: factor, IOR, maximum thickness");
            check(near(material->anisotropy(), 0.6f) && near(material->anisotropyRotation(), 30.0f),
                "anisotropy: strength, rotation in degrees");
            const auto& u = material->packedUniforms();
            // f0(1.5) = 0.04, x colour 0.5 (linear, round-tripped through gamma), x factor 0.5.
            check(near(u.metalnessSpecular[0], 0.04f * 0.5f * 0.5f, 1e-5f) &&
                  near(u.metalnessSpecular[1], 0.04f * 0.5f, 1e-5f) && near(u.metalnessSpecular[3], 0.5f),
                "the non-metal F0 packs f0(IOR) x specular colour x factor");
            check(near(u.anisotropyParams[0], std::cos(kPi / 6.0f)) && near(u.anisotropyParams[1], std::sin(kPi / 6.0f)),
                "the anisotropy direction packs (cos, sin) of the rotation");
        }

        // ── EXT_mesh_gpu_instancing ──
        const auto& instancing = mi->instancingData();
        check(instancing.vertexBuffer && instancing.count == 2, "two instances");
        if (instancing.vertexBuffer && instancing.vertexBuffer->storage().size() >= 128) {
            float m[32];
            std::memcpy(m, instancing.vertexBuffer->storage().data(), sizeof(m));
            check(near(m[0], 1.0f) && near(m[13], 0.0f) && near(m[16], 2.0f) && near(m[16 + 13], 5.0f),
                "the matrices are the instances' TRS, column-major");
        }
        // Triangle bounds (0..1, 0..1); instances add the scaled copy at y 5..7, so
        // the local union is x 0..2, y 0..7 — carried to world through the node.
        BoundingBox box = mi->aabb();
        check(near(box.center().getX() - box.halfExtents().getX(), 10.0f) &&
              near(box.center().getX() + box.halfExtents().getX(), 12.0f) &&
              near(box.center().getY() + box.halfExtents().getY(), 7.0f),
            "the instanced bounds are the instances' union in the node's space");
        inst->setLocalPosition(20.0f, 0.0f, 0.0f);
        box = mi->aabb();
        check(near(box.center().getX() - box.halfExtents().getX(), 20.0f),
            "and follow the node when it moves (instances live in its space)");

        // ── KHR_materials_variants ──
        const auto& variants = container.getMaterialVariants();
        check(variants.size() == 2 && variants[0] == "red" && variants[1] == "blue", "variant names in file order");
        check(container.applyMaterialVariant(root.get(), "red") && mi->material() &&
              mi->material()->name() == "VariantRed" && mi->materialShared(),
            "'red' swaps the mapped material in, co-owned");
        check(container.applyMaterialVariant(root.get(), "blue") && mi->material()->name() == "VariantRed",
            "'blue' does not map this primitive, which keeps its material");
        check(container.applyMaterialVariant(root.get(), "") && mi->material()->name() == "Base",
            "an empty name puts the primitive's own material back");
        check(!container.applyMaterialVariant(root.get(), "green"), "an unknown name is refused");

        // ── KHR_gaussian_splatting ──
        Entity* splatNode = dynamic_cast<Entity*>(root->findByName("Splats"));
        auto* gsplat = splatNode ? splatNode->findComponent<GSplatComponent>() : nullptr;
        check(gsplat && gsplat->resource(), "the splat node has a gsplat component with a resource");
        if (gsplat && gsplat->resource()) {
            const auto* data = &gsplat->resource()->data();
            check(data && data->numSplats() == 2 && data->shBands() == 1, "two splats, one SH band");
            if (data && data->numSplats() == 2) {
                const auto& s = data->splats()[0];
                check(near(s.covA[0], 4.0f) && near(s.covB[0], 1.0f) && near(s.covB[2], 9.0f),
                    "linear scale and xyzw rotation: covariance diagonal (4, 1, 9)");
                check((s.color >> 24) == 128u && (s.color & 0xFFu) == 128u,
                    "post-sigmoid opacity 0.5 and a zero DC coefficient pack to 128");
                const auto& sh = data->shCoeffs();
                check(sh.size() == 90 && near(sh[0], 1.0f) && near(sh[1], 10.0f) && near(sh[2], 100.0f) &&
                      near(sh[3], 2.0f) && near(sh[45], -1.0f), "SH coefficient-major, padded to 15 per splat");
            }
        }
        check(firstMeshInstance(splatNode) == nullptr ||
              firstMeshInstance(splatNode)->gsplatInstance() != nullptr,
            "a splat primitive is not drawn as a point cloud");
    }
}

int main()
{
    std::cout << std::unitbuf;
    const auto device = std::make_shared<StubDevice>();

    {
        StandardMaterial defaults;
        const auto& u = defaults.packedUniforms();
        check(u.metalnessSpecular[0] == 0.04f && u.metalnessSpecular[3] == 1.0f,
            "a default material's non-metal F0 is EXACTLY 0.04, the constant it replaced");
        check(u.anisotropyParams[0] == 1.0f && u.anisotropyParams[1] == 0.0f, "and its anisotropy direction (1, 0)");
        StandardMaterial legacy;
        legacy.setAnisotropy(-0.5f);
        check(legacy.packedUniforms().anisotropy == 0.5f && legacy.packedUniforms().anisotropyParams[0] == 0.0f &&
              legacy.packedUniforms().anisotropyParams[1] == 1.0f,
            "a negative strength is rotation 90 (the bitangent), exactly, as the old sign test picked");
    }

    {
        tinygltf::Model model = buildModel();
        auto container = GlbParser::createFromModel(model, device, "glbExtensionsTests");
        if (!container) {
            std::cout << "  FAIL createFromModel\n";
            return 1;
        }
        checkContainer(*container, "\ncreateFromModel");
    }
    {
        tinygltf::Model model = buildModel();
        auto prepared = GlbParser::prepareFromModel(model, PixelFormat::PIXELFORMAT_RGBA8, "glbExtensionsTests");
        auto container = GlbParser::createFromPrepared(model, std::move(prepared), device, "glbExtensionsTests");
        if (!container) {
            std::cout << "  FAIL createFromPrepared\n";
            return 1;
        }
        checkContainer(*container, "\nprepareFromModel + createFromPrepared (the async path)");
    }

    std::cout << (failures == 0 ? "\nAll glTF extension tests passed\n" : "\nglTF extension tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
