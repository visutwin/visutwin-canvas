/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2025-2026 Arnis Lektauers
 *
 * Created by Arnis Lektauers on 09.11.2026
 *
 * The VisuTwin Canvas C API: the engine driven from C or from any language that calls C
 * (Julia's ccall, Python's ctypes, Rust, C#), in the caller's own process. C99, no C++ across
 * the boundary.
 *
 * Conventions
 *   - Every function returns a vt_status; VT_OK is 0. vt_last_error() then describes the last
 *     failure of the calling thread. Results come back through out-parameters.
 *   - Entities are named by vt_entity ids, never pointers: an id is unique in the process and
 *     never reused, so a call naming an entity that is gone fails with VT_ERROR_NOT_FOUND
 *     instead of touching freed memory. 0 names no entity. Resources (the scene, materials,
 *     meshes, textures) are named the same way by vt_resource ids, which have their top bit
 *     set, so the property calls take either kind of id.
 *   - Every struct the caller passes or fills begins with its `size` (set it to sizeof the
 *     struct): fields are only ever appended, and a library newer than the caller's header
 *     reads and writes only the fields that caller knows.
 *   - Strings passed in are UTF-8 and copied during the call. Strings handed out by the schema
 *     functions live as long as the process; a string read from an entity is written into the
 *     caller's buffer (the two-call pattern: pass a NULL buffer to learn the length).
 *   - One thread: every call naming an engine must come from the thread that created it, which
 *     on macOS must be the main thread; another thread gets VT_ERROR_WRONG_THREAD. The schema
 *     functions (types and properties) may be called from any thread.
 *   - The caller owns the loop: poll events, wait for the next frame, update, render — or
 *     vt_frame for all four.
 */
#ifndef VISUTWIN_H
#define VISUTWIN_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(VT_BUILDING_LIBRARY)
#    define VT_API __declspec(dllexport)
#  else
#    define VT_API __declspec(dllimport)
#  endif
#else
#  define VT_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Version and status --------------------------------------------------------------- */

#define VT_API_VERSION_MAJOR 0
#define VT_API_VERSION_MINOR 3

/** The library's API version: (major << 16) | minor. A caller built against major M runs on
 *  a library of major M and any minor at least its own. */
VT_API uint32_t vt_api_version(void);

typedef int32_t vt_status;
enum
{
    VT_OK = 0,
    VT_ERROR_INVALID_ARGUMENT = 1,  /* a NULL pointer, a bad size, a malformed name */
    VT_ERROR_INVALID_STATE = 2,     /* the call does not apply now (destroying the root, ...) */
    VT_ERROR_WRONG_THREAD = 3,      /* not the thread that created the engine */
    VT_ERROR_NOT_FOUND = 4,         /* no entity with that id or path */
    VT_ERROR_NO_TYPE = 5,           /* no type of that name */
    VT_ERROR_NO_PROPERTY = 6,       /* no property of that name */
    VT_ERROR_NO_TARGET = 7,         /* the entity has no component of the property's type */
    VT_ERROR_READ_ONLY = 8,
    VT_ERROR_TYPE_MISMATCH = 9,     /* a getter or setter of another value type, unreadable text */
    VT_ERROR_INVALID_VALUE = 10,    /* an enum value that is none of the options */
    VT_ERROR_BUFFER_TOO_SMALL = 11, /* the result was truncated; the length says what it needs */
    VT_ERROR_UNSUPPORTED = 12,      /* not available in this build or on this backend */
    VT_ERROR_INTERNAL = 13          /* the engine failed; vt_last_error says how */
};

/** A constant name for a status ("VT_ERROR_NOT_FOUND"). */
VT_API const char* vt_status_name(vt_status status);

/** The message of the last failed call on this thread, "" after a call that succeeded. Valid
 *  until the next call on this thread. */
VT_API const char* vt_last_error(void);

/* ---- Values ---------------------------------------------------------------------------- */

typedef uint64_t vt_entity;

typedef struct vt_vec2 { float x, y; } vt_vec2;
typedef struct vt_vec3 { float x, y, z; } vt_vec3;
/** Also a quaternion (x, y, z, w). */
typedef struct vt_vec4 { float x, y, z, w; } vt_vec4;
/** In the space its property's setter takes (a material colour is authored sRGB). */
typedef struct vt_color { float r, g, b, a; } vt_color;

typedef int32_t vt_value_type;
enum
{
    VT_VALUE_NONE = 0,
    VT_VALUE_BOOL = 1,
    VT_VALUE_INT = 2,
    VT_VALUE_FLOAT = 3,
    VT_VALUE_VEC2 = 4,
    VT_VALUE_VEC3 = 5,
    VT_VALUE_VEC4 = 6,
    VT_VALUE_QUAT = 7,
    VT_VALUE_COLOR = 8,
    VT_VALUE_STRING = 9,
    VT_VALUE_ENUM = 10,     /* read and written as an int; its names are the options */
    VT_VALUE_ENTITY = 11
};

/* ---- Engine ---------------------------------------------------------------------------- */

typedef struct vt_engine vt_engine;

typedef int32_t vt_backend;
enum
{
    VT_BACKEND_DEFAULT = 0, /* VISUTWIN_BACKEND if set, otherwise the build's preferred GPU */
    VT_BACKEND_METAL = 1,
    VT_BACKEND_VULKAN = 2,
    VT_BACKEND_NULL = 3     /* no window and no GPU: everything runs, nothing is drawn */
};

enum
{
    VT_ENGINE_NO_ANTIALIAS = 1u << 0,   /* a single-sampled back buffer (4x MSAA otherwise) */
    VT_ENGINE_NO_VSYNC = 1u << 1,       /* Metal: present as fast as frames complete */
    VT_ENGINE_HIDDEN = 1u << 2          /* the window is not shown: render offscreen and read back */
};

typedef struct vt_engine_desc
{
    uint32_t size;          /* sizeof(vt_engine_desc) */
    const char* title;      /* the window title; NULL for "VisuTwin" */
    int32_t width;          /* the window (or null back buffer) size in points; 0 for 1280 x 720 */
    int32_t height;
    vt_backend backend;
    uint32_t flags;         /* VT_ENGINE_* */
} vt_engine_desc;

/** An engine with a window (or none, for VT_BACKEND_NULL) and the standard component types:
 *  light, camera, render, script, screen and element. The calling thread becomes its thread. */
VT_API vt_status vt_engine_create(const vt_engine_desc* desc, vt_engine** out);

/** Destroys the engine, every entity in it and its window. NULL does nothing. */
VT_API vt_status vt_engine_destroy(vt_engine* engine);

VT_API vt_status vt_engine_backend(vt_engine* engine, vt_backend* out);

/** Runs the initialize phases and renders the first frame. Call once the initial scene exists;
 *  the first vt_update, vt_render or vt_frame calls it if nobody did, and then does no more. */
VT_API vt_status vt_engine_start(vt_engine* engine);

/** Delivers the pending window and input events to the engine; *quit (may be NULL) becomes 1
 *  once the window was asked to close. */
VT_API vt_status vt_poll_events(vt_engine* engine, int32_t* quit);

/** Waits until the next frame can be rendered (the display and the GPU); optional, but it
 *  makes the input a frame shows one display interval fresher. */
VT_API vt_status vt_wait_next_frame(vt_engine* engine);

/** Advances the scene by dt seconds (scripts, animation, physics, layout). */
VT_API vt_status vt_update(vt_engine* engine, float dt);

/** Renders and presents a frame. */
VT_API vt_status vt_render(vt_engine* engine);

/** vt_poll_events, vt_wait_next_frame, vt_update(dt) and vt_render. */
VT_API vt_status vt_frame(vt_engine* engine, float dt, int32_t* quit);

typedef struct vt_frame_stats
{
    uint32_t size;          /* sizeof(vt_frame_stats) */
    uint64_t frames;        /* frames rendered since the engine was created */
    int32_t draw_calls;     /* in the last frame rendered */
    int32_t triangles;
    int32_t cameras;
} vt_frame_stats;

VT_API vt_status vt_frame_stats_get(vt_engine* engine, vt_frame_stats* out);

/* ---- Entities -------------------------------------------------------------------------- */

/** The root every entity hangs below. */
VT_API vt_status vt_entity_root(vt_engine* engine, vt_entity* out);

/** A new, enabled entity under `parent` (0 for the root). */
VT_API vt_status vt_entity_create(vt_engine* engine, vt_entity parent, const char* name, vt_entity* out);

/** Destroys the entity and everything below it, and frees them. The root cannot be destroyed. */
VT_API vt_status vt_entity_destroy(vt_engine* engine, vt_entity entity);

/** *out is 1 while the entity exists in this engine. */
VT_API vt_status vt_entity_alive(vt_engine* engine, vt_entity entity, int32_t* out);

/** The entity at a path: names from below the root joined by '/' ("Car/Wheel"), the first child
 *  of each name where siblings share one; "" is the root. */
VT_API vt_status vt_entity_find(vt_engine* engine, const char* path, vt_entity* out);

/** The path vt_entity_find reads. */
VT_API vt_status vt_entity_path(vt_engine* engine, vt_entity entity, char* buffer, size_t capacity,
    size_t* length);

/** The parent (0 for the root). */
VT_API vt_status vt_entity_parent(vt_engine* engine, vt_entity entity, vt_entity* out);

/** Moves the entity under `parent` (0 for the root), keeping its LOCAL transform. */
VT_API vt_status vt_entity_set_parent(vt_engine* engine, vt_entity entity, vt_entity parent);

/** The children in order: *count is always how many there are; up to `capacity` are written,
 *  and VT_ERROR_BUFFER_TOO_SMALL says some were left out. */
VT_API vt_status vt_entity_children(vt_engine* engine, vt_entity entity, vt_entity* buffer, size_t capacity,
    size_t* count);

/** Every property of every type the entity carries, one "type.property = value" line each. */
VT_API vt_status vt_entity_describe(vt_engine* engine, vt_entity entity, char* buffer, size_t capacity,
    size_t* length);

/* ---- Components ------------------------------------------------------------------------- */

/** Adds the component of a type ("light", "camera", "render", "element", "screen"); an entity
 *  that has one already keeps it. */
VT_API vt_status vt_component_add(vt_engine* engine, vt_entity entity, const char* type);

/** Removes it; VT_ERROR_NO_TARGET when the entity has none. */
VT_API vt_status vt_component_remove(vt_engine* engine, vt_entity entity, const char* type);

VT_API vt_status vt_component_has(vt_engine* engine, vt_entity entity, const char* type, int32_t* out);

/* ---- Schema: types and properties ------------------------------------------------------ */

/** A type ("node", "light", ...), 0 for none. Stable for the process. */
typedef uint32_t vt_type;
/** A property of a type, 0 for none. Stable for the process; look it up once and keep it. */
typedef uint64_t vt_property;

enum
{
    VT_TYPE_ON_ENTITY = 1u << 0,    /* found on an entity (the node, a component) */
    VT_TYPE_COMPONENT = 1u << 1     /* added and removed by vt_component_add / _remove */
};

typedef struct vt_type_info
{
    uint32_t size;              /* sizeof(vt_type_info) */
    const char* name;
    const char* description;
    uint32_t property_count;
    uint32_t flags;             /* VT_TYPE_* */
} vt_type_info;

enum
{
    VT_PROPERTY_READ_ONLY = 1u << 0
};

typedef struct vt_property_info
{
    uint32_t size;              /* sizeof(vt_property_info) */
    const char* name;           /* without its type ("intensity") */
    const char* description;    /* "" when none */
    const char* unit;           /* "" when none */
    vt_value_type value_type;
    uint32_t flags;             /* VT_PROPERTY_* */
    float min;                  /* editor hints, not enforced; -inf / inf / 0 when none */
    float max;
    float step;
    uint32_t option_count;      /* an enum's options */
    vt_type type;               /* the type it belongs to */
} vt_property_info;

VT_API vt_status vt_type_count(uint32_t* out);
/** The type at an index, 0 <= index < vt_type_count. */
VT_API vt_status vt_type_at(uint32_t index, vt_type* out);
VT_API vt_status vt_type_find(const char* name, vt_type* out);
VT_API vt_status vt_type_get_info(vt_type type, vt_type_info* out);

/** "light.intensity". */
VT_API vt_status vt_property_find(const char* qualified_name, vt_property* out);
/** The property at an index, 0 <= index < vt_type_info.property_count. */
VT_API vt_status vt_property_at(vt_type type, uint32_t index, vt_property* out);
VT_API vt_status vt_property_get_info(vt_property property, vt_property_info* out);
/** An enum's option at an index: its value and its name (valid for the process). */
VT_API vt_status vt_property_option(vt_property property, uint32_t index, int32_t* value, const char** name);

/* ---- Property values on an entity or a resource -------------------------------------------
 * Each pair reads and writes a property of its value type (vt_get_int / vt_set_int serve INT
 * and ENUM); another value type is VT_ERROR_TYPE_MISMATCH. The `entity` argument takes an
 * entity id or a resource id: the property's type must be on the entity, or be the resource's
 * own type ("scene.*" on the scene, "standard.*" on a material); VT_ERROR_NO_TARGET otherwise. */

VT_API vt_status vt_get_bool(vt_engine* engine, vt_entity entity, vt_property property, int32_t* out);
VT_API vt_status vt_set_bool(vt_engine* engine, vt_entity entity, vt_property property, int32_t value);
VT_API vt_status vt_get_int(vt_engine* engine, vt_entity entity, vt_property property, int32_t* out);
VT_API vt_status vt_set_int(vt_engine* engine, vt_entity entity, vt_property property, int32_t value);
VT_API vt_status vt_get_float(vt_engine* engine, vt_entity entity, vt_property property, float* out);
VT_API vt_status vt_set_float(vt_engine* engine, vt_entity entity, vt_property property, float value);
VT_API vt_status vt_get_vec2(vt_engine* engine, vt_entity entity, vt_property property, vt_vec2* out);
VT_API vt_status vt_set_vec2(vt_engine* engine, vt_entity entity, vt_property property, vt_vec2 value);
VT_API vt_status vt_get_vec3(vt_engine* engine, vt_entity entity, vt_property property, vt_vec3* out);
VT_API vt_status vt_set_vec3(vt_engine* engine, vt_entity entity, vt_property property, vt_vec3 value);
VT_API vt_status vt_get_vec4(vt_engine* engine, vt_entity entity, vt_property property, vt_vec4* out);
VT_API vt_status vt_set_vec4(vt_engine* engine, vt_entity entity, vt_property property, vt_vec4 value);
VT_API vt_status vt_get_quat(vt_engine* engine, vt_entity entity, vt_property property, vt_vec4* out);
VT_API vt_status vt_set_quat(vt_engine* engine, vt_entity entity, vt_property property, vt_vec4 value);
VT_API vt_status vt_get_color(vt_engine* engine, vt_entity entity, vt_property property, vt_color* out);
VT_API vt_status vt_set_color(vt_engine* engine, vt_entity entity, vt_property property, vt_color value);
VT_API vt_status vt_get_entity(vt_engine* engine, vt_entity entity, vt_property property, vt_entity* out);
VT_API vt_status vt_set_entity(vt_engine* engine, vt_entity entity, vt_property property, vt_entity value);
VT_API vt_status vt_get_string(vt_engine* engine, vt_entity entity, vt_property property, char* buffer,
    size_t capacity, size_t* length);
VT_API vt_status vt_set_string(vt_engine* engine, vt_entity entity, vt_property property, const char* value);

/** Any property as text, in the form vt_entity_describe writes (vectors "1, 2, 3", enums by
 *  name, bools "true" / "false"), whatever its value type. */
VT_API vt_status vt_get_text(vt_engine* engine, vt_entity entity, vt_property property, char* buffer,
    size_t capacity, size_t* length);
VT_API vt_status vt_set_text(vt_engine* engine, vt_entity entity, vt_property property, const char* text);

/* ---- Resources: the scene, materials, meshes and textures --------------------------------
 * A resource lives until vt_resource_release and, after that, for as long as something in the
 * scene still uses it (an entity drawing a mesh, a material using a texture). Arrays handed in
 * are read and copied during the call. */

typedef uint64_t vt_resource;
#define VT_RESOURCE_BIT (UINT64_C(1) << 63)

/** The engine's scene: ambient light, exposure, tone mapping, sky and fog ("scene.*"). It is
 *  the engine's and cannot be released. */
VT_API vt_status vt_engine_scene(vt_engine* engine, vt_resource* out);

/** A new standard material ("standard.*"): white, rough, lit, in the specular workflow with no
 *  specular until standard.useMetalness (or a specular colour) says otherwise. */
VT_API vt_status vt_material_create(vt_engine* engine, vt_resource* out);

typedef struct vt_mesh_desc
{
    uint32_t size;              /* sizeof(vt_mesh_desc) */
    uint32_t vertex_count;
    const float* positions;     /* xyz per vertex; required */
    const float* normals;       /* xyz per vertex; NULL: derived from the triangles */
    const float* uvs;           /* uv per vertex, v = 0 at the image's top row; NULL: zeros */
    uint32_t index_count;       /* a multiple of 3; 0: the vertices taken three at a time */
    const uint32_t* indices;    /* a triangle list, counter-clockwise seen from the front */
} vt_mesh_desc;

/** A triangle mesh from arrays (copied during the call). */
VT_API vt_status vt_mesh_create(vt_engine* engine, const vt_mesh_desc* desc, vt_resource* out);

/** Replaces a mesh's vertices, keeping its triangles: the same vertex count, positions required,
 *  normals and uvs as in vt_mesh_create (the index fields are ignored). Every entity drawing the
 *  mesh shows the new vertices from the next frame; the frames still drawing the old ones finish
 *  with them. */
VT_API vt_status vt_mesh_update(vt_engine* engine, vt_resource mesh, const vt_mesh_desc* desc);

typedef int32_t vt_pixel_format;
enum
{
    VT_PIXEL_RGBA8 = 1,     /* 4 bytes; a colour map is read as sRGB */
    VT_PIXEL_RGBA16F = 2,   /* 8 bytes, half floats, linear */
    VT_PIXEL_RGBA32F = 3    /* 16 bytes, floats, linear */
};

enum
{
    VT_TEXTURE_MIPMAPS = 1u << 0,   /* a mip chain, regenerated on every update */
    VT_TEXTURE_NEAREST = 1u << 1,   /* nearest filtering (linear otherwise) */
    VT_TEXTURE_CLAMP = 1u << 2      /* clamp at the edges (repeat otherwise) */
};

typedef struct vt_texture_desc
{
    uint32_t size;              /* sizeof(vt_texture_desc) */
    uint32_t width;
    uint32_t height;
    vt_pixel_format format;
    uint32_t flags;             /* VT_TEXTURE_* */
    const void* data;           /* rows top first, tightly packed; NULL: left uninitialised */
    size_t data_size;           /* width * height * the format's size */
} vt_texture_desc;

VT_API vt_status vt_texture_create(vt_engine* engine, const vt_texture_desc* desc, vt_resource* out);

/** Replaces the texture's pixels (the same size and format, rows top first). */
VT_API vt_status vt_texture_update(vt_engine* engine, vt_resource texture, const void* data, size_t data_size);

/** Puts a texture on one of a material's maps: "diffuseMap", "normalMap", "emissiveMap",
 *  "metalnessMap", "glossMap", "opacityMap" or "aoMap"; texture 0 clears the map. */
VT_API vt_status vt_material_set_texture(vt_engine* engine, vt_resource material, const char* map,
    vt_resource texture);

/** Gives up the caller's reference; what the scene still uses stays alive until it does not. */
VT_API vt_status vt_resource_release(vt_engine* engine, vt_resource resource);

/* ---- What an entity draws -------------------------------------------------------------- */

/** The entity's render component draws this mesh (with its material, or the default). */
VT_API vt_status vt_render_set_mesh(vt_engine* engine, vt_entity entity, vt_resource mesh);

/** The entity's render component draws with this material; 0 for the default. */
VT_API vt_status vt_render_set_material(vt_engine* engine, vt_entity entity, vt_resource material);

/* ---- Offscreen rendering and capture ---------------------------------------------------- */

/** The camera renders into an offscreen image of width x height pixels instead of the window
 *  (0 x 0: back to the window). With VT_ENGINE_HIDDEN this is headless rendering. */
VT_API vt_status vt_camera_set_offscreen(vt_engine* engine, vt_entity camera, uint32_t width, uint32_t height);

/** The offscreen camera's last rendered frame as RGBA8, rows top first, into `buffer`
 *  (width * height * 4 bytes; a NULL buffer only reports the size). Blocks until the GPU has
 *  finished the frame. VT_ERROR_UNSUPPORTED on the null backend, which has no pixels. */
VT_API vt_status vt_camera_read_pixels(vt_engine* engine, vt_entity camera, uint8_t* buffer, size_t capacity,
    uint32_t* width, uint32_t* height);

#ifdef __cplusplus
}
#endif

#endif /* VISUTWIN_H */
