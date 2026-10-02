// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.09.2026
//
// These glTF extensions, checked through BOTH halves of the load pipeline on a model
// built in memory:
//
//  - KHR_materials_sheen / _specular / _iridescence / _anisotropy land on the
//    StandardMaterial, and the two uniforms they feed pack as the shaders expect:
//    the metalness workflow's non-metal F0 and the
//    anisotropy direction (cos, sin). The defaults must pack to EXACTLY 0.04 and
//    (1, 0), the constants they stand in for, or every frame without them moves.
//  - EXT_mesh_gpu_instancing: one TRS matrix per instance, in the NODE's space, so
//    the instanced bounds follow the node.
//  - KHR_materials_variants: names, apply, unmapped primitives untouched, reset.
//  - KHR_gaussian_splatting: splat primitives become a GSplatComponent, with the
//    activated values packed as the PLY loader packs its own.

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
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/gltfModel.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-4f;

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
            check(nearStrict(sheen.r, std::pow(0.5f, 1.0f / 2.2f), kTolerance) &&
                  nearStrict(sheen.b, 1.0f, kTolerance) &&
                  nearStrict(material->sheenRoughness(), 0.3f, kTolerance),
                "sheen: colour stored gamma-encoded, roughness");
            check(material->useMetalnessSpecularColor() && nearStrict(material->specularityFactor(), 0.5f, kTolerance),
                "specular: metalness specular colour on, factor");
            check(nearStrict(material->iridescenceIntensity(), 0.8f, kTolerance) &&
                  nearStrict(material->iridescenceIOR(), 1.4f, kTolerance) &&
                  nearStrict(material->iridescenceThicknessMax(), 500.0f, kTolerance),
                "iridescence: factor, IOR, maximum thickness");
            check(nearStrict(material->anisotropy(), 0.6f, kTolerance) &&
                  nearStrict(material->anisotropyRotation(), 30.0f, kTolerance),
                "anisotropy: strength, rotation in degrees");
            const auto& u = material->packedUniforms();
            // f0(1.5) = 0.04, x colour 0.5 (linear, round-tripped through gamma), x factor 0.5.
            check(nearStrict(u.metalnessSpecular[0], 0.04f * 0.5f * 0.5f, 1e-5f) &&
                  nearStrict(u.metalnessSpecular[1], 0.04f * 0.5f, 1e-5f) &&
                  nearStrict(u.metalnessSpecular[3], 0.5f, kTolerance),
                "the non-metal F0 packs f0(IOR) x specular colour x factor");
            check(nearStrict(u.anisotropyParams[0], std::cos(kPi / 6.0f), kTolerance) &&
                  nearStrict(u.anisotropyParams[1], std::sin(kPi / 6.0f), kTolerance),
                "the anisotropy direction packs (cos, sin) of the rotation");
        }

        // ── EXT_mesh_gpu_instancing ──
        const auto& instancing = mi->instancingData();
        check(instancing.vertexBuffer && instancing.count == 2, "two instances");
        if (instancing.vertexBuffer && instancing.vertexBuffer->storage().size() >= 128) {
            float m[32];
            std::memcpy(m, instancing.vertexBuffer->storage().data(), sizeof(m));
            check(nearStrict(m[0], 1.0f, kTolerance) && nearStrict(m[13], 0.0f, kTolerance) &&
                  nearStrict(m[16], 2.0f, kTolerance) && nearStrict(m[16 + 13], 5.0f, kTolerance),
                "the matrices are the instances' TRS, column-major");
        }
        // Triangle bounds (0..1, 0..1); instances add the scaled copy at y 5..7, so
        // the local union is x 0..2, y 0..7 — carried to world through the node.
        BoundingBox box = mi->aabb();
        check(nearStrict(box.center().getX() - box.halfExtents().getX(), 10.0f, kTolerance) &&
              nearStrict(box.center().getX() + box.halfExtents().getX(), 12.0f, kTolerance) &&
              nearStrict(box.center().getY() + box.halfExtents().getY(), 7.0f, kTolerance),
            "the instanced bounds are the instances' union in the node's space");
        inst->setLocalPosition(20.0f, 0.0f, 0.0f);
        box = mi->aabb();
        check(nearStrict(box.center().getX() - box.halfExtents().getX(), 20.0f, kTolerance),
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
                check(nearStrict(s.covA[0], 4.0f, kTolerance) && nearStrict(s.covB[0], 1.0f, kTolerance) &&
                      nearStrict(s.covB[2], 9.0f, kTolerance),
                    "linear scale and xyzw rotation: covariance diagonal (4, 1, 9)");
                check((s.color >> 24) == 128u && (s.color & 0xFFu) == 128u,
                    "post-sigmoid opacity 0.5 and a zero DC coefficient pack to 128");
                const auto& sh = data->shCoeffs();
                check(sh.size() == 90 && nearStrict(sh[0], 1.0f, kTolerance) &&
                      nearStrict(sh[1], 10.0f, kTolerance) && nearStrict(sh[2], 100.0f, kTolerance) &&
                      nearStrict(sh[3], 2.0f, kTolerance) &&
                      nearStrict(sh[45], -1.0f, kTolerance), "SH coefficient-major, padded to 15 per splat");
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
    const auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true});

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

    const bool loaded = forEachLoadPath([] { return buildModel(); }, device, "glbExtensionsTests",
        [](GlbContainerResource* container, const LoadPath path) {
            const bool created = path == LoadPath::CreateFromModel;
            if (!container) {
                std::cout << (created ? "  FAIL createFromModel\n" : "  FAIL createFromPrepared\n");
                return false;
            }
            checkContainer(*container,
                created ? "\ncreateFromModel" : "\nprepareFromModel + createFromPrepared (the async path)");
            return true;
        });
    if (!loaded) {
        return 1;
    }

    return finish("glTF extensions");
}
