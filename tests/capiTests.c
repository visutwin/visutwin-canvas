/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2025-2026 Arnis Lektauers
 *
 * Created by Arnis Lektauers on 09.11.2026
 *
 * The C API, driven from C99 on the null backend (no window, no GPU): the header must stay C,
 * and every call a frontend makes must answer as documented. Lifecycle and the loop, entities
 * and their hierarchy, components by type name, the schema (including a caller whose structs
 * are OLDER than the library's), typed and text property access with every error status, the
 * two-call buffers, a destroyed entity's id, two engines, and the thread check.
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "visutwin/visutwin.h"

static int checks = 0;
static int failures = 0;

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

static vt_property property(const char* name)
{
    vt_property handle = 0;
    if (vt_property_find(name, &handle) != VT_OK) {
        printf("  FAIL no property %s\n", name);
        ++failures;
    }
    return handle;
}

/* An enum property's value for an option name, -1 when none has it. */
static int optionValue(vt_property p, const char* name)
{
    vt_property_info info;
    uint32_t i;
    info.size = sizeof(info);
    if (vt_property_get_info(p, &info) != VT_OK) {
        return -1;
    }
    for (i = 0; i < info.option_count; ++i) {
        int32_t value = 0;
        const char* optionName = NULL;
        if (vt_property_option(p, i, &value, &optionName) == VT_OK && strcmp(optionName, name) == 0) {
            return value;
        }
    }
    return -1;
}

typedef struct ThreadProbe
{
    vt_engine* engine;
    vt_status engineCall;
    vt_status schemaCall;
} ThreadProbe;

static void* probeThread(void* argument)
{
    ThreadProbe* probe = (ThreadProbe*)argument;
    vt_entity root = 0;
    uint32_t count = 0;
    probe->engineCall = vt_entity_root(probe->engine, &root);
    probe->schemaCall = vt_type_count(&count);
    return NULL;
}

int main(void)
{
    vt_engine_desc desc;
    vt_engine* engine = NULL;
    vt_engine* other = NULL;
    vt_backend backend = VT_BACKEND_DEFAULT;
    vt_entity root = 0, car = 0, wheel = 0, lamp = 0, box = 0, cam = 0, found = 0, parent = 0;
    vt_entity children[4];
    size_t count = 0, length = 0;
    char text[256];
    char tiny[4];
    int32_t flag = 0, number = 0;
    float value = 0.0f;
    vt_vec3 position;
    vt_color color;
    vt_entity ref = 0;
    uint32_t typeCount = 0, t;
    int schemaOk = 1;
    vt_frame_stats stats;
    ThreadProbe probe;
    pthread_t thread;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- Version, creation ---- */
    check(vt_api_version() == ((VT_API_VERSION_MAJOR << 16) | VT_API_VERSION_MINOR), "the library reports the header's version");
    memset(&desc, 0, sizeof(desc));
    desc.backend = VT_BACKEND_NULL;
    check(vt_engine_create(&desc, &engine) == VT_ERROR_INVALID_ARGUMENT && engine == NULL,
        "a desc without its size is refused");
    check(vt_last_error()[0] != '\0', "a failure leaves a message");
    desc.size = sizeof(desc);
    desc.width = 320;
    desc.height = 200;
    check(vt_engine_create(&desc, NULL) == VT_ERROR_INVALID_ARGUMENT, "creation needs an output");
    check(vt_engine_create(&desc, &engine) == VT_OK && engine != NULL, "a null-backend engine is created");
    check(vt_last_error()[0] == '\0', "a success clears the message");
    check(vt_engine_backend(engine, &backend) == VT_OK && backend == VT_BACKEND_NULL, "it reports its backend");
    check(strcmp(vt_status_name(VT_ERROR_NOT_FOUND), "VT_ERROR_NOT_FOUND") == 0, "statuses have names");

    /* ---- Entities and their hierarchy ---- */
    check(vt_entity_root(engine, &root) == VT_OK && root != 0, "the root has an id");
    check(vt_entity_create(engine, 0, "Car", &car) == VT_OK && car != 0, "an entity is created under the root");
    check(vt_entity_create(engine, car, "Wheel", &wheel) == VT_OK && wheel > car, "and a child under it, with a later id");
    check(vt_entity_find(engine, "Car/Wheel", &found) == VT_OK && found == wheel, "a path finds the child");
    check(vt_entity_find(engine, "Car/Door", &found) == VT_ERROR_NOT_FOUND, "a path naming nothing is not found");
    check(vt_entity_path(engine, wheel, NULL, 0, &length) == VT_OK && length == 9, "a NULL buffer asks the length");
    check(vt_entity_path(engine, wheel, text, sizeof(text), &length) == VT_OK && strcmp(text, "Car/Wheel") == 0,
        "the path is written");
    check(vt_entity_path(engine, wheel, tiny, sizeof(tiny), &length) == VT_ERROR_BUFFER_TOO_SMALL &&
        strcmp(tiny, "Car") == 0 && length == 9, "a short buffer is filled, terminated and reported");
    check(vt_entity_parent(engine, wheel, &parent) == VT_OK && parent == car, "the parent is reported");
    check(vt_entity_parent(engine, root, &parent) == VT_OK && parent == 0, "the root has none");
    check(vt_entity_create(engine, 0, "Lamp", &lamp) == VT_OK, "a second entity under the root");
    check(vt_entity_children(engine, root, NULL, 0, &count) == VT_OK && count == 2, "the root has two children");
    check(vt_entity_children(engine, root, children, 1, &count) == VT_ERROR_BUFFER_TOO_SMALL && count == 2 &&
        children[0] == car, "a short buffer takes the first child and says so");
    check(vt_entity_set_parent(engine, car, wheel) == VT_ERROR_INVALID_ARGUMENT, "an entity cannot move below itself");
    check(vt_entity_set_parent(engine, wheel, lamp) == VT_OK && vt_entity_find(engine, "Lamp/Wheel", &found) == VT_OK &&
        found == wheel, "a child moves to another parent");
    check(vt_entity_set_parent(engine, wheel, car) == VT_OK, "and back");

    /* ---- Components ---- */
    check(vt_component_add(engine, lamp, "light") == VT_OK, "a light is added by name");
    check(vt_component_has(engine, lamp, "light", &flag) == VT_OK && flag == 1, "and is there");
    check(vt_component_add(engine, lamp, "light") == VT_OK, "adding it again keeps the one there");
    check(vt_component_add(engine, lamp, "torch") == VT_ERROR_NO_TYPE, "an unknown type is refused");
    check(vt_component_add(engine, lamp, "node") == VT_ERROR_INVALID_ARGUMENT, "a type that is no component is refused");
    check(vt_component_has(engine, car, "light", &flag) == VT_OK && flag == 0, "another entity has none");
    check(vt_component_remove(engine, car, "light") == VT_ERROR_NO_TARGET, "removing what is not there says so");

    /* ---- Schema ---- */
    check(vt_type_count(&typeCount) == VT_OK && typeCount >= 7, "the schema lists the built-in types");
    for (t = 0; t < typeCount; ++t) {
        vt_type type = 0;
        vt_type_info info;
        uint32_t i;
        info.size = sizeof(info);
        if (vt_type_at(t, &type) != VT_OK || vt_type_get_info(type, &info) != VT_OK || !info.name || !info.description) {
            schemaOk = 0;
            continue;
        }
        for (i = 0; i < info.property_count; ++i) {
            vt_property p = 0;
            vt_property_info pinfo;
            vt_property again = 0;
            char qualified[128];
            pinfo.size = sizeof(pinfo);
            if (vt_property_at(type, i, &p) != VT_OK || vt_property_get_info(p, &pinfo) != VT_OK) {
                schemaOk = 0;
                continue;
            }
            snprintf(qualified, sizeof(qualified), "%s.%s", info.name, pinfo.name);
            if (pinfo.type != type || pinfo.value_type == VT_VALUE_NONE || !pinfo.unit || !pinfo.description ||
                (pinfo.value_type == VT_VALUE_ENUM) != (pinfo.option_count > 0) ||
                vt_property_find(qualified, &again) != VT_OK || again != p) {
                printf("        schema entry %s is inconsistent\n", qualified);
                schemaOk = 0;
            }
        }
    }
    check(schemaOk, "every type and property describes itself and finds itself by name");
    {
        vt_type light = 0;
        vt_type_info info;
        /* A caller built against a header whose vt_type_info ended before property_count. */
        memset(&info, 0xAB, sizeof(info));
        info.size = (uint32_t)offsetof(vt_type_info, property_count);
        check(vt_type_find("light", &light) == VT_OK && vt_type_get_info(light, &info) == VT_OK &&
            strcmp(info.name, "light") == 0 && info.property_count == 0xABABABABu && info.flags == 0xABABABABu,
            "an older caller's struct is filled only as far as its size");
        info.size = sizeof(info);
        check(vt_type_get_info(light, &info) == VT_OK && (info.flags & VT_TYPE_COMPONENT) && (info.flags & VT_TYPE_ON_ENTITY),
            "the light is a component found on an entity");
        check(vt_type_find("standard", &light) == VT_OK && vt_type_get_info(light, &info) == VT_OK && info.flags == 0,
            "the standard material is neither");
    }
    check(vt_property_find("light", &ref) == VT_ERROR_NO_TYPE && vt_property_find("light.brightness", &ref) ==
        VT_ERROR_NO_PROPERTY, "unknown names are told apart");

    /* ---- Property values ---- */
    check(vt_set_float(engine, lamp, property("light.intensity"), 2.5f) == VT_OK &&
        vt_get_float(engine, lamp, property("light.intensity"), &value) == VT_OK && value == 2.5f,
        "a float is written and read back");
    check(vt_set_int(engine, lamp, property("light.intensity"), 3) == VT_ERROR_TYPE_MISMATCH &&
        vt_get_float(engine, lamp, property("light.intensity"), &value) == VT_OK && value == 2.5f,
        "a setter of another type is refused and changes nothing");
    check(vt_set_int(engine, lamp, property("light.type"), 99) == VT_ERROR_INVALID_VALUE,
        "an enum value outside its options is refused");
    check(vt_set_text(engine, lamp, property("light.type"), "spot") == VT_OK &&
        vt_get_int(engine, lamp, property("light.type"), &number) == VT_OK &&
        number == optionValue(property("light.type"), "spot"), "an enum is set by name and read as its value");
    check(vt_set_text(engine, lamp, property("light.intensity"), "bright") == VT_ERROR_TYPE_MISMATCH,
        "unreadable text is refused");
    color.r = 1.0f; color.g = 0.5f; color.b = 0.25f; color.a = 1.0f;
    check(vt_set_color(engine, lamp, property("light.color"), color) == VT_OK &&
        vt_get_text(engine, lamp, property("light.color"), text, sizeof(text), &length) == VT_OK &&
        strcmp(text, "1, 0.5, 0.25, 1") == 0, "a colour reads back as text");
    position.x = 1.0f; position.y = 2.0f; position.z = 3.0f;
    check(vt_set_vec3(engine, car, property("node.position"), position) == VT_OK &&
        vt_get_vec3(engine, car, property("node.position"), &position) == VT_OK &&
        position.x == 1.0f && position.y == 2.0f && position.z == 3.0f, "a vector is written and read back");
    position.x = 0.0f; position.y = 90.0f; position.z = 0.0f;
    check(vt_set_vec3(engine, car, property("node.rotation"), position) == VT_OK &&
        vt_get_vec3(engine, car, property("node.rotation"), &position) == VT_OK && fabsf(position.y - 90.0f) < 1e-3f,
        "a rotation is set in degrees");
    check(vt_set_string(engine, car, property("node.name"), "Truck") == VT_OK &&
        vt_get_string(engine, car, property("node.name"), text, sizeof(text), &length) == VT_OK &&
        strcmp(text, "Truck") == 0 && vt_entity_find(engine, "Truck/Wheel", &found) == VT_OK,
        "renaming an entity changes its path");
    check(vt_get_entity(engine, wheel, property("node.parent"), &ref) == VT_OK && ref == car,
        "an entity reference reads as an id");
    check(vt_get_entity(engine, car, property("node.id"), &ref) == VT_OK && ref == car &&
        vt_set_entity(engine, car, property("node.id"), 5) == VT_ERROR_READ_ONLY, "a read-only property is refused");
    check(vt_get_float(engine, car, property("light.intensity"), &value) == VT_ERROR_NO_TARGET,
        "an entity without the component is no target");
    check(vt_get_float(engine, car, property("standard.gloss"), &value) == VT_ERROR_NO_TARGET,
        "a material property is not on an entity");
    check(vt_get_float(engine, car, 0, &value) == VT_ERROR_NO_PROPERTY, "property handle 0 is none");
    check(vt_entity_describe(engine, lamp, NULL, 0, &length) == VT_OK && length > 100,
        "an entity describes its properties");

    /* ---- The loop ---- */
    check(vt_entity_create(engine, 0, "Box", &box) == VT_OK && vt_component_add(engine, box, "render") == VT_OK &&
        vt_set_string(engine, box, property("render.type"), "box") == VT_OK, "a box is built from the API alone");
    check(vt_get_int(engine, box, property("render.meshInstanceCount"), &number) == VT_OK && number == 1,
        "and has its mesh");
    check(vt_entity_create(engine, 0, "Camera", &cam) == VT_OK && vt_component_add(engine, cam, "camera") == VT_OK,
        "a camera too");
    position.x = 0.0f; position.y = 0.0f; position.z = 6.0f;
    vt_set_vec3(engine, cam, property("node.position"), position);
    check(vt_update(engine, -1.0f) == VT_ERROR_INVALID_ARGUMENT, "a negative dt is refused");
    check(vt_frame(engine, 1.0f / 60.0f, &flag) == VT_OK && flag == 0, "the first frame starts the engine");
    check(vt_engine_start(engine) == VT_ERROR_INVALID_STATE, "it cannot start twice");
    check(vt_frame(engine, 1.0f / 60.0f, NULL) == VT_OK, "a second frame");
    check(vt_update(engine, 1.0f / 60.0f) == VT_OK && vt_render(engine) == VT_OK, "update and render apart");
    stats.size = sizeof(stats);
    check(vt_frame_stats_get(engine, &stats) == VT_OK && stats.frames == 3 && stats.cameras == 1 &&
        stats.draw_calls >= 1 && stats.triangles >= 12, "three frames rendered, the box drawn by the camera");

    /* ---- Destruction ---- */
    check(vt_entity_destroy(engine, car) == VT_OK, "an entity is destroyed");
    check(vt_entity_alive(engine, car, &flag) == VT_OK && flag == 0 && vt_entity_alive(engine, wheel, &flag) == VT_OK &&
        flag == 0, "it and its child are gone");
    check(vt_get_vec3(engine, car, property("node.position"), &position) == VT_ERROR_NOT_FOUND,
        "a call naming it fails cleanly");
    check(vt_entity_create(engine, 0, "Later", &found) == VT_OK && found > box, "a new entity takes a new id");
    check(vt_entity_destroy(engine, root) == VT_ERROR_INVALID_STATE, "the root cannot be destroyed");

    /* ---- Two engines, and the thread ---- */
    check(vt_engine_create(&desc, &other) == VT_OK, "a second engine");
    check(vt_entity_alive(other, lamp, &flag) == VT_OK && flag == 0 &&
        vt_component_add(other, lamp, "light") == VT_ERROR_NOT_FOUND, "it does not see the first engine's entities");
    vt_engine_destroy(other);

    probe.engine = engine;
    probe.engineCall = VT_OK;
    probe.schemaCall = VT_ERROR_INTERNAL;
    check(pthread_create(&thread, NULL, probeThread, &probe) == 0 && pthread_join(thread, NULL) == 0, "a second thread ran");
    check(probe.engineCall == VT_ERROR_WRONG_THREAD, "another thread may not drive the engine");
    check(probe.schemaCall == VT_OK, "but may read the schema");

    check(vt_engine_destroy(engine) == VT_OK && vt_engine_destroy(NULL) == VT_OK, "the engine is destroyed");

    printf("capi: %d passed, %d failed\n", checks - failures, failures);
    return failures == 0 ? 0 : 1;
}
