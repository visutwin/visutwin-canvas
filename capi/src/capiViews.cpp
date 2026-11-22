// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// C API: offscreen cameras and capture, input and picking.
#include "capiInternal.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::capi;

extern "C" {

// ---- Offscreen rendering and capture ----------------------------------------------------

vt_status vt_camera_set_offscreen(vt_engine* engine, const vt_entity id, const uint32_t width, const uint32_t height)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        auto* camera = entity->findComponent<CameraComponent>();
        if (!camera) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(id) + " has no camera");
        }
        if (width == 0 || height == 0) {
            camera->camera()->setRenderTarget(nullptr);
            engine->offscreen.erase(id);
            return VT_OK;
        }
        const auto maxSize = static_cast<uint32_t>(engine->device->maxTextureSize());
        if (width > maxSize || height > maxSize) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "an offscreen image is at most " + std::to_string(maxSize) +
                " pixels a side");
        }
        TextureOptions textureOptions;
        textureOptions.width = width;
        textureOptions.height = height;
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = false;
        textureOptions.name = "vt_offscreen";
        vt_engine::Offscreen offscreen;
        offscreen.texture = std::make_shared<Texture>(engine->device.get(), textureOptions);
        // A depth TEXTURE, not an internal buffer: the camera's scene depth map (what a
        // volume raymarch or soft particles stop at) is copied from it after the opaque
        // layers. An internal buffer is neither stored between the passes nor reachable by
        // that copy, which then took the window's back-buffer depth: another image, of
        // another size. Under MSAA the pass resolves into it.
        TextureOptions depthOptions;
        depthOptions.width = width;
        depthOptions.height = height;
        depthOptions.format = PixelFormat::PIXELFORMAT_DEPTH;
        depthOptions.mipmaps = false;
        depthOptions.name = "vt_offscreen_depth";
        offscreen.depth = std::make_shared<Texture>(engine->device.get(), depthOptions);
        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = engine->device.get();
        targetOptions.name = "vt_offscreen";
        targetOptions.colorBuffer = offscreen.texture.get();
        targetOptions.depthBuffer = offscreen.depth.get();
        // Antialiased as the window's back buffer is.
        targetOptions.samples = std::max(1, engine->device->samples());
        offscreen.target = engine->device->createRenderTarget(targetOptions);
        if (!offscreen.target) {
            return fail(VT_ERROR_UNSUPPORTED, "this backend cannot render offscreen");
        }
        camera->camera()->setRenderTarget(offscreen.target);
        engine->offscreen[id] = std::move(offscreen);
        return VT_OK;
    });
}

vt_status vt_camera_read_pixels(vt_engine* engine, const vt_entity id, uint8_t* buffer, const size_t capacity,
    uint32_t* width, uint32_t* height)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        if (!entity->findComponent<CameraComponent>()) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(id) + " has no camera");
        }
        const auto it = engine->offscreen.find(id);
        if (it == engine->offscreen.end()) {
            return fail(VT_ERROR_INVALID_STATE, "the camera does not render offscreen (vt_camera_set_offscreen)");
        }
        const Texture& texture = *it->second.texture;
        if (width) {
            *width = texture.width();
        }
        if (height) {
            *height = texture.height();
        }
        const size_t bytes = size_t(texture.width()) * texture.height() * 4;
        if (!buffer) {
            return VT_OK;
        }
        if (capacity < bytes) {
            return fail(VT_ERROR_BUFFER_TOO_SMALL, "the pixels need " + std::to_string(bytes) + " bytes");
        }
        if (engine->backend == VT_BACKEND_NULL) {
            return fail(VT_ERROR_UNSUPPORTED, "the null backend has no pixels to read");
        }
        std::vector<uint8_t> pixels;
        if (!it->second.texture->read(pixels) || pixels.size() < bytes) {
            return fail(VT_ERROR_INTERNAL, "the offscreen image could not be read back");
        }
        std::memcpy(buffer, pixels.data(), bytes);
        return VT_OK;
    });
}

// ---- Input and picking ----------------------------------------------------------------

vt_status vt_input_get(vt_engine* engine, vt_input_state* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        return fillSized(out, engine->input);
    });
}

vt_status vt_key_get(vt_engine* engine, const int32_t scancode, int32_t* down, int32_t* pressed)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no scancode " + std::to_string(scancode));
        }
        if (down) {
            *down = engine->keysDown[static_cast<size_t>(scancode)];
        }
        if (pressed) {
            *pressed = engine->keysPressed[static_cast<size_t>(scancode)];
        }
        return VT_OK;
    });
}

vt_status vt_pick(vt_engine* engine, const vt_entity cameraId, const float x, const float y, int32_t* hit,
    vt_entity* out, vt_vec3* point)
{
    return guarded([&]() -> vt_status {
        Entity* cameraEntity = nullptr;
        if (const vt_status status = findEntity(engine, cameraId, cameraEntity); status != VT_OK) {
            return status;
        }
        auto* camera = cameraEntity->findComponent<CameraComponent>();
        if (!camera) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(cameraId) + " has no camera");
        }
        if (!hit || !out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *hit = 0;
        *out = 0;
        // The id buffer at the window's size in points, so a point of the window is its pixel.
        const auto [width, height] = engine->engine->canvasSize();
        const int w = std::max(1, width);
        const int h = std::max(1, height);
        if (!engine->picker) {
            engine->picker = std::make_unique<Picker>(engine->engine.get(), w, h, true);
        } else if (engine->picker->width() != w || engine->picker->height() != h) {
            engine->picker->resize(w, h);
        }
        engine->picker->prepare(camera, engine->engine->scene().get());
        const int px = static_cast<int>(std::floor(x));
        const int py = static_cast<int>(std::floor(y));
        if (px < 0 || py < 0 || px >= w || py >= h) {
            return VT_OK;
        }
        MeshInstance* meshInstance = engine->picker->getSelectionSingle(px, py);
        if (!meshInstance) {
            return VT_OK;
        }
        // The entity whose render component drew it: the instance's node or the nearest entity above.
        for (GraphNode* node = meshInstance->node(); node; node = node->parent()) {
            if (auto* entity = dynamic_cast<Entity*>(node)) {
                *out = entity->id().value;
                *hit = 1;
                break;
            }
        }
        if (point && *hit) {
            if (const auto world = engine->picker->getWorldPoint(px, py)) {
                *point = vt_vec3{world->getX(), world->getY(), world->getZ()};
            } else {
                *point = vt_vec3{0.0f, 0.0f, 0.0f};
            }
        }
        return VT_OK;
    });
}

} // extern "C"
