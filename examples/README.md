# VisuTwin Canvas Examples

47 example applications, each a small self-contained program built on the
engine. All but one are ports of [PlayCanvas](https://playcanvas.com/) engine
examples, so they can be compared side by side with upstream: the file header names
the upstream example and lists every place the port could not follow it. The
exception, `ambient-occlusion-davinci`, is an original scene and says so in its
header; a new scene belongs here only where upstream has no counterpart.

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

The second column is the upstream example each one ports, as
`<category>/<name>` under upstream's `examples/src/examples/`.

### Materials & shading

| Example | Upstream | Description |
|---------|----------|-------------|
| clearcoat | materials/clear-coat | Clearcoat dual-specular layer on the Khronos test asset |
| anisotropy | materials/material-anisotropic | Anisotropy × gloss grid of metallic spheres |
| refraction | materials/material-refraction | Env-atlas and dynamic grab-pass refraction |
| parallax-mapping | materials/parallax-mapping | Parallax occlusion mapping in a brick room |
| custom-shader | shaders/shader-toon | ShaderMaterial toon shader (MSL + GLSL) over the statue |
| mesh-decals | graphics/mesh-decals | Decals stamped by a bouncing ball |

### Lighting & shadows

| Example | Upstream | Description |
|---------|----------|-------------|
| lights | graphics/lights | Spot, omni and directional lights with cookies and shadows |
| area-light | graphics/area-lights | LTC area lights (rect / sphere / disk) |
| shadow-cascades | graphics/shadow-cascades | Cascaded shadow maps over a low-poly terrain |
| clustered-lighting | graphics/clustered-lighting | 46 local lights via the 3D cluster grid |
| clustered-spot-shadows | graphics/clustered-spot-shadows | Ten shadow-casting cookie spots through the shadow atlas |
| pcss-dither | graphics/dithered-transparency | Blend and dither strengths decoupled, dithered shadows |
| pcss-local | test/contact-hardening-shadows | PCSS contact-hardening shadows from local lights |
| lightmap-bake | graphics/lights-baked-a-o | CPU-baked lightmaps with shadows and AO |
| lightmap-sources | test/lightmap-sources | A material lightmap vs a mesh instance's own bake, and which wins |

### Reflections & environment

| Example | Upstream | Description |
|---------|----------|-------------|
| reflection-probe-dynamic | graphics/reflection-cubemap | Live cubemap capture and its reprojections |
| reflection-planar-blurred | graphics/reflection-planar-blurred | Planar reflections blurred with height |
| procedural-sky | graphics/procedural-sky | Laboratory in dunes under a time-of-day sun |

### Post-processing

| Example | Upstream | Description |
|---------|----------|-------------|
| post-processing | graphics/post-processing | The camera frame's compose chain |
| taa | graphics/taa | Temporal anti-aliasing over the PBR house |
| depth-of-field | graphics/depth-of-field | Bokeh depth of field over an apartment interior |
| ambient-occlusion | graphics/ambient-occlusion | Screen-space ambient occlusion in the laboratory |
| ambient-occlusion-davinci | — (original scene) | SSAO over the da Vinci workshop with the colour finishing chain |
| edge-detect | compute/edge-detect | Compute-shader Sobel filter over an offscreen render |

### Animation & geometry

| Example | Upstream | Description |
|---------|----------|-------------|
| anim-stategraph | animation/locomotion | Anim state graph with a 1D locomotion blend tree |
| blend-trees-2d | animation/blend-trees-2d-cartesian | 2D-cartesian animation blend tree |
| mesh-morph | graphics/mesh-morph | Procedural morph targets driven by sine weights |
| instancing-basic | graphics/instancing-basic | 1000 hardware-instanced cylinders |
| dynamic-batching | graphics/batching-dynamic | 500 moving primitives in one dynamic batch group |
| wide-line | graphics/wide-line | Instanced polyline with per-point width and colour |

### Gaussian splatting & particles

| Example | Upstream | Description |
|---------|----------|-------------|
| gsplat | gaussian-splatting/simple | Gaussian splat on a shadow-receiving ground |
| particles | compute/particles | 1M compute-simulated particles colliding with spheres |
| particles-anim-index | graphics/particles-anim-index | Sprite-sheet animation rows selected by animIndex |

### Scene, camera & loading

| Example | Upstream | Description |
|---------|----------|-------------|
| orbit | camera/orbit | Orbit camera controls around the statue |
| glb-loader | loaders/glb | A GLB carrying meshes, lights and cameras |
| layers | graphics/layers | X-ray, character and front layers |
| multi-view | graphics/multi-view | One board through three cameras and viewports |
| render-to-texture | graphics/render-to-texture | A camera rendering into a texture shown in the scene |

### Physics

| Example | Upstream | Description |
|---------|----------|-------------|
| falling-shapes | physics/falling-shapes | Shapes dropped onto a floor through the Jolt world |
| physics-joints | physics/joints | Hinge, ball, slider, 6-DOF and breakable fixed joints |
| raycast | physics/raycast | raycastFirst and raycastAll through two rows of shapes |

### Interaction & UI

| Example | Upstream | Description |
|---------|----------|-------------|
| area-picker | graphics/area-picker | Picker area queries highlighting primitives |
| transform-translate | gizmos/transform-translate | Translate gizmo |
| transform-rotate | gizmos/transform-rotate | Rotate gizmo |
| transform-scale | gizmos/transform-scale | Scale gizmo |
| ui-text | user-interface/text | Screen-space text elements |
| world-to-screen | user-interface/world-to-screen | Screen-space UI anchored to world positions |
