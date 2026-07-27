# VisuTwin Canvas

**Home page**: [canvas.visutwin.com](https://canvas.visutwin.com)

A C++ 3D rendering engine targeting **Apple Metal**, derived from the [PlayCanvas](https://playcanvas.com/) open-source JavaScript engine.

VisuTwin Canvas ports PlayCanvas's architecture, class hierarchy, and algorithms to C++23, replacing the WebGL/WebGPU rendering backend with Apple Metal.

> **Status: Alpha.** The API is not stable. See [Implementation Status](#implementation-status) below.

## Features

### Rendering & materials
- **Forward PBR renderer** (metalness/roughness) with multi-light support (directional, point, spot, rectangular/disk/spherical **area lights** via LTC) and a frame-graph pass scheduler
- **StandardMaterial** with clearcoat, anisotropy, sheen, iridescence, transmission, parallax, **spec-gloss** (KHR_materials_pbrSpecularGlossiness), **Oren-Nayar** diffuse, **detail normals** (UDN), and vertex-stage **displacement mapping**
- **Image-based lighting**: environment atlas (GGX/Lambert prefiltered), HDR cubemap skybox, **ambient SH light probes**, and **box-projected cubemap reflection probes** (parallax-corrected local reflections)
- **Dynamic grab-pass refraction** with chromatic **dispersion** and KHR_materials_volume Beer-law attenuation
- **Screen-space reflections** (per-fragment world-space ray march against a scene depth+color grab, with env/probe fallback)
- **Vertex colors**, **point-size** primitives, **opacity dither** (Bayer8 order-independent transparency), and **lightmap** (UV1) sampling
- **GPU instancing** with per-instance color and optional per-frame GPU frustum culling (compute-driven indirect draw), plus **dynamic batching** with a bone-index matrix palette

### Lighting & shadows
- **Cascaded shadow maps** (4-cascade PSSM with cross-cascade blending and distance fade)
- **PCF**, **EVSM_16F** (Exponential Variance Shadow Maps: separable Gaussian blur, Chebyshev sampling, caster-AABB depth tightening), and **PCSS** contact-hardening soft shadows for directional lights
- **Spot/point** 2D depth maps and **omnidirectional cubemap** shadows, with PCSS also supported on spot/omni local lights
- **Clustered lighting** for many-light scenes, plus a **shadow catcher** material for compositing

### Animation & geometry
- **GPU skinning** (4-bone weighted blend) and **morph targets**, with skinned-mesh bone-AABB frustum culling
- **Animation state graph** (state machine, 1D/2D/directional/direct blend trees, typed parameters, crossfades) alongside the legacy animation component
- **Morph-weight animation** from glTF `weights` channels

### Gaussian splatting & particles
- **Gaussian splatting** (classic path): 3DGS PLY loading, CPU-precomputed covariance, background depth sorter, EWA screen-space projection — with **view-dependent spherical harmonics** (bands 1–3) and the SuperSplat **`.compressed.ply`** quantized format
- **GPU particle system**: compute-simulated pool, curve-driven size/color/alpha over life, box/sphere emitters, sprite-sheet animation, additive/normal/premultiplied blending

### Post-processing & tooling
- **TAA**, **SSAO**, **bloom**, **depth of field**, **edge detection**, and a compose chain with **color grading**, **3D LUT**, chromatic **fringing**, **color enhance**, **vignette**, and tone mapping (Linear, Filmic, ACES, **ACES2**, Neutral, None)
- **Planar reflections** with distance-based blur, **atmosphere/sky scattering** (Nishita), and **surface LIC** flow visualization
- **GPU timestamp profiler** (per-pass MTLCounterSampleBuffer timings)
- **KTX2/Basis compressed textures** transcoded to ASTC 4×4 on the loader thread
- **ImGui overlay** (Metal/SDL3 bindings) for digital-twin HUDs, **immediate-mode** debug rendering, **transform gizmos**, and an **outline renderer** + **view cube** (extras)

### Foundation
- **Scene graph** with an entity-component system (13 component types) and layer composition with render-action scheduling
- **GLB/glTF loading** with Draco decompression, plus OBJ/STL/Assimp parsers
- **Screen-space UI** with anchored elements, buttons, and text rendering
- **SIMD math** with SSE, ARM NEON, and Apple SIMD backends (Apple SIMD active on Apple Silicon)
- **ShaderChunks registry**: 24 named, user-overridable Metal micro-chunks with cache-invalidation hashing, plus build-time-embedded standalone shaders
- **XR / ARKit** framework (in development)

## Supported Platforms

- macOS (Apple Silicon and Intel) with Metal or Vulkan
- Vulkan 1.3 backend for macOS/Linux/Windows (optional build feature)

## Build

### Prerequisites

- CMake 3.28+
- C++23 compiler (Clang 16+ / Apple Clang 15+)
- [vcpkg](https://vcpkg.io/) package manager
- Ninja build system (recommended)

### Build Steps

```bash
# Set VCPKG_ROOT if not already set
export VCPKG_ROOT=/path/to/vcpkg

# Configure and build
cmake --preset default
cmake --build build

# Configure and build the examples separately
cmake --preset examples
cmake --build build-examples
```

`VISUTWIN_BUILD_EXAMPLES` defaults to `OFF`, keeping the normal CMake/CLion
project focused on the engine, tools, and tests. The `examples` preset enables
the example applications in its separate `build-examples` directory.

Presets: `default` (Debug), `release`, `examples`, and `vulkan`.
On macOS, `default` selects Metal only and `vulkan` selects Vulkan only.
The backends can also be selected explicitly with
`VISUTWIN_BACKEND_METAL=ON|OFF` and `VISUTWIN_BACKEND_VULKAN=ON|OFF`;
at least one backend must be enabled.

Debug builds enable Vulkan validation by default. The Vulkan preset registers
`vulkan-validation-smoke` with CTest; any validation-layer error fails the test:

```bash
cmake --preset vulkan
cmake --build build-vulkan --target visutwin-vulkan-smoke
ctest --test-dir build-vulkan -R vulkan-validation-smoke --output-on-failure
```

### Dependencies

All dependencies are managed via vcpkg (see `vcpkg.json`):

| Library | Purpose |
|---------|---------|
| SDL3 | Windowing and input |
| spdlog | Logging |
| tinyobjloader | OBJ mesh loading |
| tinygltf | glTF/GLB parsing |
| draco | Mesh compression |
| assimp | Multi-format model loading |
| basisu | KTX2/Basis texture transcoding |
| Boost.Core | Core utilities |
| imgui | UI overlay (Metal + SDL3 bindings, docking) |
| implot | Chart and plotting for ImGui |

Optional (enabled via `vulkan` feature):

| Library | Purpose |
|---------|---------|
| vulkan-headers | Vulkan API headers |
| vulkan-memory-allocator | GPU memory management |
| vk-bootstrap | Vulkan initialization |

Additionally, `metal-cpp` (Apple) and `stb` (Sean Barrett) are vendored in `engine/lib/`.

## Examples

37 example applications in `examples/`:

| Example | Description |
|---------|-------------|
| orbit | Orbital camera with GLB model and environment lighting |
| taa | Temporal anti-aliasing with a PBR scene |
| glb-loader | Loading and rendering GLB models |
| material-test | PBR material properties (metalness, gloss, normal maps) |
| material-stubs | Spec-gloss, Oren-Nayar, detail normals, displacement |
| clearcoat | Clearcoat dual-specular layer showcase |
| anisotropy | Anisotropic specular highlights |
| sheen | Fabric/velvet sheen layer |
| iridescence | Thin-film iridescence (soap-bubble color shift) |
| procedural-sky | Nishita atmosphere / procedural sky with animated sun |
| post-processing | Compose chain: bloom, DOF, vignette, color grading, fringing |
| custom-shader | ShaderMaterial with a user-supplied Metal toon shader |
| mesh-decals | Decal projection |
| shadow-cascades | Cascaded shadow maps |
| clustered-lighting | Many local lights via 3D cluster grid (+ atlas spot shadows) |
| lights | Directional / omni / spot / area light types |
| transform-rotate | Rotate gizmo |
| transform-scale | Scale gizmo |
| depth-of-field | Bokeh depth-of-field with interactive focus |
| dynamic-batching | Dynamic mesh batching of many shared-material objects |
| blend-trees-2d | 2D-cartesian animation blend tree |
| ui-text | Screen-space UI text (Screen + Element components) |
| pcss-dither | PCSS soft shadows + opacity dither |
| pcss-local | Spot/omni PCSS contact-hardening shadows |
| ambient-occlusion | Screen-space ambient occlusion |
| light-probes | Ambient SH light probes |
| area-light | LTC area lights (rect / disk / sphere) |
| reflection-planar-blurred | Planar reflections with blur |
| reflection-probe | Box-projected cubemap reflection probe |
| reflection-probe-dynamic | Runtime scene-capture reflection probe (live cubemap) |
| refraction | Dynamic grab-pass refraction + dispersion + volume |
| ssr | Screen-space reflections on a glossy floor |
| edge-detect | Post-processing edge detection |
| shader-chunks | ShaderChunks registry overrides (global + per-material) |
| gsplat | Gaussian splatting (classic path) |
| gsplat-tier2 | Splatting with view-dependent SH + compressed PLY |
| particles | GPU particle system (fire / smoke / sparks) |
| animation | Skeletal animation playback (GPU skinning) |
| anim-stategraph | Animation state graph with blend trees |
| morph-anim | Morph-weight animation + skinned culling |
| instancing-basic | GPU instancing |
| layers | Render layer composition |
| multi-view | Multiple camera viewports |
| render-to-texture | Off-screen rendering |
| lightmap-bake | CPU-baked lightmap (soft shadows + AO) applied at UV1 |
| raycast | Mouse picking via ray casting |
| area-picker | Area selection / picking |
| gizmo-translate | Transform gizmo interaction |
| outline-viewcube | Selection outlines + orientation view cube |
| world-to-screen | Screen-space UI with world anchors |

A shared `cameraControls` utility provides orbit, fly, focus, and auto far-clip camera modes across examples.

### Running Examples

After building, example executables are in `build/examples/`. Each is a macOS app bundle:

```bash
open build/examples/visutwin-taa.app
```

### Example Assets

Examples expect asset files in the `assets/` directory. Some examples generate their test assets procedurally; others need models, textures, or HDR environment maps you provide.

Recommended free asset sources:
- [Poly Haven](https://polyhaven.com/) (CC0 HDR environment maps and textures)
- [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) (CC-BY-4.0 test models)

## Project Structure

```
visutwin-canvas/
├── engine/                        # Engine library (488 C++ source/header files)
│   ├── src/
│   │   ├── core/                  # Math, events, tags, shapes, curves, utilities
│   │   ├── platform/
│   │   │   ├── graphics/          # Graphics abstraction layer
│   │   │   │   ├── metal/         # Metal backend (+ 11 compute/post passes)
│   │   │   │   └── vulkan/        # Vulkan 1.3 backend
│   │   │   └── input/             # Keyboard, mouse, gamepad, touch
│   │   ├── scene/                 # Scene graph, renderer, materials, lighting
│   │   │   ├── composition/       # Layer composition, render actions
│   │   │   ├── graphics/          # Env lighting, embedded LTC LUTs
│   │   │   ├── gsplat/            # Gaussian splatting (data, sorter, resource)
│   │   │   ├── particles/         # GPU particle emitter
│   │   │   ├── renderer/          # Forward renderer + render passes
│   │   │   ├── shader-lib/        # ProgramLibrary + ShaderChunks registry
│   │   │   └── immediate/         # Immediate-mode rendering
│   │   ├── framework/             # Engine, Entity, Components, ECS
│   │   │   ├── anim/              # Animation state graph + controller
│   │   │   ├── components/        # 13 component types
│   │   │   ├── extras/            # Outline renderer, view cube
│   │   │   ├── gizmo/             # Transform gizmos
│   │   │   └── xr/                # XR / ARKit support
│   │   ├── viz/overlay/           # ImGui overlay for digital-twin HUD
│   │   └── util/                  # General utilities
│   ├── lib/                       # Vendored: metal-cpp, stb
│   └── shaders/metal/
│       ├── chunks/                # 24 composable Metal shader micro-chunks
│       └── embedded/              # Standalone shaders embedded at build time
├── examples/                      # 37 example applications
├── tools/                         # Build and utility tools
└── assets/                        # Example assets (some procedural, some user-provided)
```

Sibling repositories (separate CMake projects): `visutwin-geo` (geospatial — WGS84, 3D Tiles, globe camera, atmosphere) and `visutwin-viz` (scientific visualization — volume rendering, marching cubes, streamlines).

## Implementation Status

| Module | Coverage | Notes |
|--------|----------|-------|
| Core / Math | ~80% | Vector2/3/4, Matrix4, Quaternion, Curve, Color, Random (SIMD multi-backend) |
| Core / Events | ~95% | EventHandler, EventHandle |
| Core / Shapes | ~70% | BoundingBox, BoundingSphere, OrientedBox, Plane, Ray, Tri |
| Scene / Renderer | ~80% | Forward PBR, camera frame graph, 23 render-pass classes, frustum/shadow-caster culling, layer sorting, and post-processing scheduling |
| Scene / Materials | ~85% | StandardMaterial with clearcoat, sheen, iridescence, transmission/dispersion/volume, anisotropy, parallax, spec-gloss, Oren-Nayar, detail normals, and displacement |
| Scene / Lighting | ~80% | Directional/point/spot + rectangular LTC area lights, clustered lighting/cookie atlas, ambient SH probes, and box-projected reflection probes |
| Scene / Shadows | ~85% | 4-cascade CSM (PSSM + blending), PCF/EVSM_16F/PCSS for directional, spot/point depth maps + omni cubemaps, PCSS on local lights |
| Scene / Shader-lib | ~80% | 24 overridable Metal chunks, 50 compile-time feature flags, cache-invalidation hashing, and 3 embedded standalone shaders |
| Scene / Graphics | ~70% | Camera-frame/post stack with bloom, SSAO, TAA, DOF, compose, color/depth grabs, environment atlas/convolution, HDR cubemaps, and spherical harmonics |
| Scene / GSplat | ~60% | Classic 3DGS path, background depth sorting, SH bands 1–3 on Metal, and uncompressed/compressed SuperSplat PLY; Vulkan currently evaluates SH0 |
| Graphics / Metal | ~70% | Buffers/textures/pipelines, ASTC/BC formats, compute, particles/culling, post-processing, environment baking, GSplat, and GPU timestamp profiling |
| Graphics / Vulkan | ~65% | Vulkan 1.3 dynamic rendering/synchronization2, MRT, PBR draw binding, compute/particles/culling, post-processing, environment baking, async uploads, and validation smoke coverage |
| Framework / ECS | ~75% | Engine, Entity, component-system registry, scripts, hierarchy, and lifecycle/event integration |
| Framework / Components | ~55% | 13 types: Camera, Render, Light, Script, Animation, Anim (state graph), Screen, Element, Button, Collision, RigidBody, GSplat, ParticleSystem |
| Framework / Animation | ~75% | GPU skinning, morph targets/weights, clips/evaluator/binder, state graphs, transitions, and blend trees |
| Framework / Gizmo | ~75% | Interactive translate/rotate/scale handles with axis picking and snapping |
| Framework / Assets | ~65% | Async container/texture/font loading; GLB/glTF (+Draco), OBJ/STL/Assimp; KTX2/Basis transcoding to ASTC or BC |
| Viz / Overlay | ~45% | Metal-only ImGui/ImPlot HUD integration, input capture, digital-twin theme, and 3D-anchored labels/panels |

### Known Limitations

- Metal remains the primary graphics backend; Vulkan is functional but does not yet have complete feature parity
- Vulkan still trails Metal in some platform-specific tooling and recently added rendering paths
- No audio subsystem; no Sprite / layout / scroll-view UI components
- Gaussian splatting: WebP-packed SOG format and the unified octree/LOD streaming path are not ported
- Reflection probes support runtime scene-capture baking (dynamic cubemap) as well as supplied cubemaps; per-level GGX cube prefilter is deferred (roughness uses hardware trilinear cube mips)
- Texture streaming is partial (no progressive mip-level budgeting)
- The lightmapper baker is CPU-only (LDR, single bounce, no color+dir or auto-UV-unwrap)
- Screen-space reflections march per-fragment (no HiZ acceleration or roughness cone) and are sharp-only, with no temporal accumulation

## Attribution

This project is a C++ port of the [PlayCanvas engine](https://github.com/playcanvas/engine), which is licensed under the MIT License. See the [NOTICE](NOTICE) file for full attribution details.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the full text.
