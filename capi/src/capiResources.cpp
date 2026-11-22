// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// C API: scene, material, mesh and texture resources, and what an entity draws.
#include "capiInternal.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::capi;

extern "C" {

// ---- Resources ------------------------------------------------------------------------

vt_status vt_engine_scene(vt_engine* engine, vt_resource* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *out = engine->sceneResource;
        return VT_OK;
    });
}

vt_status vt_material_create(vt_engine* engine, vt_resource* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        auto material = std::make_shared<StandardMaterial>();
        void* target = material.get();
        *out = addResource(engine, vt_engine::ResourceKind::Material, "standard", std::move(material), target);
        return VT_OK;
    });
}

vt_status vt_mesh_create(vt_engine* engine, const vt_mesh_desc* desc, vt_resource* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out || !desc || desc->size < sizeof(uint32_t)) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no desc, no output or vt_mesh_desc.size not set");
        }
        vt_mesh_desc d{};
        std::memcpy(&d, desc, std::min<size_t>(desc->size, sizeof(vt_mesh_desc)));
        if (!d.positions || d.vertex_count < 3) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "a mesh needs positions for at least 3 vertices");
        }
        const size_t n = d.vertex_count;
        std::vector<uint32_t> indices;
        if (d.index_count == 0) {
            if (n % 3 != 0) {
                return fail(VT_ERROR_INVALID_ARGUMENT, "without indices the vertex count must be a multiple of 3");
            }
            indices.resize(n);
            for (size_t i = 0; i < n; ++i) {
                indices[i] = static_cast<uint32_t>(i);
            }
        } else {
            if (!d.indices || d.index_count % 3 != 0) {
                return fail(VT_ERROR_INVALID_ARGUMENT, "the index count must be a multiple of 3, with indices given");
            }
            indices.assign(d.indices, d.indices + d.index_count);
            for (const uint32_t index : indices) {
                if (index >= n) {
                    return fail(VT_ERROR_INVALID_ARGUMENT, "index " + std::to_string(index) + " is past the " +
                        std::to_string(n) + " vertices");
                }
            }
        }
        PrimitiveGeometry geometry;
        if (const vt_status status = vertexGeometry(d, indices, geometry); status != VT_OK) {
            return status;
        }
        std::shared_ptr<Mesh> mesh = createMeshFromGeometry(engine->device, geometry);
        if (!mesh) {
            return fail(VT_ERROR_INTERNAL, "the mesh could not be built");
        }
        void* target = mesh.get();
        *out = addResource(engine, vt_engine::ResourceKind::Mesh, nullptr, std::move(mesh), target);
        vt_engine::Resource& record = engine->resources[*out];
        record.indices = std::move(geometry.indices);
        record.vertexCount = d.vertex_count;
        return VT_OK;
    });
}

vt_status vt_mesh_update(vt_engine* engine, const vt_resource id, const vt_mesh_desc* desc)
{
    return guarded([&]() -> vt_status {
        vt_engine::Resource* resource = nullptr;
        if (const vt_status status = findResource(engine, id, vt_engine::ResourceKind::Mesh, "mesh", resource);
            status != VT_OK) {
            return status;
        }
        if (!desc || desc->size < sizeof(uint32_t)) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no desc or vt_mesh_desc.size not set");
        }
        vt_mesh_desc d{};
        std::memcpy(&d, desc, std::min<size_t>(desc->size, sizeof(vt_mesh_desc)));
        if (!d.positions || d.vertex_count != resource->vertexCount) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "an update gives positions for the mesh's " +
                std::to_string(resource->vertexCount) + " vertices");
        }
        PrimitiveGeometry geometry;
        if (const vt_status status = vertexGeometry(d, resource->indices, geometry); status != VT_OK) {
            return status;
        }
        // Only the vertices: the mesh keeps its index buffer, whose triangles did not change.
        GeometryVertexBuffer vertices = createGeometryVertexBuffer(engine->device, geometry);
        if (!vertices.vertexBuffer) {
            return fail(VT_ERROR_INTERNAL, "the mesh's vertices could not be rebuilt");
        }
        // A new vertex buffer into the same mesh, so every instance drawing it follows; the old
        // buffer is freed once the frames still drawing it are done (both backends defer it).
        auto* mesh = static_cast<Mesh*>(resource->target);
        mesh->setVertexBuffer(std::move(vertices.vertexBuffer));
        mesh->setAabb(vertices.aabb);
        return VT_OK;
    });
}

vt_status vt_texture_create(vt_engine* engine, const vt_texture_desc* desc, vt_resource* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out || !desc || desc->size < sizeof(uint32_t)) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no desc, no output or vt_texture_desc.size not set");
        }
        vt_texture_desc d{};
        std::memcpy(&d, desc, std::min<size_t>(desc->size, sizeof(vt_texture_desc)));
        const uint32_t pixelBytes = bytesPerPixel(d.format);
        if (pixelBytes == 0) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "unknown pixel format " + std::to_string(d.format));
        }
        const auto maxSize = static_cast<uint32_t>(engine->device->maxTextureSize());
        if (d.width == 0 || d.height == 0 || d.width > maxSize || d.height > maxSize) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "a texture is 1 to " + std::to_string(maxSize) + " pixels a side");
        }
        const size_t expected = size_t(d.width) * d.height * pixelBytes;
        if (d.data && d.data_size != expected) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "the pixels are " + std::to_string(d.data_size) + " bytes, not " +
                std::to_string(expected));
        }
        TextureOptions options;
        options.width = d.width;
        options.height = d.height;
        options.format = d.format == VT_PIXEL_RGBA8 ? PixelFormat::PIXELFORMAT_RGBA8
            : d.format == VT_PIXEL_RGBA16F ? PixelFormat::PIXELFORMAT_RGBA16F : PixelFormat::PIXELFORMAT_RGBA32F;
        options.mipmaps = (d.flags & VT_TEXTURE_MIPMAPS) != 0;
        const bool nearest = (d.flags & VT_TEXTURE_NEAREST) != 0;
        options.magFilter = nearest ? FilterMode::FILTER_NEAREST : FilterMode::FILTER_LINEAR;
        options.minFilter = options.mipmaps
            ? (nearest ? FilterMode::FILTER_NEAREST_MIPMAP_NEAREST : FilterMode::FILTER_LINEAR_MIPMAP_LINEAR)
            : (nearest ? FilterMode::FILTER_NEAREST : FilterMode::FILTER_LINEAR);
        options.name = "vt_texture";
        auto texture = std::make_shared<Texture>(engine->device.get(), options);
        if (d.flags & VT_TEXTURE_CLAMP) {
            texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
            texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        }
        if (d.data) {
            texture->setLevelData(0, static_cast<const uint8_t*>(d.data), d.data_size);
            texture->upload();
        }
        void* target = texture.get();
        *out = addResource(engine, vt_engine::ResourceKind::Texture, nullptr, std::move(texture), target);
        return VT_OK;
    });
}

vt_status vt_texture_update(vt_engine* engine, const vt_resource id, const void* data, const size_t dataSize)
{
    return guarded([&]() -> vt_status {
        vt_engine::Resource* resource = nullptr;
        if (const vt_status status = findResource(engine, id, vt_engine::ResourceKind::Texture, "texture", resource);
            status != VT_OK) {
            return status;
        }
        auto* texture = static_cast<Texture*>(resource->target);
        const size_t expected = size_t(texture->width()) * texture->height() * pixelFormatBytesPerPixel(texture->format());
        if (!data || dataSize != expected) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "the pixels must be " + std::to_string(expected) + " bytes");
        }
        texture->setLevelData(0, static_cast<const uint8_t*>(data), dataSize);
        texture->upload();
        return VT_OK;
    });
}

vt_status vt_material_set_texture(vt_engine* engine, const vt_resource materialId, const char* map,
    const vt_resource textureId)
{
    return guarded([&]() -> vt_status {
        vt_engine::Resource* materialResource = nullptr;
        if (const vt_status status = findResource(engine, materialId, vt_engine::ResourceKind::Material, "material",
                materialResource);
            status != VT_OK) {
            return status;
        }
        if (!map) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no map name");
        }
        Texture* texture = nullptr;
        if (textureId != 0) {
            vt_engine::Resource* textureResource = nullptr;
            if (const vt_status status = findResource(engine, textureId, vt_engine::ResourceKind::Texture, "texture",
                    textureResource);
                status != VT_OK) {
                return status;
            }
            texture = static_cast<Texture*>(textureResource->target);
            // The material holds its maps by raw pointer: it keeps the texture alive itself.
            static_cast<Material*>(materialResource->target)->retainResource(textureResource->owner);
        }
        auto* material = static_cast<StandardMaterial*>(materialResource->target);
        const std::string_view name(map);
        if (name == "diffuseMap") {
            material->setDiffuseMap(texture);
        } else if (name == "normalMap") {
            material->setNormalMap(texture);
        } else if (name == "emissiveMap") {
            material->setEmissiveMap(texture);
        } else if (name == "metalnessMap") {
            material->setMetalnessMap(texture);
        } else if (name == "glossMap") {
            material->setGlossMap(texture);
        } else if (name == "opacityMap") {
            material->setOpacityMap(texture);
        } else if (name == "aoMap") {
            material->setAoMap(texture);
        } else {
            return fail(VT_ERROR_INVALID_ARGUMENT, std::string("no map ") + map);
        }
        return VT_OK;
    });
}

vt_status vt_resource_release(vt_engine* engine, const vt_resource id)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (id == engine->sceneResource) {
            return fail(VT_ERROR_INVALID_STATE, "the scene is the engine's and cannot be released");
        }
        if (!(id & VT_RESOURCE_BIT) || engine->resources.erase(id) == 0) {
            return fail(VT_ERROR_NOT_FOUND, "no resource " + std::to_string(id & ~VT_RESOURCE_BIT) + " in this engine");
        }
        return VT_OK;
    });
}

// ---- What an entity draws -------------------------------------------------------------

vt_status vt_render_set_mesh(vt_engine* engine, const vt_entity id, const vt_resource meshId)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        auto* render = entity->findComponent<RenderComponent>();
        if (!render) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(id) + " has no render component");
        }
        vt_engine::Resource* resource = nullptr;
        if (const vt_status status = findResource(engine, meshId, vt_engine::ResourceKind::Mesh, "mesh", resource);
            status != VT_OK) {
            return status;
        }
        std::shared_ptr<Material> material;
        if (const auto it = engine->renderMaterials.find(id); it != engine->renderMaterials.end()) {
            material = it->second;
        } else {
            material = getDefaultMaterial(engine->device);
        }
        // "asset": the component builds no primitive of its own and keeps what it is given.
        render->setType("asset");
        render->clearMeshInstances();
        // The instance co-owns the mesh and the material, so releasing either resource is safe.
        render->addMeshInstance(std::make_unique<MeshInstance>(std::static_pointer_cast<Mesh>(resource->owner),
            material, entity));
        return VT_OK;
    });
}

vt_status vt_render_set_material(vt_engine* engine, const vt_entity id, const vt_resource materialId)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        auto* render = entity->findComponent<RenderComponent>();
        if (!render) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(id) + " has no render component");
        }
        std::shared_ptr<Material> material;
        if (materialId != 0) {
            vt_engine::Resource* resource = nullptr;
            if (const vt_status status = findResource(engine, materialId, vt_engine::ResourceKind::Material,
                    "material", resource);
                status != VT_OK) {
                return status;
            }
            material = std::static_pointer_cast<Material>(resource->owner);
            engine->renderMaterials[id] = material;
        } else {
            material = getDefaultMaterial(engine->device);
            engine->renderMaterials.erase(id);
        }
        render->setMaterial(material.get());
        for (MeshInstance* meshInstance : render->meshInstances()) {
            if (meshInstance) {
                meshInstance->setMaterial(material);
            }
        }
        return VT_OK;
    });
}

} // extern "C"
