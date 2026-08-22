# VisuTwin Canvas Examples

44 example applications, each a small self-contained program built on the
engine. Most are ports of their [PlayCanvas](https://playcanvas.com/) engine
counterparts, so they can be compared side by side with upstream.

## Building

The examples are not part of the default build — `VISUTWIN_BUILD_EXAMPLES`
defaults to `OFF` so the normal CMake/CLion project stays focused on the engine,
tools, and tests. The `examples` preset enables them in their own build
directory:

```bash
cmake --preset examples
cmake --build build-examples
```

The `examples/` directory is also a standalone CMake project, so it can be
configured on its own against an already-built engine.

Each example is one line in `CMakeLists.txt`:

```cmake
visutwin_add_example(clearcoat)   # builds src/clearcoat-example.cpp
```

The helper compiles the example together with the shared `ExampleApp` host and
the `CameraControls` script, links the engine, and bundles it for macOS.

## Structure

Every example derives from **`ExampleApp`** (`exampleApp.h`), which owns the
window, the graphics device, the engine and the frame loop, so an example file
contains only the scene it exists to demonstrate:

```cpp
class MyExample final: public ExampleApp
{
public:
    MyExample(): ExampleApp({.title = "My Example"}) {}

protected:
    bool create() override { ...build the scene...; return true; }
    void update(float dt) override { ...per-frame...; }
};

VISUTWIN_EXAMPLE_MAIN(MyExample)
```

`run()` calls the virtual hooks in order: `configure()` (extra component
systems) → `create()` → [`update()` → `preRender()` → `postRender()`]* →
`destroy()`. Only `create()` is mandatory. The class also carries the small
pieces of setup that repeat — `assetPath()`, `createCamera()`,
`addOrbitControls()`, `createDirectionalLight()`, `createPrimitive()` and
`entityBounds()`.

Backend selection lives there too: `ExampleApp` resolves Metal vs Vulkan before
creating the window (each needs different window flags) and hosts metal-cpp's
`*_PRIVATE_IMPLEMENTATION` translation unit, so no example carries a backend
`#ifdef` of its own and every one of them builds for either backend.

## Running

The executables land in `build-examples/examples/`, each as a macOS app bundle:

```bash
open build-examples/examples/visutwin-taa.app
```

Every example honours the engine's runtime environment variables
(`VISUTWIN_BACKEND`, `VISUTWIN_SCREENSHOT`, `VISUTWIN_SCREENSHOT_FRAME`) — see
the [main README](../README.md#runtime-environment-variables). A shared
`cameraControls` utility provides orbit, fly, focus, and auto far-clip camera
modes across examples.

## Assets

Examples expect asset files in the top-level `assets/` directory. Some generate
their test assets procedurally; others need models, textures, or HDR environment
maps you provide.

Recommended free asset sources:
- [Poly Haven](https://polyhaven.com/) (CC0 HDR environment maps and textures)
- [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) (CC-BY-4.0 test models)

## Catalog

### Materials & shading

| Example | Description |
|---------|-------------|
| clearcoat | Clearcoat dual-specular layer showcase |
| anisotropy | Anisotropic specular highlights |
| area-light | LTC area lights (rect / disk / sphere) |
| refraction | Dynamic grab-pass refraction + dispersion + volume |
| custom-shader | ShaderMaterial toon shader (MSL + GLSL) over the statue model |
| shader-chunks | ShaderChunks registry overrides (global + per-material) |
| mesh-decals | Decal projection |

### Lighting & shadows

| Example | Description |
|---------|-------------|
| lights | Directional / omni / spot / area light types |
| shadow-cascades | Cascaded shadow maps |
| clustered-lighting | 46 local lights (30 omni + 16 spot) via the 3D cluster grid |
| pcss-dither | Dithered transparency: blend and dither strengths decoupled, dithered shadows |
| pcss-local | Spot/omni PCSS contact-hardening shadows |
| lightmap-bake | CPU-baked lightmap (soft shadows + AO) applied at UV1 |

### Reflections & environment

| Example | Description |
|---------|-------------|
| reflection-probe | Box-projected cubemap reflection probe |
| reflection-probe-dynamic | Runtime scene-capture reflection probe (live cubemap) |
| reflection-planar-blurred | Planar reflections with blur |
| procedural-sky | Laboratory in dunes under a time-of-day sun (upstream procedural-sky scene) |

### Post-processing

| Example | Description |
|---------|-------------|
| post-processing | Compose chain: bloom, DOF, vignette, color grading, fringing |
| taa | Temporal anti-aliasing with a PBR scene |
| depth-of-field | Bokeh depth-of-field over an apartment interior, interactive focus |
| ambient-occlusion | Screen-space ambient occlusion (PlayCanvas port, laboratory scene) |
| ambient-occlusion-davinci | Screen-space ambient occlusion (da Vinci workshop + colour LUT) |
| edge-detect | Post-processing edge detection |

### Animation & geometry

| Example | Description |
|---------|-------------|
| anim-stategraph | Animation state graph with blend trees |
| blend-trees-2d | 2D-cartesian animation blend tree |
| morph-anim | Morph-weight animation + skinned culling |
| instancing-basic | GPU instancing |
| dynamic-batching | Dynamic mesh batching of many shared-material objects |

### Gaussian splatting & particles

| Example | Description |
|---------|-------------|
| gsplat | Gaussian splatting (classic path) |
| gsplat-tier2 | Splatting with view-dependent SH + compressed PLY |
| particles | 1M particles simulated by an app-authored compute shader, colliding with spheres |

### Scene, camera & loading

| Example | Description |
|---------|-------------|
| orbit | Orbital camera with GLB model and environment lighting |
| glb-loader | Loading and rendering GLB models |
| layers | Render layer composition |
| multi-view | Multiple camera viewports |
| render-to-texture | Off-screen rendering |

### Interaction & UI

| Example | Description |
|---------|-------------|
| raycast | Mouse picking via ray casting |
| area-picker | Area selection / picking |
| gizmo-translate | Transform gizmo interaction |
| transform-rotate | Rotate gizmo |
| transform-scale | Scale gizmo |
| ui-text | Screen-space UI text (Screen + Element components) |
| world-to-screen | Screen-space UI with world anchors |
