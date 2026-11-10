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
 * two-call buffers, a destroyed entity's id, two engines, and the thread check; then the
 * resources: the scene's settings, materials, meshes and textures from arrays, what releasing
 * them leaves alive, and the offscreen camera, whose pixels the null backend cannot read.
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

/* Counts a model's entities, and its cameras and lights still enabled, below `entity`. */
static void walk(vt_engine* engine, vt_entity entity, int* entities, int* cameras, int* lights, int* enabled)
{
    vt_entity children[64];
    size_t count = 0, i;
    int32_t has = 0, on = 0;
    ++*entities;
    if (vt_component_has(engine, entity, "camera", &has) == VT_OK && has) {
        ++*cameras;
        if (vt_get_bool(engine, entity, property("camera.enabled"), &on) == VT_OK && on) {
            ++*enabled;
        }
    }
    if (vt_component_has(engine, entity, "light", &has) == VT_OK && has) {
        ++*lights;
        if (vt_get_bool(engine, entity, property("light.enabled"), &on) == VT_OK && on) {
            ++*enabled;
        }
    }
    vt_entity_children(engine, entity, children, 64, &count);
    for (i = 0; i < count && i < 64; ++i) {
        walk(engine, children[i], entities, cameras, lights, enabled);
    }
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

    /* ---- Resources ---- */
    {
        vt_resource scene = 0, material = 0, mesh = 0, texture = 0, other_material = 0;
        vt_mesh_desc meshDesc;
        vt_texture_desc textureDesc;
        const float positions[] = {-1, 0, 0, 1, 0, 0, 0, 1, 0, 0, -1, 0};
        const uint32_t indices[] = {0, 1, 2, 1, 0, 3};
        const uint32_t badIndices[] = {0, 1, 2, 1, 0, 7};
        uint8_t pixels[2 * 2 * 4];
        uint8_t image[64];
        uint32_t w = 0, h = 0;
        vt_entity quad = 0, offscreenCamera = 0;
        vt_color diffuse;
        int stage;

        memset(pixels, 200, sizeof(pixels));
        check(vt_engine_scene(engine, &scene) == VT_OK && (scene & VT_RESOURCE_BIT) != 0, "the scene is a resource id");
        color.r = 0.25f; color.g = 0.5f; color.b = 0.75f; color.a = 1.0f;
        check(vt_set_color(engine, scene, property("scene.ambientLight"), color) == VT_OK &&
            vt_get_text(engine, scene, property("scene.ambientLight"), text, sizeof(text), &length) == VT_OK &&
            strcmp(text, "0.25, 0.5, 0.75, 1") == 0, "the scene's ambient light is set through the property calls");
        check(vt_set_text(engine, scene, property("scene.fogType"), "exp2") == VT_OK &&
            vt_set_float(engine, scene, property("scene.fogDensity"), 0.05f) == VT_OK &&
            vt_set_bool(engine, scene, property("scene.fogEnabled"), 1) == VT_OK, "and its fog");
        check(vt_set_float(engine, scene, property("light.intensity"), 1.0f) == VT_ERROR_NO_TARGET,
            "a resource takes only its own type's properties");
        check(vt_resource_release(engine, scene) == VT_ERROR_INVALID_STATE, "the scene cannot be released");

        check(vt_material_create(engine, &material) == VT_OK && (material & VT_RESOURCE_BIT) != 0 &&
            material != scene, "a material is created");
        diffuse.r = 0.1f; diffuse.g = 0.8f; diffuse.b = 0.2f; diffuse.a = 1.0f;
        check(vt_set_color(engine, material, property("standard.diffuse"), diffuse) == VT_OK &&
            vt_set_bool(engine, material, property("standard.useMetalness"), 1) == VT_OK,
            "its properties are set like an entity's");

        memset(&meshDesc, 0, sizeof(meshDesc));
        meshDesc.size = sizeof(meshDesc);
        meshDesc.vertex_count = 4;
        meshDesc.positions = positions;
        meshDesc.index_count = 6;
        meshDesc.indices = badIndices;
        check(vt_mesh_create(engine, &meshDesc, &mesh) == VT_ERROR_INVALID_ARGUMENT, "an index past the vertices is refused");
        meshDesc.indices = indices;
        meshDesc.index_count = 5;
        check(vt_mesh_create(engine, &meshDesc, &mesh) == VT_ERROR_INVALID_ARGUMENT, "so is an index count not a multiple of 3");
        meshDesc.index_count = 6;
        check(vt_mesh_create(engine, &meshDesc, &mesh) == VT_OK, "a mesh is built from arrays, its normals derived");
        {
            const float raised[] = {-1, 0, 1, 1, 0, 1, 0, 1, 1, 0, -1, 1};
            vt_mesh_desc update = meshDesc;
            update.positions = raised;
            update.vertex_count = 3;
            check(vt_mesh_update(engine, mesh, &update) == VT_ERROR_INVALID_ARGUMENT,
                "an update with another vertex count is refused");
            update.vertex_count = 4;
            check(vt_mesh_update(engine, mesh, &update) == VT_OK, "the mesh's vertices are replaced");
        }
        check(vt_get_float(engine, mesh, property("standard.gloss"), &value) == VT_ERROR_NO_TARGET,
            "a mesh has no properties");

        memset(&textureDesc, 0, sizeof(textureDesc));
        textureDesc.size = sizeof(textureDesc);
        textureDesc.width = 2;
        textureDesc.height = 2;
        textureDesc.format = VT_PIXEL_RGBA8;
        textureDesc.data = pixels;
        textureDesc.data_size = sizeof(pixels) - 1;
        check(vt_texture_create(engine, &textureDesc, &texture) == VT_ERROR_INVALID_ARGUMENT,
            "pixels of the wrong size are refused");
        textureDesc.data_size = sizeof(pixels);
        check(vt_texture_create(engine, &textureDesc, &texture) == VT_OK &&
            vt_texture_update(engine, texture, pixels, sizeof(pixels)) == VT_OK, "a texture is created and updated");
        check(vt_material_set_texture(engine, material, "diffuseMap", texture) == VT_OK &&
            vt_material_set_texture(engine, material, "sheenMap", texture) == VT_ERROR_INVALID_ARGUMENT &&
            vt_material_set_texture(engine, mesh, "diffuseMap", texture) == VT_ERROR_INVALID_ARGUMENT,
            "it goes on a named map of a material, and nowhere else");

        check(vt_entity_create(engine, 0, "Quad", &quad) == VT_OK && vt_component_add(engine, quad, "render") == VT_OK &&
            vt_render_set_material(engine, quad, material) == VT_OK && vt_render_set_mesh(engine, quad, mesh) == VT_OK &&
            vt_get_int(engine, quad, property("render.meshInstanceCount"), &number) == VT_OK && number == 1,
            "an entity draws the mesh with the material");
        check(vt_entity_describe(engine, quad, text, sizeof(text), &length) != VT_ERROR_INVALID_ARGUMENT &&
            vt_render_set_mesh(engine, lamp, mesh) == VT_ERROR_NO_TARGET, "an entity without a render component cannot");

        /* Released, the mesh, material and texture stay alive for the entity that uses them. */
        check(vt_resource_release(engine, mesh) == VT_OK && vt_resource_release(engine, material) == VT_OK &&
            vt_resource_release(engine, texture) == VT_OK, "the resources are released");
        check(vt_set_color(engine, material, property("standard.diffuse"), diffuse) == VT_ERROR_NOT_FOUND &&
            vt_resource_release(engine, material) == VT_ERROR_NOT_FOUND, "a released id finds nothing");
        stats.size = sizeof(stats);
        stage = vt_frame(engine, 1.0f / 60.0f, NULL) == VT_OK && vt_frame_stats_get(engine, &stats) == VT_OK;
        check(stage && stats.triangles >= 2, "and the entity still draws what they made");
        check(vt_material_create(engine, &other_material) == VT_OK && vt_render_set_material(engine, quad, other_material) == VT_OK &&
            vt_render_set_material(engine, quad, 0) == VT_OK && vt_frame(engine, 1.0f / 60.0f, NULL) == VT_OK,
            "the material is swapped, then reset to the default");
        check(vt_entity_destroy(engine, quad) == VT_OK && vt_frame(engine, 1.0f / 60.0f, NULL) == VT_OK,
            "the entity is destroyed with the last reference to the released resources");

        check(vt_entity_create(engine, 0, "Offscreen", &offscreenCamera) == VT_OK &&
            vt_component_add(engine, offscreenCamera, "camera") == VT_OK &&
            vt_camera_read_pixels(engine, offscreenCamera, NULL, 0, &w, &h) == VT_ERROR_INVALID_STATE,
            "a camera that renders to the window has no pixels to read");
        check(vt_camera_set_offscreen(engine, offscreenCamera, 4, 3) == VT_OK &&
            vt_camera_read_pixels(engine, offscreenCamera, NULL, 0, &w, &h) == VT_OK && w == 4 && h == 3,
            "an offscreen camera reports its size");
        check(vt_camera_read_pixels(engine, offscreenCamera, image, 8, &w, &h) == VT_ERROR_BUFFER_TOO_SMALL &&
            vt_frame(engine, 1.0f / 60.0f, NULL) == VT_OK &&
            vt_camera_read_pixels(engine, offscreenCamera, image, sizeof(image), &w, &h) == VT_ERROR_UNSUPPORTED,
            "the null backend renders it but has no pixels to give");
        check(vt_camera_set_offscreen(engine, offscreenCamera, 0, 0) == VT_OK &&
            vt_entity_destroy(engine, offscreenCamera) == VT_OK, "and it goes back to the window");
    }

    /* ---- Models, loaded textures, camera controls ---- */
    {
        char file[1024];
        vt_entity model = 0, heart = 0, viewer = 0, notCamera = 0;
        vt_resource loaded = 0, material = 0;
        vt_vec3 center, half;
        int entities = 0, cameras = 0, lights = 0, enabled = 0;

        snprintf(file, sizeof(file), "%s/models/geometry-camera-light.glb", VT_TEST_ASSETS);
        check(vt_gltf_load(engine, file, 0, &model) == VT_OK && model != 0, "a glTF model is loaded");
        walk(engine, model, &entities, &cameras, &lights, &enabled);
        printf("        %d entities, %d cameras, %d lights\n", entities, cameras, lights);
        check(entities > 1 && cameras >= 1 && lights >= 1 && enabled == 0,
            "its hierarchy is entities, its camera and light come disabled");
        check(vt_entity_bounds(engine, model, &center, &half) == VT_OK && half.x > 0.0f && half.y > 0.0f,
            "its bounds are what it draws");
        check(vt_frame(engine, 1.0f / 60.0f, NULL) == VT_OK, "and a frame draws it");
        snprintf(file, sizeof(file), "%s/models/heart_draco.glb", VT_TEST_ASSETS);
        check(vt_gltf_load(engine, file, model, &heart) == VT_OK && vt_entity_parent(engine, heart, &parent) == VT_OK &&
            parent == model, "a Draco-compressed model loads below another entity");
        check(vt_gltf_load(engine, "/no/such/model.glb", 0, &heart) == VT_ERROR_NOT_FOUND, "a missing file is not found");
        snprintf(file, sizeof(file), "%s/textures/checkboard.png", VT_TEST_ASSETS);
        check(vt_gltf_load(engine, file, 0, &heart) == VT_ERROR_INVALID_ARGUMENT, "an image is not a model");

        check(vt_texture_load(engine, file, VT_TEXTURE_MIPMAPS, &loaded) == VT_OK && vt_material_create(engine, &material) == VT_OK &&
            vt_material_set_texture(engine, material, "diffuseMap", loaded) == VT_OK, "an image file is loaded as a texture");
        check(vt_texture_load(engine, "/no/such/image.png", 0, &loaded) == VT_ERROR_NOT_FOUND, "a missing image is not found");
        check(vt_resource_release(engine, material) == VT_OK, "and released with its material");
        {
            vt_resource atlas = 0;
            snprintf(file, sizeof(file), "%s/cubemaps/studio-env-atlas.png", VT_TEST_ASSETS);
            check(vt_texture_load(engine, file, VT_TEXTURE_ENV_ATLAS, &atlas) == VT_OK &&
                vt_scene_set_environment(engine, atlas) == VT_OK && vt_resource_release(engine, atlas) == VT_OK &&
                vt_frame(engine, 1.0f / 60.0f, NULL) == VT_OK, "an environment atlas lights the scene, kept past its release");
            check(vt_scene_set_environment(engine, model) == VT_ERROR_NOT_FOUND &&
                vt_scene_set_environment(engine, 0) == VT_OK && vt_frame(engine, 1.0f / 60.0f, NULL) == VT_OK,
                "an entity is no environment, and 0 removes it");
            snprintf(file, sizeof(file), "%s/textures/checkboard.png", VT_TEST_ASSETS);
        }

        check(vt_entity_create(engine, 0, "Viewer", &viewer) == VT_OK && vt_component_add(engine, viewer, "camera") == VT_OK &&
            vt_component_add(engine, viewer, "cameraControls") == VT_OK &&
            vt_component_has(engine, viewer, "cameraControls", &flag) == VT_OK && flag == 1,
            "a camera is given orbit controls");
        check(vt_entity_create(engine, 0, "NotACamera", &notCamera) == VT_OK &&
            vt_component_add(engine, notCamera, "cameraControls") == VT_ERROR_UNSUPPORTED, "an entity without a camera is not");
        position.x = 0.0f; position.y = 0.0f; position.z = 10.0f;
        vt_set_vec3(engine, viewer, property("node.position"), position);
        center.x = 0.0f; center.y = 0.0f; center.z = 0.0f;
        check(vt_set_vec3(engine, viewer, property("cameraControls.focusPoint"), center) == VT_OK &&
            vt_set_float(engine, viewer, property("cameraControls.orbitDistance"), 4.0f) == VT_OK &&
            vt_frame(engine, 1.0f / 60.0f, NULL) == VT_OK &&
            vt_get_vec3(engine, viewer, property("node.position"), &position) == VT_OK && fabsf(position.z - 4.0f) < 1e-3f,
            "its orbit distance moves the camera toward the focus point");
        check(vt_get_text(engine, viewer, property("cameraControls.mode"), text, sizeof(text), &length) == VT_OK,
            "its mode reads back");
        {
            /* Picking (the null backend answers from bounds) and the window's input (none here). */
            vt_entity picked = 0, target = 0;
            vt_vec3 at, far_ = {20.0f, 0.0f, 0.0f};   /* away from everything else in this scene */
            int32_t hit = 0, down = 1, pressed = 1;
            vt_input_state input;
            check(vt_entity_create(engine, 0, "Target", &target) == VT_OK && vt_component_add(engine, target, "render") == VT_OK &&
                vt_set_string(engine, target, property("render.type"), "box") == VT_OK, "a box to pick");
            vt_set_vec3(engine, target, property("node.position"), far_);
            position.x = 20.0f; position.y = 0.0f; position.z = 6.0f;
            vt_set_vec3(engine, viewer, property("node.position"), position);
            vt_set_vec3(engine, viewer, property("cameraControls.focusPoint"), far_);
            vt_frame(engine, 1.0f / 60.0f, NULL);
            check(vt_pick(engine, viewer, 160.0f, 100.0f, &hit, &picked, &at) == VT_OK && hit == 1 && picked == target,
                "the middle of the view picks the box");
            check(vt_pick(engine, viewer, 2.0f, 2.0f, &hit, &picked, NULL) == VT_OK && hit == 0 && picked == 0,
                "a corner picks nothing");
            check(vt_pick(engine, target, 160.0f, 100.0f, &hit, &picked, NULL) == VT_ERROR_NO_TARGET,
                "an entity without a camera cannot pick");
            input.size = sizeof(input);
            check(vt_input_get(engine, &input) == VT_OK && input.buttons == 0 && input.pressed == 0 && input.inside == 0,
                "a windowless engine has no input");
            check(vt_key_get(engine, 4, &down, &pressed) == VT_OK && down == 0 && pressed == 0 &&
                vt_key_get(engine, 100000, &down, &pressed) == VT_ERROR_INVALID_ARGUMENT, "nor keys");
            vt_entity_destroy(engine, target);
        }
        check(vt_component_remove(engine, viewer, "cameraControls") == VT_OK &&
            vt_component_has(engine, viewer, "cameraControls", &flag) == VT_OK && flag == 0 &&
            vt_frame(engine, 1.0f / 60.0f, NULL) == VT_OK, "and loses them again");
        vt_entity_destroy(engine, model);
        vt_entity_destroy(engine, viewer);
        vt_entity_destroy(engine, notCamera);
    }

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
