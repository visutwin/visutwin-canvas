# VisuTwin Canvas Examples

150 example applications, each a small self-contained program built on the
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
| anisotropy-strength | materials/anisotropy-strength | Khronos AnisotropyStrengthTest: strength from 0 to 1 |
| anisotropy-rotation | materials/anisotropy-rotation | Khronos AnisotropyRotationTest: the anisotropy direction turned |
| anisotropy-disc | materials/anisotropy-disc | Khronos AnisotropyDiscTest: a direction map spun round a disc |
| anisotropy-lamp | materials/anisotropy-lamp | Khronos AnisotropyBarnLamp with its anisotropy maps |
| dispersion | materials/dispersion | Khronos DispersionTest: dispersion against IOR over a checker |
| transmission-roughness | materials/transmission-roughness | Khronos TransmissionRoughnessTest under the grabbed scene colour |
| refraction | materials/material-refraction | Env-atlas and dynamic grab-pass refraction |
| parallax-mapping | materials/parallax-mapping | Parallax occlusion mapping in a brick room |
| custom-shader | shaders/shader-toon | ShaderMaterial toon shader (MSL + GLSL) over the statue |
| shader-material | shaders/shader-material | A textured box through a Slang ShaderMaterial |
| shader-wobble | shaders/shader-wobble | The statue wobbling in a custom vertex shader |
| shader-burn | shaders/shader-burn | The statue burning away against a noise threshold |
| clipping-planes | graphics/clipping-planes | A turbofan cut away by clipping planes, its section capped by red back faces |
| mesh-decals | graphics/mesh-decals | Decals stamped by a bouncing ball |
| material-physical | materials/material-physical | A 5x5 grid of spheres, metalness against gloss, with text labels |
| material-translucent-specular | materials/material-translucent-specular | Blended spheres whose specular survives their opacity (opacityFadesSpecular off) |
| material-transparency | materials/material-transparency | Alpha blending against six opacity dither patterns |
| normals-and-tangents | materials/normals-and-tangents | Khronos NormalTangentTest: a normal map read through the mesh's own tangents |
| specular | test/specular | Khronos SpecularTest: KHR_materials_specular factors against its two maps |
| attenuation | test/attenuation | Khronos AttenuationTest: transmissive slabs absorbing by attenuation colour and distance |
| primitive-mode | test/primitive-mode | Khronos PrimitiveModeNormalsTest: points, lines and triangles with and without normals |
| texture-basis | graphics/texture-basis | A torus with Basis Universal (KTX2) colour, gloss and packed normal maps |
| text-markup | test/text-markup | The same texts with and without markup, which must match |

### Lighting & shadows

| Example | Upstream | Description |
|---------|----------|-------------|
| lights | lights-and-shadows/lights | Spot, omni and directional lights with cookies and shadows |
| area-light | lights-and-shadows/area-lights | LTC area lights (rect / sphere / disk) |
| shadow-cascades | lights-and-shadows/shadow-cascades | Cascaded shadow maps over a low-poly terrain |
| clustered-lighting | lights-and-shadows/clustered-lighting | 46 local lights via the 3D cluster grid |
| clustered-spot-shadows | lights-and-shadows/clustered-spot-shadows | Ten shadow-casting cookie spots through the shadow atlas |
| clustered-light-cookies | lights-and-shadows/clustered-light-cookies | Four spot lights with animated cookies through the clustered cookie atlas |
| clustered-omni-shadows | lights-and-shadows/clustered-omni-shadows | Shadowed omni lights with cubemap cookies in the shadow atlas |
| clustered-area-lights | lights-and-shadows/clustered-area-lights | Rect, disk and sphere area lights through the cluster grid |
| lights-non-clustered | lights-and-shadows/lights-non-clustered | Spot, omni and directional lights without clustering |
| light-physical-units | lights-and-shadows/light-physical-units | glTF punctual lights in physical units, with exposure |
| shadow-catcher | lights-and-shadows/shadow-catcher | A statue under an HDR sky, its shadow caught on an invisible ground |
| shadow-soft | lights-and-shadows/shadow-soft | PCSS soft directional shadows over a terrain with drifting clouds |
| shadow-bounds | lights-and-shadows/shadow-bounds | A directional shadow fitted to a box around a bouncing sphere, against one fitted to the camera frustum |
| lights-baked | lights-and-shadows/lights-baked | Baked omni, spot and directional lights over a grid of primitives |
| pcss-dither | graphics/dithered-transparency | Blend and dither strengths decoupled, dithered shadows |
| pcss-local | test/contact-hardening-shadows | PCSS contact-hardening shadows from local lights |
| lightmap-bake | lights-and-shadows/lights-baked-a-o | CPU-baked lightmaps with shadows and AO |
| lightmap-sources | test/lightmap-sources | A material lightmap vs a mesh instance's own bake, and which wins |
| two-sided-lighting | test/two-sided-lighting | Back faces of double-sided materials lit from the viewer's side |
| light-masks | test/light-masks | Which lights reach lightmapped, dynamic and baked-only geometry |
| opacity | test/opacity | Opacity map, alpha test and five blend types on emissive boxes |
| material-texture-transforms | test/material-texture-transforms | Diffuse, opacity and emissive maps under their own UV transforms |

### Reflections & environment

| Example | Upstream | Description |
|---------|----------|-------------|
| reflection-probe-dynamic | graphics/reflection-cubemap | Live cubemap capture and its reprojections |
| reflection-planar | graphics/reflection-planar | A mirror ground sampling a reflection camera's texture by screen position |
| reflection-box | graphics/reflection-box | A room reflecting a live probe through per-material, box-projected environment atlases |
| material-instance-overrides | test/material-instance-overrides | Spheres sharing one material, each overriding its uniforms per mesh instance |
| reflection-planar-blurred | graphics/reflection-planar-blurred | Planar reflections blurred with height |
| procedural-sky | graphics/procedural-sky | Laboratory in dunes under a time-of-day sun |
| sky | graphics/sky | Dome, infinite and box skies from HDRIs, with colour enhance |
| grab-pass | shaders/grab-pass | A frosted glass pane refracting the grabbed scene colour through a custom shader |

### Post-processing

| Example | Upstream | Description |
|---------|----------|-------------|
| post-processing | camera-frame/post-processing | The camera frame's compose chain |
| taa | camera-frame/taa | Temporal anti-aliasing over the PBR house |
| auto-exposure | camera-frame/auto-exposure | The exposure adapting to the brightness of the screen, through De Dust 2's courtyards and tunnels, with its histogram debug view |
| anti-aliasing | camera-frame/anti-aliasing | SMAA (a resolve-stage CameraFrameEffect), TAA and MSAA compared over the PBR house and a sign of thin bars |
| depth-of-field | camera-frame/depth-of-field | Bokeh depth of field over an apartment interior |
| ambient-occlusion | camera-frame/ambient-occlusion | Screen-space ambient occlusion in the laboratory |
| volumetric-fog-local-lights | camera-frame/volumetric-fog-local-lights | Height fog lit by shadowed spot and omni lights over a terrain |
| volumetric-fog-shafts | camera-frame/volumetric-fog-shafts | Light shafts from shadowed spot lights through volumetric fog |
| ink-outline | camera-frame/ink-outline | A CameraFrameEffect with its own pass: ink lines drawn between the opaque and transparent layers |
| custom-compose-shader | camera-frame/custom-compose-shader | A CameraFrameEffect adding a pixelation to the compose pass at the LDR slot |
| hdr | camera-frame/hdr | HDR bloom over a neon sign, a colour LUT, and the bloom debug view |
| volumetric-fog | camera-frame/volumetric-fog | Amber height fog with long shafts from a low sun through the terrain and clouds |
| ambient-occlusion-davinci | — (original scene) | SSAO over the da Vinci workshop with the colour finishing chain |
| edge-detect | compute/edge-detect | Compute-shader Sobel filter over an offscreen render |
| outlines-colored | graphics/outlines-colored | Coloured outlines on laboratory objects under SSAO |
| model-outline | graphics/model-outline | An outline around one of four primitives |
| taa-alpha | test/taa-alpha | A camera frame with TAA keeping the alpha of its own render target |
| ground-fog | shaders/ground-fog | A soft fog plane over a terrain, faded against the scene depth |

### Animation & geometry

| Example | Upstream | Description |
|---------|----------|-------------|
| anim-stategraph | animation/locomotion | Anim state graph with a 1D locomotion blend tree |
| blend-trees-1d | animation/blend-trees-1d | 1D animation blend tree between idle and a dance |
| blend-trees-2d | animation/blend-trees-2d-cartesian | 2D-cartesian animation blend tree |
| layer-masks | animation/layer-masks | An additive upper-body layer masked to a subtree, the skeleton coloured by layer |
| mesh-morph | graphics/mesh-morph | Procedural morph targets driven by sine weights |
| instancing-basic | graphics/instancing-basic | 1000 hardware-instanced cylinders |
| instancing-glb | graphics/instancing-glb | A glTF using EXT_mesh_gpu_instancing |
| dynamic-batching | graphics/batching-dynamic | 500 moving primitives in one dynamic batch group |
| wide-line | graphics/wide-line | Instanced polyline with per-point width and colour |
| mesh-morph-many | graphics/mesh-morph-many | Khronos MorphStressTest driven by eight morph targets |
| wide-lines-dynamic | graphics/wide-lines-dynamic | A globe-scale network of blooming world-width lines rewritten every frame |
| wide-lines-styles | graphics/wide-lines-styles | Eight rows of wide-line styles: caps, joins, dashes and widths |
| shapes | graphics/shapes | Every built-in primitive under one material |
| hierarchy | graphics/hierarchy | 5460 entities in a six-level hierarchy of boxes and spheres |
| model-textured-box | graphics/model-textured-box | A tumbling textured box lit by a circling red omni light |
| mesh-generation | graphics/mesh-generation | A grid mesh rebuilt every frame, rising under four circling lights |
| mesh-deformation | graphics/mesh-deformation | The statue swaying as its vertices are displaced on the CPU every frame |
| instancing-custom | graphics/instancing-custom | 3000 instanced spheres pulled toward a bobbing sphere |

### Gaussian splatting & particles

| Example | Upstream | Description |
|---------|----------|-------------|
| gsplat | gaussian-splatting/simple | Gaussian splat on a shadow-receiving ground |
| gsplat-spherical-harmonics | gaussian-splatting/spherical-harmonics | A splat with view-dependent colour from SH bands, casting its shadow |
| gsplat-shadows | gaussian-splatting/shadows | Two splat models casting onto a shadow catcher under turning lights |
| gsplat-shadow-orientation | test/gsplat-shadow-orientation | Thin splats whose shadows must lie parallel to them |
| particles | compute/particles | 1M compute-simulated particles colliding with spheres |
| particles-anim-index | graphics/particles-anim-index | Sprite-sheet animation rows selected by animIndex |
| particles-random-sprites | graphics/particles-random-sprites | Coin and bonus sprite sheets, each particle picking a random animation |

### Scene, camera & loading

| Example | Upstream | Description |
|---------|----------|-------------|
| orbit | camera/orbit | Orbit camera controls around the statue |
| hello-world | misc/hello-world | A spinning box |
| model-asset | graphics/model-asset | The statue and a small clone, lit by an omni light |
| render-asset | graphics/render-asset | Cubes and the statue instanced from containers, turning |
| texture-read | test/texture-read | Texture::read round trips for R8, RG8 and RGBA8 |
| texture-copy | test/texture-copy | Texture::copy: whole levels, a mip and a sub-rectangle, read back |
| fly | camera/fly | Fly camera controls around the statue |
| multi | camera/multi | One camera with orbit, fly and pan all enabled |
| glb-loader | loaders/glb | A GLB carrying meshes, lights and cameras |
| draco-glb | loaders/draco-glb | A Draco-compressed glTF |
| obj-loader | loaders/obj | An OBJ file through the OBJ parser |
| layers | graphics/layers | X-ray, character and front layers |
| multi-view | graphics/multi-view | One board through three cameras and viewports |
| render-to-texture | render-targets/render-to-texture | A camera rendering into a texture shown in the scene |
| texture-array-impostors | render-targets/texture-array-impostors | A statue baked from 64 directions into texture-array layers, drawn as thousands of instanced impostors |
| volume-texture | render-targets/volume-texture | Colourful fog in a volume texture raymarched over a terrain, static or rendered into its depth slices, with mipmaps |
| render-pass-procedural-texture | render-targets/render-pass-procedural-texture | Two frame-start quad passes generating a material's tiling colour and normal maps |
| render-pass-camera | render-targets/render-pass-camera | A camera's own render passes: a forward pass into a texture, then a tint quad to the screen |
| render-pass-shader-quad | render-targets/render-pass-shader-quad | Glowing trails painted by fade and brush quad passes, the emissive map of a cube |

### Debug

| Example | Upstream | Description |
|---------|----------|-------------|
| wire-shapes | debug/wire-shapes | Every WireRenderer shape in a wall of animated cells |
| lines | debug/lines | A packed wave field, a ring, fading spokes and comet trails as immediate lines |
| frustum-culling | debug/frustum-culling | Objects culled against an observer camera, with bounds, arrows and its frustum drawn |

### Physics

| Example | Upstream | Description |
|---------|----------|-------------|
| falling-shapes | physics/falling-shapes | Shapes dropped onto a floor through the Jolt world |
| physics-joints | physics/joints | Hinge, ball, slider, 6-DOF and breakable fixed joints |
| raycast | physics/raycast | raycastFirst and raycastAll through two rows of shapes |
| offset-collision | physics/offset-collision | Balls bouncing off a head collider offset from the character's feet |
| compound-collision | physics/compound-collision | Chairs falling as compound bodies of a seat, a back and four legs |
| mesh-collider-scaling | physics/mesh-collider-scaling | Spheres dropped through scaled torus mesh colliders, one pulsing |

### Interaction & UI

| Example | Upstream | Description |
|---------|----------|-------------|
| area-picker | graphics/area-picker | Picker area queries highlighting primitives |
| keyboard | input/keyboard | The statue turned by the arrow keys |
| mouse | input/mouse | The statue turned by dragging with the mouse |
| gamepad | input/gamepad | The statue turned by a gamepad's D-pad |
| transform-translate | gizmos/transform-translate | Translate gizmo |
| transform-rotate | gizmos/transform-rotate | Rotate gizmo |
| transform-scale | gizmos/transform-scale | Scale gizmo |
| bounding-box | gizmos/bounding-box | Bounding box gizmo: corner brackets, faint where hidden |
| ui-text | user-interface/text | Screen-space text elements |
| world-to-screen | user-interface/world-to-screen | Screen-space UI anchored to world positions |
