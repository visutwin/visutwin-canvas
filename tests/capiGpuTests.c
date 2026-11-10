/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2025-2026 Arnis Lektauers
 *
 * Created by Arnis Lektauers on 10.11.2026
 *
 * The C API's headless rendering on this build's GPU (label `gpu`: it needs a device and a
 * window system, so the CI test presets leave it out). A hidden window, a camera rendering
 * offscreen with a red clear, and a quad drawn unlit with a 2x2 texture on its emissive map:
 * the corners read back red and the middle the texture's colour, and an update of the texture
 * reaches the next frame. Everything is built through the C API, from C99.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "visutwin/visutwin.h"

static int failures = 0;
static int checks = 0;

static void check(int condition, const char* what)
{
    ++checks;
    if (!condition) {
        ++failures;
    }
    printf("%s %s\n", condition ? "  ok  " : "  FAIL", what);
    if (!condition && vt_last_error()[0]) {
        printf("        last error: %s\n", vt_last_error());
    }
}

static vt_property p(const char* name)
{
    vt_property handle = 0;
    vt_property_find(name, &handle);
    return handle;
}

static void fill(uint8_t* pixels, uint8_t r, uint8_t g, uint8_t b)
{
    int i;
    for (i = 0; i < 4; ++i) {
        pixels[i * 4 + 0] = r;
        pixels[i * 4 + 1] = g;
        pixels[i * 4 + 2] = b;
        pixels[i * 4 + 3] = 255;
    }
}

/* The pixel at (x, y), rows from the top, is within 6 counts of (r, g, b). */
static int near(const uint8_t* image, uint32_t width, uint32_t x, uint32_t y, int r, int g, int b)
{
    const uint8_t* px = image + (size_t)(y * width + x) * 4;
    return abs(px[0] - r) <= 6 && abs(px[1] - g) <= 6 && abs(px[2] - b) <= 6;
}

int main(void)
{
    enum { W = 64, H = 48 };
    vt_engine_desc desc;
    vt_engine* engine = NULL;
    vt_backend backend = 0;
    vt_entity camera = 0, quad = 0;
    vt_resource material = 0, mesh = 0, texture = 0;
    vt_mesh_desc meshDesc;
    vt_texture_desc textureDesc;
    const float positions[] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    const float uvs[] = {0, 1, 1, 1, 1, 0, 0, 0};
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    uint8_t pixels[16];
    static uint8_t image[W * H * 4];
    uint32_t w = 0, h = 0;
    vt_vec3 position = {0.0f, 0.0f, 5.0f};
    vt_color red = {1.0f, 0.0f, 0.0f, 1.0f};
    vt_color black = {0.0f, 0.0f, 0.0f, 1.0f};
    vt_color white = {1.0f, 1.0f, 1.0f, 1.0f};
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.width = 320;
    desc.height = 240;
    desc.flags = VT_ENGINE_HIDDEN;
    if (!(vt_engine_create(&desc, &engine) == VT_OK)) {
        printf("no GPU engine: %s\n", vt_last_error());
        return 1;
    }
    vt_engine_backend(engine, &backend);
    printf("backend %s\n", backend == VT_BACKEND_METAL ? "Metal" : "Vulkan");

    check(vt_entity_create(engine, 0, "Camera", &camera) == VT_OK && vt_component_add(engine, camera, "camera") == VT_OK &&
        vt_set_vec3(engine, camera, p("node.position"), position) == VT_OK &&
        vt_set_color(engine, camera, p("camera.clearColor"), red) == VT_OK &&
        vt_camera_set_offscreen(engine, camera, W, H) == VT_OK, "a camera renders offscreen with a red clear");

    fill(pixels, 0, 0, 255);
    memset(&textureDesc, 0, sizeof(textureDesc));
    textureDesc.size = sizeof(textureDesc);
    textureDesc.width = 2;
    textureDesc.height = 2;
    textureDesc.format = VT_PIXEL_RGBA8;
    textureDesc.flags = VT_TEXTURE_NEAREST | VT_TEXTURE_CLAMP;
    textureDesc.data = pixels;
    textureDesc.data_size = sizeof(pixels);
    memset(&meshDesc, 0, sizeof(meshDesc));
    meshDesc.size = sizeof(meshDesc);
    meshDesc.vertex_count = 4;
    meshDesc.positions = positions;
    meshDesc.uvs = uvs;
    meshDesc.index_count = 6;
    meshDesc.indices = indices;
    check(vt_texture_create(engine, &textureDesc, &texture) == VT_OK && vt_material_create(engine, &material) == VT_OK &&
        vt_set_bool(engine, material, p("standard.unlit"), 1) == VT_OK &&
        vt_set_color(engine, material, p("standard.diffuse"), black) == VT_OK &&
        vt_set_color(engine, material, p("standard.emissive"), white) == VT_OK &&
        vt_material_set_texture(engine, material, "emissiveMap", texture) == VT_OK &&
        vt_mesh_create(engine, &meshDesc, &mesh) == VT_OK, "a blue texture on an unlit material, and a quad");
    check(vt_entity_create(engine, 0, "Quad", &quad) == VT_OK && vt_component_add(engine, quad, "render") == VT_OK &&
        vt_render_set_material(engine, quad, material) == VT_OK && vt_render_set_mesh(engine, quad, mesh) == VT_OK,
        "an entity draws them");
    vt_resource_release(engine, mesh);
    vt_resource_release(engine, material);

    for (i = 0; i < 3; ++i) {
        vt_frame(engine, 1.0f / 60.0f, NULL);
    }
    check(vt_camera_read_pixels(engine, camera, image, sizeof(image), &w, &h) == VT_OK && w == W && h == H,
        "the frame is read back");
    printf("        corner %d %d %d, middle %d %d %d\n", image[0], image[1], image[2],
        image[(H / 2 * W + W / 2) * 4], image[(H / 2 * W + W / 2) * 4 + 1], image[(H / 2 * W + W / 2) * 4 + 2]);
    check(near(image, W, 0, 0, 255, 0, 0) && near(image, W, W - 1, H - 1, 255, 0, 0), "the corners are the red clear");
    check(near(image, W, W / 2, H / 2, 0, 0, 255), "the middle is the texture's blue");

    fill(pixels, 255, 255, 0);
    check(vt_texture_update(engine, texture, pixels, sizeof(pixels)) == VT_OK, "the texture is updated to yellow");
    vt_frame(engine, 1.0f / 60.0f, NULL);
    check(vt_camera_read_pixels(engine, camera, image, sizeof(image), &w, &h) == VT_OK &&
        near(image, W, W / 2, H / 2, 255, 255, 0) && near(image, W, 0, 0, 255, 0, 0),
        "and the next frame shows it");

    {
        /* A glTF model in place of the quad, under a light: the middle shows it. */
        char file[1024];
        vt_entity model = 0, sun = 0;
        vt_vec3 tilt = {45.0f, 30.0f, 0.0f};   /* shining away from the camera, onto the faces it sees */
        const uint8_t* middle = image + (H / 2 * W + W / 2) * 4;
        snprintf(file, sizeof(file), "%s/models/box_textured.glb", VT_TEST_ASSETS);
        check(vt_entity_destroy(engine, quad) == VT_OK && vt_gltf_load(engine, file, 0, &model) == VT_OK &&
            vt_entity_create(engine, 0, "Sun", &sun) == VT_OK && vt_component_add(engine, sun, "light") == VT_OK &&
            vt_set_vec3(engine, sun, p("node.rotation"), tilt) == VT_OK, "a textured glTF box is loaded and lit");
        for (i = 0; i < 3; ++i) {
            vt_frame(engine, 1.0f / 60.0f, NULL);
        }
        check(vt_camera_read_pixels(engine, camera, image, sizeof(image), &w, &h) == VT_OK, "the frame is read back");
        printf("        middle %d %d %d\n", middle[0], middle[1], middle[2]);
        check(!near(image, W, W / 2, H / 2, 255, 0, 0) && (middle[0] + middle[1] + middle[2]) > 30 &&
            near(image, W, 0, 0, 255, 0, 0), "the model covers the middle, lit, with the clear around it");
    }

    check(vt_engine_destroy(engine) == VT_OK, "the engine is destroyed");
    printf("capi-gpu: %d passed, %d failed\n", checks - failures, failures);
    return failures == 0 ? 0 : 1;
}
