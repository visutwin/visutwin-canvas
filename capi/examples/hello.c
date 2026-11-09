/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2025-2026 Arnis Lektauers
 *
 * Created by Arnis Lektauers on 09.11.2026
 *
 * The C API from a C program: a window, a grey ground, a box turning on it, a directional
 * light casting its shadow and a camera, all built through property names, then the caller's
 * own loop until the window closes (or for the number of frames given as the first argument).
 * VISUTWIN_BACKEND=metal|vulkan picks the backend, VISUTWIN_SCREENSHOT=<png> captures a frame.
 */
#include <stdio.h>
#include <stdlib.h>

#include "visutwin/visutwin.h"

#define CHECK(call)                                                                               \
    do {                                                                                          \
        const vt_status status_ = (call);                                                         \
        if (status_ != VT_OK) {                                                                   \
            fprintf(stderr, "%s failed: %s (%s)\n", #call, vt_status_name(status_), vt_last_error()); \
            return 1;                                                                             \
        }                                                                                         \
    } while (0)

static vt_property p(const char* name)
{
    vt_property handle = 0;
    if (vt_property_find(name, &handle) != VT_OK) {
        fprintf(stderr, "no property %s\n", name);
    }
    return handle;
}

static vt_vec3 v3(float x, float y, float z)
{
    vt_vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

int main(int argc, char** argv)
{
    const long maxFrames = argc > 1 ? strtol(argv[1], NULL, 10) : 0;
    vt_engine_desc desc = {0};
    vt_engine* engine = NULL;
    vt_entity ground, box, light, camera;
    vt_color clear = {0.55f, 0.65f, 0.8f, 1.0f};
    int32_t quit = 0;
    long frame = 0;
    float angle = 0.0f;

    desc.size = sizeof(desc);
    desc.title = "VisuTwin from C";
    desc.width = 900;
    desc.height = 700;
    CHECK(vt_engine_create(&desc, &engine));

    CHECK(vt_entity_create(engine, 0, "Ground", &ground));
    CHECK(vt_component_add(engine, ground, "render"));
    CHECK(vt_set_string(engine, ground, p("render.type"), "plane"));
    CHECK(vt_set_vec3(engine, ground, p("node.scale"), v3(12.0f, 1.0f, 12.0f)));
    CHECK(vt_set_bool(engine, ground, p("render.castShadows"), 0));

    CHECK(vt_entity_create(engine, 0, "Box", &box));
    CHECK(vt_component_add(engine, box, "render"));
    CHECK(vt_set_string(engine, box, p("render.type"), "box"));
    CHECK(vt_set_vec3(engine, box, p("node.position"), v3(0.0f, 0.75f, 0.0f)));
    CHECK(vt_set_vec3(engine, box, p("node.scale"), v3(1.5f, 1.5f, 1.5f)));

    CHECK(vt_entity_create(engine, 0, "Sun", &light));
    CHECK(vt_component_add(engine, light, "light"));
    CHECK(vt_set_text(engine, light, p("light.type"), "directional"));
    CHECK(vt_set_vec3(engine, light, p("node.rotation"), v3(-50.0f, 30.0f, 0.0f)));
    CHECK(vt_set_float(engine, light, p("light.intensity"), 1.2f));
    CHECK(vt_set_bool(engine, light, p("light.castShadows"), 1));
    CHECK(vt_set_float(engine, light, p("light.shadowDistance"), 20.0f));

    CHECK(vt_entity_create(engine, 0, "Camera", &camera));
    CHECK(vt_component_add(engine, camera, "camera"));
    CHECK(vt_set_vec3(engine, camera, p("node.position"), v3(0.0f, 3.5f, 7.0f)));
    CHECK(vt_set_vec3(engine, camera, p("node.rotation"), v3(-25.0f, 0.0f, 0.0f)));
    CHECK(vt_set_color(engine, camera, p("camera.clearColor"), clear));

    while (!quit && (maxFrames <= 0 || frame < maxFrames)) {
        angle += 0.5f;
        CHECK(vt_set_vec3(engine, box, p("node.rotation"), v3(0.0f, angle, 0.0f)));
        CHECK(vt_frame(engine, 1.0f / 60.0f, &quit));
        ++frame;
    }
    CHECK(vt_engine_destroy(engine));
    return 0;
}
