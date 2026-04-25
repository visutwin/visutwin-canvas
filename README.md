# VisuTwin Canvas

**Home page**: [canvas.visutwin.com](https://canvas.visutwin.com)

A C++ 3D rendering engine targeting **Apple Metal**, derived from the [PlayCanvas](https://playcanvas.com/) open-source JavaScript engine.

VisuTwin Canvas ports PlayCanvas's architecture, class hierarchy, and algorithms to C++23, replacing the WebGL/WebGPU rendering backend with Apple Metal.

> **Status: Alpha.** The API is not stable. See [Implementation Status](#implementation-status) below.

## Features

- **PBR rendering** with metalness/roughness workflow, normal maps, environment lighting
- **Forward renderer** with multi-light support (directional, point, spot, area rect)
- **Shadow mapping** with PCF and **EVSM_16F** (Exponential Variance Shadow Maps with separable Gaussian blur and Chebyshev sampling) for directional lights, plus spot/point depth maps and omnidirectional cubemap shadows
- **Post-processing**: TAA, SSAO, bloom, depth of field, tone mapping (Linear, Filmic, ACES, **ACES2**, Neutral, None), edge detection
- **Clustered lighting** for many-light scenes
- **Scene graph** with entity-component system (11 component types)
- **GLB/glTF loading** with Draco mesh decompression
- **Skybox rendering** (box, dome, infinite) via cubemap or equirectangular environment atlas
- **Transform gizmos** (translate, rotate, scale)
- **Screen-space UI** with anchored elements, buttons, text rendering
- **GPU instancing** with per-instance color and optional per-frame GPU frustum culling (compute-driven indirect draw)
- **Dynamic batching** with bone-index matrix palette
- **Planar reflections** with distance-based blur
- **Shadow catcher** materials (multiplicative ground plane shadows)
- **Clearcoat, anisotropy, parallax, transmission, sheen, iridescence** PBR extensions
- **Vertex color** rendering and **point-size** primitives
- **Surface LIC** flow visualization shader path
- **SIMD math** with SSE, ARM NEON, and Apple SIMD backends (scalar fallback default)
- **ImGui overlay** with Metal/SDL3 bindings for digital twin HUD rendering
- **Immediate-mode rendering** API for debug lines and primitives
- **Gaussian splatting** unified rendering pipeline
- **Layer composition** with render action scheduling
- **XR / ARKit** framework (in development)

## Supported Platforms

- macOS (Apple Silicon and Intel) with Metal
- Vulkan backend in development (Linux/Windows, optional build feature)

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

21 example applications in `examples/`:

| Example | Description |
|---------|-------------|
| orbit | Orbital camera with GLB model and environment lighting |
| taa | Temporal anti-aliasing with PBR scene |
| glb-loader | Loading and rendering GLB models |
| material-test | PBR material properties (metalness, gloss, normal maps) |
| shadow-cascades | Cascaded shadow maps |
| ambient-occlusion | Screen-space ambient occlusion |
| layers | Render layer composition |
| multi-view | Multiple camera viewports |
| raycast | Mouse picking via ray casting |
| gizmo-translate | Transform gizmo interaction |
| edge-detect | Post-processing edge detection |
| render-to-texture | Off-screen rendering |
| instancing-basic | GPU instancing |
| instancing-culled | 10,000 instances with per-frame GPU frustum culling |
| reflection-planar-blurred | Planar reflections with blur |
| texture-stream | Dynamic texture updates |
| world-to-screen | Screen-space UI with world anchors |
| animation | Skeletal animation playback |
| assimp-loader | Multi-format model loading via Assimp |
| area-picker | Area selection / picking |
| env-reproject-test | Environment-atlas bake / reproject test (mipmap, GGX, Lambert) |

A shared `cameraControls` utility provides orbit, fly, focus, and auto far-clip camera modes across examples.

### Running Examples

After building, example executables are in `build/examples/`. Each is a macOS app bundle:

```bash
open build/examples/visutwin-taa.app
```

### Example Assets

Examples expect asset files in the `assets/` directory. You need to provide your own models, textures, and environment maps. See `assets/` for the expected directory structure.

Recommended free asset sources:
- [Poly Haven](https://polyhaven.com/) (CC0 HDR environment maps and textures)
- [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) (CC-BY-4.0 test models)

## Project Structure

```
visutwin-canvas/
├── engine/                        # Engine library (422 source files)
│   ├── src/
│   │   ├── core/                  # Math, events, tags, shapes, utilities
│   │   ├── platform/
│   │   │   ├── graphics/          # Graphics abstraction layer
│   │   │   │   ├── metal/         # Metal backend
│   │   │   │   └── vulkan/        # Vulkan backend (WIP)
│   │   │   └── input/             # Keyboard, mouse, gamepad, touch
│   │   ├── scene/                 # Scene graph, renderer, materials, lighting
│   │   │   ├── composition/       # Layer composition, render actions
│   │   │   ├── graphics/          # Post-processing passes (TAA, SSAO, bloom, DoF, etc.)
│   │   │   ├── gsplay-unified/    # Gaussian splatting (stub)
│   │   │   └── immediate/         # Immediate-mode rendering
│   │   ├── framework/             # Engine, Entity, Components, ECS
│   │   │   ├── xr/                # XR / ARKit support
│   │   │   └── gizmo/             # Transform gizmos
│   │   ├── viz/
│   │   │   └── overlay/           # ImGui overlay for digital twin HUD
│   │   ├── extras/                # Input utilities
│   │   └── util/                  # General utilities
│   ├── lib/                       # Vendored: metal-cpp, stb
│   └── shaders/metal/chunks/      # 7 composable Metal shader chunks (41 feature toggles)
├── examples/                      # 21 example applications
├── tools/                         # Build and utility tools
└── assets/                        # Example assets (user-provided)
```

## Implementation Status

| Module | Coverage | Notes |
|--------|----------|-------|
| Core / Math | ~75% | Vector2/3/4, Matrix4, Quaternion, Curve, Color, Random |
| Core / Events | ~95% | EventHandler, EventHandle |
| Core / Shapes | ~70% | BoundingBox, BoundingSphere, OrientedBox, Plane, Ray, Tri |
| Scene / Renderer | ~70% | Forward PBR, frustum culling, layer sorting |
| Scene / Materials | ~75% | StandardMaterial with 67 properties; clearcoat, sheen, iridescence, transmission, anisotropy, parallax all functional |
| Scene / Lighting | ~65% | Directional + point + spot + area rect, clustered lighting |
| Scene / Shadows | ~80% | Full 4-cascade CSM with PSSM splits and cross-cascade blending, **PCF + EVSM_16F (Exponential VSM with separable Gaussian blur, Chebyshev sampling, caster-AABB depth tightening)** for directional, spot/point depth maps, omni cubemaps |
| Scene / Shader-lib | ~75% | 7 Metal chunks (2k+ lines), 35 features implemented (incl. `VT_FEATURE_VSM_SHADOWS`), 7 stubbed |
| Scene / Graphics | ~50% | Environment atlas, HDR cubemap, 14 post-processing passes |
| Scene / Composition | ~50% | Layer composition, render action scheduling |
| Graphics / Metal | ~40% | Buffer, texture, pipeline functional |
| Graphics / Vulkan | ~10% | File structure in place, minimal implementation |
| Framework / ECS | ~70% | Engine, Entity, ComponentSystem, Script |
| Framework / Components | ~35% | 11 types: Camera, Render, Light, Script, Animation, Screen, Element, Button, Collision, RigidBody, GSplat |
| Framework / Gizmo | ~70% | Translate, rotate, scale gizmos |
| Framework / Assets | ~50% | GLB/glTF loading, 3 resource handlers (texture, container, font) |
| Viz / Overlay | New | ImGui-based digital twin HUD with 3D-anchored labels |

### Known Limitations

- Metal is the primary graphics backend; Vulkan is in early development
- Skeletal animation GPU path not implemented (SkinInstance/MorphInstance are stubs)
- 7 shader features stubbed (light probes, skinning, morphs, spec-gloss, Oren-Nayar, detail normals, displacement)
- Gaussian splatting component registered but not implemented (empty directory)
- No texture streaming or progressive mip-level loading

## Attribution

This project is a C++ port of the [PlayCanvas engine](https://github.com/playcanvas/engine), which is licensed under the MIT License. See the [NOTICE](NOTICE) file for full attribution details.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the full text.
