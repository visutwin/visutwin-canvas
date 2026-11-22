// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// C API: models and loaded textures, the scene environment and entity bounds.
#include "capiInternal.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::capi;

extern "C" {

// ---- Models and loaded textures ----------------------------------------------------------

vt_status vt_gltf_load(vt_engine* engine, const char* path, const vt_entity parent, vt_entity* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!path || !out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no path or no output");
        }
        Entity* parentEntity = engine->engine->root();
        if (parent != 0) {
            if (const vt_status status = findEntity(engine, parent, parentEntity); status != VT_OK) {
                return status;
            }
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            return fail(VT_ERROR_NOT_FOUND, std::string("no file ") + path);
        }
        // The asset only builds the model: the entities co-own their meshes and materials, and the
        // materials keep the container's textures, so the asset goes when the call returns.
        Asset asset(std::filesystem::path(path).filename().string(), AssetType::CONTAINER, path);
        auto* container = asset.resourceAs<ContainerResource>();
        if (!container) {
            return fail(VT_ERROR_INVALID_ARGUMENT, std::string("not a glTF model the engine can read: ") + path);
        }
        Entity* model = container->instantiateRenderEntity();
        if (!model) {
            return fail(VT_ERROR_INTERNAL, std::string("the model could not be built: ") + path);
        }
        model->setEngine(engine->engine.get());
        parentEntity->addChild(model);
        *out = model->id().value;
        return VT_OK;
    });
}

vt_status vt_texture_load(vt_engine* engine, const char* path, const uint32_t flags, vt_resource* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!path || !out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no path or no output");
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            return fail(VT_ERROR_NOT_FOUND, std::string("no file ") + path);
        }
        AssetData data;
        data.mipmaps = (flags & VT_TEXTURE_MIPMAPS) != 0;
        if (flags & VT_TEXTURE_ENV_ATLAS) {
            // Its rects are its levels: no mips, decoded as RGBP.
            data.type = TextureType::TEXTURETYPE_RGBP;
            data.mipmaps = false;
        }
        // The asset owns the texture, so the resource keeps the asset.
        auto asset = std::make_shared<Asset>(std::filesystem::path(path).filename().string(), AssetType::TEXTURE,
            path, data);
        auto* texture = asset->resourceAs<Texture>();
        if (!texture) {
            return fail(VT_ERROR_INVALID_ARGUMENT, std::string("not an image the engine can read: ") + path);
        }
        if (flags & VT_TEXTURE_NEAREST) {
            texture->setMagFilter(FilterMode::FILTER_NEAREST);
            texture->setMinFilter(data.mipmaps ? FilterMode::FILTER_NEAREST_MIPMAP_NEAREST : FilterMode::FILTER_NEAREST);
        }
        if (flags & VT_TEXTURE_CLAMP) {
            texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
            texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        }
        *out = addResource(engine, vt_engine::ResourceKind::Texture, nullptr, std::move(asset), texture);
        return VT_OK;
    });
}

vt_status vt_scene_set_environment(vt_engine* engine, const vt_resource id)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (id == 0) {
            engine->engine->scene()->setEnvAtlas(nullptr);
            engine->environment.reset();
            return VT_OK;
        }
        vt_engine::Resource* resource = nullptr;
        if (const vt_status status = findResource(engine, id, vt_engine::ResourceKind::Texture, "texture", resource);
            status != VT_OK) {
            return status;
        }
        engine->engine->scene()->setEnvAtlas(static_cast<Texture*>(resource->target));
        engine->environment = resource->owner;
        return VT_OK;
    });
}

vt_status vt_entity_bounds(vt_engine* engine, const vt_entity id, vt_vec3* center, vt_vec3* halfExtents)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        if (!center || !halfExtents) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        BoundingBox bounds;
        bool any = false;
        for (auto* render : entity->findComponents<RenderComponent>()) {
            if (!render) {
                continue;
            }
            for (MeshInstance* meshInstance : render->meshInstances()) {
                if (!meshInstance) {
                    continue;
                }
                // The first box seeds the union: added to an empty box at the origin, every
                // bounds would reach the origin.
                if (any) {
                    bounds.add(meshInstance->aabb());
                } else {
                    bounds = meshInstance->aabb();
                }
                any = true;
            }
        }
        if (!any) {
            bounds.setCenter(entity->position());
            bounds.setHalfExtents(0.5f, 0.5f, 0.5f);
        }
        const Vector3 c = bounds.center();
        const Vector3 h = bounds.halfExtents();
        *center = vt_vec3{c.getX(), c.getY(), c.getZ()};
        *halfExtents = vt_vec3{h.getX(), h.getY(), h.getZ()};
        return VT_OK;
    });
}

} // extern "C"
