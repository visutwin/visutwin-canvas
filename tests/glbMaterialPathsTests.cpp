// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.09.2026
//
// A GLB loads through three paths — the synchronous parse(), createFromModel and
// prepareFromModel + createFromPrepared (the last two are what loadAsync uses) — and
// all three call one createGltfMaterial. An async path with its own copy that drifts
// (no occlusion texture, no emissive texture, no metallic-roughness UV set, no
// KHR_materials_unlit) loses a model's baked AO and its glow and lights an unlit model,
// while the synchronous load stays right. No example loads asynchronously, so only this
// shows it.
//
// This builds a model in memory that uses every one of those features and checks the
// material each ASYNC path produces; the synchronous path shares the function, and
// loads from disk in every example.

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/parsers/glbContainerResource.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "support/check.h"
#include "support/gltfModel.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    const Vec3Bounds kVec3Bounds{{0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}};

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
            {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC3, kVec3Bounds);
        primitive.attributes["NORMAL"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f}, 3, TINYGLTF_TYPE_VEC3, kVec3Bounds);
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
    // Mesh payloads are only created when the device hands back a vertex buffer, so
    // these keep their bytes on the CPU; textures get no GPU object, which upload()
    // tolerates.
    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true});

    forEachLoadPath([] { return buildModel(); }, device, "glbMaterialPathsTests",
        [&](const GlbContainerResource* container, const LoadPath path) {
            if (path == LoadPath::CreateFromModel) {
                std::cout << "createFromModel\n";
                checkMaterial(container, "createFromModel");
            } else {
                std::cout << "\nprepareFromModel + createFromPrepared\n";
                checkMaterial(container, "createFromPrepared");
            }
        });

    // A material holds raw Texture*s into its container. A mesh instance co-owns its
    // material, so an entity built from an asset survives the asset's unload(); unless
    // every material the container hands out keeps its texture list alive, those
    // materials point at freed textures. The device's texture VRAM figure is
    // the observable: a texture gives its bytes back in its destructor.
    std::cout << "\na material outlives its container\n";
    {
        tinygltf::Model model = buildModel();
        auto container = GlbParser::createFromModel(model, device, "glbMaterialPathsTests");
        const auto before = device->vram().tex;
        std::shared_ptr<Material> material =
            container && !container->meshPayloads().empty() ? container->meshPayloads()[0].material : nullptr;
        check(material != nullptr && before > 0, "the parsed textures are counted in VRAM");
        container.reset();   // what Asset::unload() does
        check(device->vram().tex == before,
            "unloading the container frees none of the textures a live material uses");
        check(material && material->occlusionTexture() && material->occlusionTexture()->width() == 2,
            "and they are still readable through the material");
        material.reset();
        check(device->vram().tex == 0, "releasing the last material frees them");
    }

    return finish("GLB material paths");
}
