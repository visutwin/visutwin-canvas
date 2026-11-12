# VisuTwin Canvas Examples

197 example applications, each a small self-contained program built on the
engine. All but three are ports of [PlayCanvas](https://playcanvas.com/) engine
examples, so they can be compared side by side with upstream: the file header names
the upstream example and lists every place the port could not follow it. The
exceptions, `ambient-occlusion-davinci`, `order-independent-transparency` and
`visibility-range`, are
original scenes and say so in their headers; a new scene belongs here only where
upstream has no counterpart.

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
visutwin_add_example(materials clear-coat)   # builds src/materials/clear-coat.cpp
```

The helper compiles the example together with the shared `ExampleApp` host and
the `CameraControls` script, links the engine, and bundles it for macOS. The
category and name are upstream's (see the catalog below): they place the source,
the built program (`examples/<category>/<name>.app`) and the target
(`visutwin-<category>-<name>`), and give the window its title, written as
upstream's example browser writes it ("Materials › Clear Coat").

## Structure

Every example derives from **`ExampleApp`** (`exampleApp.h`), which owns the
window, the graphics device, the engine and the frame loop, so an example file
contains only the scene it exists to demonstrate:

```cpp
class MyExample final: public ExampleApp
{
public:
    MyExample(): ExampleApp({}) {}   // or ExampleApp({.width = 1280}) ...

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

The executables land in `build-examples/examples/<category>/`, each as a macOS app bundle:

```bash
open build-examples/examples/camera-frame/taa.app
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

The examples are grouped and named as upstream's example browser groups and names them:
`<category>/<name>` is the folder of the upstream example each one ports, under upstream's
`examples/src/examples/`, and the source here is `src/<category>/<name>.cpp`. The three
original scenes, marked *original*, sit in the category nearest to what they show.

### Animation (`animation`)

| Example | Description |
|---------|-------------|
| blend-trees-1d | 1D animation blend tree between idle and a dance |
| blend-trees-2d-cartesian | 2D-cartesian animation blend tree |
| blend-trees-2d-directional | The character driven by a 2D directional blend tree |
| component-properties | Animation curves driving light and camera properties |
| crowd | 20 to 500 wandering, independently animated characters, a CPU profiling scene for skinned animation |
| events | A walk track firing foot-step events that light the floor boxes |
| layer-masks | An additive upper-body layer masked to a subtree, the skeleton coloured by layer |
| locomotion | Anim state graph with a 1D locomotion blend tree |
| tween | The eleven easing functions of tween.js, one card each |

### Camera (`camera`)

| Example | Description |
|---------|-------------|
| fly | Fly camera controls around the statue |
| multi | One camera with orbit, fly and pan all enabled |
| orbit | Orbit camera controls around the statue |

### Camera Frame (`camera-frame`)

| Example | Description |
|---------|-------------|
| ambient-occlusion | Screen-space ambient occlusion in the laboratory |
| ambient-occlusion-davinci | *original* — SSAO over the da Vinci workshop with the colour finishing chain |
| anti-aliasing | SMAA (a resolve-stage CameraFrameEffect), TAA and MSAA compared over the PBR house and a sign of thin bars |
| auto-exposure | The exposure adapting to the brightness of the screen, through De Dust 2's courtyards and tunnels, with its histogram debug view |
| custom-compose-shader | A CameraFrameEffect adding a pixelation to the compose pass at the LDR slot |
| depth-of-field | Bokeh depth of field over an apartment interior |
| dynamic-sky | A day and night cycle: atmosphere, stars, moon and volumetric clouds |
| hdr | HDR bloom over a neon sign, a colour LUT, and the bloom debug view |
| ink-outline | A CameraFrameEffect with its own pass: ink lines drawn between the opaque and transparent layers |
| lens-flare | A lens flare following the dynamic sky's sun |
| post-processing | The camera frame's compose chain |
| taa | Temporal anti-aliasing over the PBR house |
| volumetric-fog | Amber height fog with long shafts from a low sun through the terrain and clouds |
| volumetric-fog-local-lights | Height fog lit by shadowed spot and omni lights over a terrain |
| volumetric-fog-shafts | Light shafts from shadowed spot lights through volumetric fog |

### Compute (`compute`)

| Example | Description |
|---------|-------------|
| edge-detect | Compute-shader Sobel filter over an offscreen render |
| histogram | Luminance histogram of the grabbed scene colour, built by a compute pass in the camera's after-passes |
| indirect-dispatch | A compute shader sizing the dispatches that follow it |
| indirect-draw | A compute shader writing an indirect draw's arguments every frame |
| particles | 1M compute-simulated particles colliding with spheres |
| scene-maps | A compute scanner pulse reading the camera's scene depth and colour maps after it renders |
| texture-gen | Two trefoil knots painted with textures a compute shader generates |
| vertex-update | A compute shader rewriting a blob's vertex buffer every frame |

### Debug (`debug`)

| Example | Description |
|---------|-------------|
| frustum-culling | Objects culled against an observer camera, with bounds, arrows and its frustum drawn |
| grid | A reference grid showing every setting of the Grid |
| lines | A packed wave field, a ring, fading spokes and comet trails as immediate lines |
| wire-shapes | Every WireRenderer shape in a wall of animated cells |

### Demos (`demos`)

| Example | Description |
|---------|-------------|
| car-showcase | A car configurator in a baked showroom |

### Gaussian Splatting (`gaussian-splatting`)

| Example | Description |
|---------|-------------|
| simple | Gaussian splat on a shadow-receiving ground |
| simple-sh | A splat with view-dependent colour from SH bands, casting its shadow |

### Gaussian Splatting Fx (`gaussian-splatting-fx`)

| Example | Description |
|---------|-------------|
| shadows | Two splat models casting onto a shadow catcher under turning lights |

### Gizmos (`gizmos`)

| Example | Description |
|---------|-------------|
| bounding-box | Bounding box gizmo: corner brackets, faint where hidden |
| camera | A CameraGizmo drawing a second camera's frustum |
| icon | Clickable icons for lights, cameras and other invisible entities |
| light | A LightGizmo on a spot light, with its area shape |
| shape | The box, sphere, cone, capsule and cylinder shape gizmos |
| transform-rotate | Rotate gizmo |
| transform-scale | Scale gizmo |
| transform-translate | Translate gizmo |

### Graphics (`graphics`)

| Example | Description |
|---------|-------------|
| area-picker | Picker area queries highlighting primitives |
| batching-dynamic | 500 moving primitives in one dynamic batch group |
| clipping-planes | A turbofan cut away by clipping planes, its section capped by red back faces |
| dithered-transparency | Glass panels blended and dithered with alphaDither; overlapping meshes offset the pattern, so their dithered shadows add up |
| hierarchy | 5460 entities in a six-level hierarchy of boxes and spheres |
| instancing-basic | 1000 hardware-instanced cylinders |
| instancing-custom | 3000 instanced spheres pulled toward a bobbing sphere |
| instancing-glb | A glTF using EXT_mesh_gpu_instancing |
| instancing-gooch | Gooch shading on instanced, skinned, morphed and static geometry |
| mesh-decals | Decals stamped by a bouncing ball |
| mesh-deformation | The statue swaying as its vertices are displaced on the CPU every frame |
| mesh-generation | A grid mesh rebuilt every frame, rising under four circling lights |
| mesh-morph | Procedural morph targets driven by sine weights |
| mesh-morph-many | Khronos MorphStressTest driven by eight morph targets |
| model-asset | The statue and a small clone, lit by an omni light |
| model-textured-box | A tumbling textured box lit by a circling red omni light |
| multi-view | One board through three cameras and viewports |
| order-independent-transparency | *original* — Weighted blended order-independent transparency: interpenetrating transparent panes and spheres around an opaque box, against per-object sorting |
| outlines-colored | Coloured outlines on laboratory objects under SSAO |
| procedural-sky | Laboratory in dunes under a time-of-day sun |
| reflection-box | A room reflecting a live probe through per-material, box-projected environment atlases |
| reflection-cubemap | Live cubemap capture and its reprojections |
| reflection-planar | A mirror ground sampling a reflection camera's texture by screen position |
| reflection-planar-blurred | Planar reflections blurred with height |
| render-asset | Cubes and the statue instanced from containers, turning |
| shapes | Every built-in primitive under one material |
| sky | Dome, infinite and box skies from HDRIs, with colour enhance |
| texture-basis | A torus with Basis Universal (KTX2) colour, gloss and packed normal maps |
| visibility-range | *original* — Levels of detail by camera distance: a field of three-level markers crossfading by complementary dithers as the camera dollies through, shadows included |
| water | A configurable water surface around a tropical island |
| wide-line | Instanced polyline with per-point width and colour |
| wide-lines-dynamic | A globe-scale network of blooming world-width lines rewritten every frame |
| wide-lines-styles | Eight rows of wide-line styles: caps, joins, dashes and widths |

### Graphics Advanced (`graphics-advanced`)

| Example | Description |
|---------|-------------|
| layers | X-ray, character and front layers |
| multi-draw | A terrain of 1,600 patches drawn as sub-draws of one mesh |
| multi-draw-instanced | Sub-draws of one instanced mesh, each with its own instance range |
| portal | A portal made with the stencil buffer |

### Input (`input`)

| Example | Description |
|---------|-------------|
| gamepad | The statue turned by a gamepad's D-pad |
| keyboard | The statue turned by the arrow keys |
| mouse | The statue turned by dragging with the mouse |

### Lights and Shadows (`lights-and-shadows`)

| Example | Description |
|---------|-------------|
| area-lights | LTC area lights (rect / sphere / disk) |
| clustered-area-lights | Rect, disk and sphere area lights through the cluster grid |
| clustered-light-cookies | Four spot lights with animated cookies through the clustered cookie atlas |
| clustered-lighting | 46 local lights via the 3D cluster grid |
| clustered-omni-shadows | Shadowed omni lights with cubemap cookies in the shadow atlas |
| clustered-spot-shadows | Ten shadow-casting cookie spots through the shadow atlas |
| light-physical-units | glTF punctual lights in physical units, with exposure |
| lights | Spot, omni and directional lights with cookies and shadows |
| lights-baked | Baked omni, spot and directional lights over a grid of primitives |
| lights-baked-a-o | CPU-baked lightmaps with shadows and AO |
| lights-non-clustered | Spot, omni and directional lights without clustering |
| shadow-bounds | A directional shadow fitted to a box around a bouncing sphere, against one fitted to the camera frustum |
| shadow-cascades | Cascaded shadow maps over a low-poly terrain |
| shadow-catcher | A statue under an HDR sky, its shadow caught on an invisible ground |
| shadow-soft | PCSS soft directional shadows over a terrain with drifting clouds |

### Loaders (`loaders`)

| Example | Description |
|---------|-------------|
| draco-glb | A Draco-compressed glTF |
| glb | A GLB carrying meshes, lights and cameras |
| obj | An OBJ file through the OBJ parser |

### Materials (`materials`)

| Example | Description |
|---------|-------------|
| anisotropy-disc | Khronos AnisotropyDiscTest: a direction map spun round a disc |
| anisotropy-lamp | Khronos AnisotropyBarnLamp with its anisotropy maps |
| anisotropy-rotation | Khronos AnisotropyRotationTest: the anisotropy direction turned |
| anisotropy-strength | Khronos AnisotropyStrengthTest: strength from 0 to 1 |
| clear-coat | Clearcoat dual-specular layer on the Khronos test asset |
| diffuse-transmission | Khronos' DiffuseTransmissionTest: light passing through thin surfaces |
| dispersion | Khronos DispersionTest: dispersion against IOR over a checker |
| material-anisotropic | Anisotropy × gloss grid of metallic spheres |
| material-physical | A 5x5 grid of spheres, metalness against gloss, with text labels |
| material-refraction | Env-atlas and dynamic grab-pass refraction |
| material-translucent-specular | Blended spheres whose specular survives their opacity (opacityFadesSpecular off) |
| material-transparency | Alpha blending against six opacity dither patterns |
| normals-and-tangents | Khronos NormalTangentTest: a normal map read through the mesh's own tangents |
| parallax-mapping | Parallax occlusion mapping in a brick room |
| transmission-roughness | Khronos TransmissionRoughnessTest under the grabbed scene colour |

### Misc (`misc`)

| Example | Description |
|---------|-------------|
| annotations | Numbered hotspots on a jet, with tooltips |
| hello-world | A spinning box |

### Particles (`particles`)

| Example | Description |
|---------|-------------|
| anim-index | Sprite-sheet animation rows selected by animIndex |
| impact-effects | Bullet hits, explosions and magic bursts of one-shot emitters, lights and decals, raycast against the reflecting room |
| mesh | A fountain of opaque, lit torus mesh particles |
| random-sprites | Coin and bonus sprite sheets, each particle picking a random animation |
| snow | Snow falling through a moonlit night scene |
| spark | A fountain of sparks under velocity curves |

### Physics (`physics`)

| Example | Description |
|---------|-------------|
| compound-collision | Chairs falling as compound bodies of a seat, a back and four legs |
| falling-shapes | Shapes dropped onto a floor through the Jolt world |
| joints | Hinge, ball, slider, 6-DOF and breakable fixed joints |
| mesh-collider-scaling | Spheres dropped through scaled torus mesh colliders, one pulsing |
| offset-collision | Balls bouncing off a head collider offset from the character's feet |
| raycast | raycastFirst and raycastAll through two rows of shapes |

### Render Targets (`render-targets`)

| Example | Description |
|---------|-------------|
| custom-msaa-resolve | Multisampled textures read per sample by a custom resolve |
| integer-textures | A falling-sand simulation in an unsigned integer texture |
| msaa-depth-fog | Fog per sample from a multisampled depth texture |
| multi-render-targets | One pass writing colour, normal and depth-like attachments at once |
| render-pass-camera | A camera's own render passes: a forward pass into a texture, then a tint quad to the screen |
| render-pass-procedural-texture | Two frame-start quad passes generating a material's tiling colour and normal maps |
| render-pass-shader-quad | Glowing trails painted by fade and brush quad passes, the emissive map of a cube |
| render-to-texture | A camera rendering into a texture shown in the scene |
| texture-array-impostors | A statue baked from 64 directions into texture-array layers, drawn as thousands of instanced impostors |
| volume-texture | Colourful fog in a volume texture raymarched over a terrain, static or rendered into its depth slices, with mipmaps |

### Shaders (`shaders`)

| Example | Description |
|---------|-------------|
| grab-pass | A frosted glass pane refracting the grabbed scene colour through a custom shader |
| ground-fog | A soft fog plane over a terrain, faded against the scene depth |
| shader-burn | The statue burning away against a noise threshold |
| shader-material | A textured box through a Slang ShaderMaterial |
| shader-toon | ShaderMaterial toon shader (MSL + GLSL) over the statue |
| shader-wobble | The statue wobbling in a custom vertex shader |

### Test (`test`)

| Example | Description |
|---------|-------------|
| alpha-to-coverage | Material::setAlphaToCoverage over an opacity ramp |
| attenuation | Khronos AttenuationTest: transmissive slabs absorbing by attenuation colour and distance |
| contact-hardening-shadows | PCSS contact-hardening shadows from local lights |
| dual-source-blending | A second fragment colour read as a blend factor |
| gsplat-shadow-orientation | Thin splats whose shadows must lie parallel to them |
| independent-blending | A blend state per colour attachment |
| light-masks | Which lights reach lightmapped, dynamic and baked-only geometry |
| lightmap-sources | A material lightmap vs a mesh instance's own bake, and which wins |
| material-instance-overrides | Spheres sharing one material, each overriding its uniforms per mesh instance |
| material-texture-transforms | Diffuse, opacity and emissive maps under their own UV transforms |
| opacity | Opacity map, alpha test and five blend types on emissive boxes |
| primitive-mode | Khronos PrimitiveModeNormalsTest: points, lines and triangles with and without normals |
| render-target-orientation | Image orientation through every path that produces a texture |
| scene-depth-read | SceneDepthReader reading back a camera's depth |
| specular | Khronos SpecularTest: KHR_materials_specular factors against its two maps |
| taa-alpha | A camera frame with TAA keeping the alpha of its own render target |
| text-markup | The same texts with and without markup, which must match |
| texture-copy | Texture::copy: whole levels, a mip and a sub-rectangle, read back |
| texture-formats | One image as PNG, DDS and KTX2 |
| texture-read | Texture::read round trips for R8, RG8 and RGBA8 |
| two-sided-lighting | Back faces of double-sided materials lit from the viewer's side |

### User Interface (`user-interface`)

| Example | Description |
|---------|-------------|
| anchors | A chat window laid out with anchors |
| buttons | A main menu built from button components |
| common-widgets | A settings screen from the common widgets |
| custom-shader | Ability cooldowns drawn by a shader material on an image element |
| drag-and-drop | Equipping gear by drag and drop |
| image-fit | A level select whose art comes in four shapes |
| input-events | A map dropping markers under a HUD that stops the clicks it takes |
| layout-group | A backpack screen built from three layout groups |
| masking | A profile card using masks three ways |
| panel | A quest dialog, toasts and a log from sliced sprites |
| particle-system | Screen-space particles in the UI's draw order |
| render-to-image | A character card with a live 3D portrait |
| screen-scaling | A mobile HUD designed at a reference resolution |
| scroll-view | A leaderboard of 50 players in a scroll view |
| text | Screen-space text elements |
| text-auto-font-size | Name badges whose font shrinks to fit |
| text-justify | Justified and aligned text on a codex page |
| text-localization | A shop card in four languages |
| text-markup | An adventure log with coloured, outlined markup runs |
| text-typewriter | A dialog box revealing each line letter by letter |
| world-to-screen | Screen-space UI anchored to world positions |
| world-ui | Screens placed in the world, in a hangar |
