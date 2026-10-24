# Features

A detailed inventory of what VisuTwin Canvas implements today, and how far each
module has been carried. For an overview and getting-started instructions, see
the [README](README.md).

## Rendering & materials
- **Forward PBR renderer** (metalness and specular workflows; normalised Blinn-Phong light specular by default, GGX on request) with multi-light support (directional, point, spot, rectangular/disk/spherical **area lights** via LTC) and a frame-graph pass scheduler
- **StandardMaterial** with clearcoat, anisotropy, sheen, iridescence, transmission, **parallax occlusion mapping** (height base + self-shadowing), **spec-gloss** (KHR_materials_pbrSpecularGlossiness), **Oren-Nayar** diffuse, **detail normals** (reoriented blend), and vertex-stage **displacement mapping**; an **opacity map** with its own channel and UV transform, an **ambient tint**, and blended surfaces that keep their specular (`opacityFadesSpecular` off) under an extra alpha fade
- **Clipping planes** on mesh instances, render components and cameras: up to eight world-space planes per set, clipping what is behind any of them or (intersection mode) only what is behind all, in every pass including picking; a mesh instance's planes clip its shadow too
- **Per-mesh-instance material overrides** (`MeshInstance::setParameter`): instances sharing one material each override its colours and scalars, and follow its other changes
- **Per-material environment atlases** with **box-projected reflections** (a room's probe reflected so it lines up with the walls) and a reflectivity factor; a dynamic **reflection probe** whose cube mips are averaged in linear space
- **Packed (GGGR) normal maps**, as two-channel block-compressed normal maps store them, and **GPU texture copies** (`Texture::copy`: mip levels, cube faces, sub-rectangles)
- **Wide lines**: connected polylines with per-point colour and width, caps, joins and dash patterns (round dash ends included), batched into one instanced draw; positions, colours and widths can be rewritten per frame
- **Rigid-body physics** behind an application-supplied `PhysicsWorld` seam, with a Jolt backend: box/sphere/capsule/cylinder shapes, shapes **offset** from their body and **compound** bodies built from child entities' shapes, static/dynamic/kinematic bodies, forces, impulses, raycasts, and **joints** (fixed, ball, hinge, slider, 6dof) with limits, motors and break impulses
- **Image-based lighting**: environment atlas (GGX/Lambert prefiltered), HDR cubemap skybox, **ambient SH light probes**, and **box-projected cubemap reflection probes** (parallax-corrected local reflections)
- **Dynamic grab-pass refraction** with chromatic **dispersion** and KHR_materials_volume Beer-law attenuation
- **Screen-space reflections**: a per-fragment world-space march against the scene depth and colour grabs, reaching 0.4 of the camera range, with bisection-refined hits, a roughness cone that reads the colour grab's mips, and env/probe fallback; works under the standalone and the camera-frame (post-processing) paths
- **Vertex colors**, **point-size** primitives, **opacity dither** (Bayer8 order-independent transparency), and **lightmap** (UV1) sampling
- **GPU instancing** with per-instance colour (Metal; Vulkan instances take the material colour) and optional per-frame GPU frustum culling (compute-driven indirect draw), plus **dynamic batching** with a bone-index matrix palette
- **MSAA** on the back buffer (4x, on by default, resolved per pass, colour and depth grabs included) and on the offscreen scene target, and **dual-source blending** (`BLENDMODE_SRC1_*`) for custom blend setups

## Lighting & shadows
- **Cascaded shadow maps** (one cascade by default as upstream, up to four PSSM cascades with cross-cascade blending and distance fade); receivers beyond the caster-fitted range are shadowed, so a receive-only ground works; or **shadow bounds**: one directional shadow map fitted to a world box (a subject's), rendered once and shared by every camera, sharp at any camera distance
- **PCF**, **EVSM_16F** (Exponential Variance Shadow Maps: separable Gaussian or box blur, Chebyshev sampling, caster-AABB depth tightening), and **PCSS** contact-hardening soft shadows for directional lights
- **Spot/point** 2D depth maps and **omnidirectional cubemap** shadows, with PCSS also supported on spot/omni local lights
- **Clustered lighting** for many-light scenes with a packed **shadow atlas** whose resolution can change at runtime, plus a **shadow catcher** material for compositing
- **Physical light units** (photometric glTF light intensities, exposure from the camera's aperture, shutter and sensitivity) and **light masks** separating dynamic, lightmapped and bake-only lights
- **Lightmap baking**, two bakers: a **CPU** baker that ray-traces ambient occlusion and soft shadows (LDR, single bounce) and a **GPU** baker that rasterises each mesh in UV space lit by the scene's own shadow maps over a few frames: soft directional shadows and ambient occlusion from virtual lights, then the bilateral denoise and dilate
- **Volumetric fog**: shadow-sampled directional ray march (Henyey-Greenstein phase, height falloff, Beer-Lambert extinction) at reduced resolution with a depth-aware upsample; clustered spot and omni lights scatter in it through their own volumes, with atlas shadows and cookies (`localOmniLights` / `localSpotLights`, per-light `volumetricScattering`)

## Animation & geometry
- **GPU skinning** (4-bone weighted blend) and **morph targets**, with skinned-mesh bone-AABB frustum culling
- **Animation state graph** (state machine, 1D/2D/directional/direct blend trees, typed parameters, crossfades) with **layers composed per node by weight** — overwrite or additive blend type, per-layer node and subtree masks, layers added at run time, optional weight normalisation — alongside the legacy animation component; glTF animation binds by node PATH, so unnamed and duplicate-named nodes animate
- **Morph-weight animation** from glTF `weights` channels

## Gaussian splatting & particles
- **Gaussian splatting** (classic path): 3DGS PLY loading, CPU-precomputed covariance, background depth sorter, EWA screen-space projection — with **view-dependent spherical harmonics** (bands 1–4), the SuperSplat **`.compressed.ply`** quantized format, and **shadow casting** (alpha-clipped footprints into directional, spot and omni shadow maps, PCF / PCSS / VSM)
- **Compute at a position in the frame**: a `ComputePass` dispatches compute work between render passes (both backends order it with the passes around it), and a camera's **after-passes** run right after everything it renders, e.g. a histogram of the scene colour it grabbed that frame
- **GPU particle system**: compute-simulated pool, curve-driven size/color/alpha over life, local and world velocity graphs, rotation speed graph, bursts, pre-warm, align-to-motion and stretch, box/sphere emitters with an initial speed along the spawn direction, sprite-sheet animation (one row per emitter or a random row per particle), additive/normal/premultiplied blending, screen-space particles drawn in the UI's order

## Post-processing & tooling
- **TAA** (with **temporal upscaling** from a lower render resolution), **SMAA 1x** (an opt-in resolve-stage effect, with edge and weight debug views), **auto exposure** (a GPU luminance histogram read back without stalling, metered and eased like the eye adapting, with a histogram debug view), **dynamic sky** (a day and night cycle with stars, moon and flat or volumetric clouds drifting with the wind, lighting the scene and driving the sun light), **lens flares** (light-driven, from a shape library copied into a texture array, faded by GPU-measured occlusion, the screen edge, distance and spot cones), **SSAO** (post-compose or per-material lighting mode), **bloom** (configurable chain depth, threshold, optional full-resolution high quality), **depth of field** (multi-pass bokeh: circle of confusion, CoC-premultiplied far downsample, concentric near/far blur, composed by CoC), **edge detection**, and a compose chain with **color grading**, **3D LUT**, chromatic **fringing**, **color enhance**, **vignette**, and tone mapping (Linear, Filmic, ACES, **ACES2**, Neutral, None)
- **Camera frame effects**: an application adds render passes of its own to a camera's post-processing frame at five stages (before the scene, between its opaque and transparent layers, after the scene, resolving the scene as TAA and SMAA do, after the resolve), with the scene depth, colour copy, half-resolution scene and scene target handed to it; its shaders are Slang compiled at run time from a string, one source for both backends. An effect can instead add a few lines of Slang to the single compose pass at one of four points (scene, HDR, LDR, output) with uniforms set from C++, no extra pass; and the composition has **debug views** (scene, depth, bloom, SSAO, DOF CoC and blur, vignette, and any an effect provides); a camera rendering into a texture of its own runs its post-processing into that texture and keeps its alpha
- **Planar reflections** with distance-based blur and **atmosphere/sky scattering** (Nishita)
- **Water** (`Water` script): a planar reflection (or the sky alone) and refraction with oblique clipping (`PlanarRenderer`), absorption, foam and a soft shoreline from the scene depth, Gerstner waves filtered into the sun's glitter in the distance, caustics and height-limited underwater fog on the materials below it, and an underwater view through the Snell window
- **Procedural sky** (`ProceduralSky` script): Preetham daylight with a night sky (stars, twilight, moon), drawn per pixel and baked into the environment atlas that lights the scene, re-baked as the sun moves; drives a sun light
- **Immediate debug lines**: the scene's `drawLine` family and a **WireRenderer** of wireframe shapes (box, sphere, circle, cylinder, capsule, cone, plane, point, arrow, axes, a camera's or a matrix's frustum, a light's extent, polylines and loops, packed line arrays) drawn for one frame, batched per layer and depth-test mode, in an optional transform
- **Debug shader passes**: replace the forward output with a single surface quantity (albedo, world normal, opacity, specularity, gloss, metalness, AO, emission, lighting, UV0) — one variant, mode switched at runtime with no recompile
- **GPU timestamp profiler** (per-pass timings on both backends) and a **MiniStats** HUD built on it, drawn by the engine itself (one-draw quad list, MSDF text, history graphs) on both backends
- **In-engine measurement hooks** in the examples harness: env-driven screenshot capture (by frame or by time, single or burst), a uniform SH probe, and a mirror floor with a pillar for screen-space-reflection checks
- **Texture arrays**: per-layer upload, `Texture::read` / `readAsync` / `copy` of one layer, and render targets over a layer (multisampled, with that layer's mips generated)
- **Volume (3D) textures** on both backends: upload with mips generated in all three dimensions, sampled with their own W address mode, `Texture::read` / `readAsync` / `copy` of one depth slice or a run of them, and render targets over a depth slice (mips regenerated after rendering)
- **KTX2/Basis compressed textures** transcoded on the loader thread to the best format the device supports (ASTC, BC7, DXT5, otherwise RGBA8)
- **ImGui overlay** (Metal/SDL3 bindings) for digital-twin HUDs, **immediate-mode** debug rendering, **transform and overlay gizmos**, and an **outline renderer** + **view cube** (extras)

## Foundation
- **Scene graph** with an entity-component system (18 component types), entity cloning that remaps references inside the cloned subtree, and a fixed-timestep `fixedUpdate` phase with a time scale and layer composition with render-action scheduling
- **GLB/glTF loading** with Draco and meshopt (`EXT_` / `KHR_meshopt_compression`) decompression, mesh quantization, Basis/WebP/AVIF textures, cameras and `KHR_lights_punctual`, `KHR_materials_variants`, `EXT_mesh_gpu_instancing` and `KHR_gaussian_splatting` (sheen as factors only; iridescence with its intensity and thickness maps; specular with its colour and factor maps; anisotropy with its direction texture), plus OBJ/STL/Assimp parsers
- **Screen-space and world-space UI** with anchored elements, image and MSDF text elements, buttons with tint and sprite states, nested stencil masks (rectangles and sprite shapes), layout groups, scrollbars and sliders, draggable elements, scroll views with momentum and bounce, and mouse and touch input events (hover, press, click, bubbling, hit padding)
- **Input devices**: keyboard, mouse, touch and gamepads, fed from the application's event loop, with press and release edges that survive a press and release inside one frame
- **Picking** through an id buffer, with the world-space point under the cursor read back from depth
- **3D annotations** (upstream's annotations scripts): numbered hotspots that face the camera at a constant screen size and show through occluding geometry at a reduced opacity, with hover colour and a click-to-open tooltip built from UI elements
- **Localization**: locales with fallbacks, messages from upstream's JSON format, CLDR plural forms, and text elements that follow the locale by key
- **SIMD math** with SSE, ARM NEON, and Apple SIMD backends (Apple SIMD active on Apple Silicon)
- **Single-source shaders (Slang)**: every engine shader written once and compiled at build time for both backends (33 programs, every file a build dependency of the bundle); the forward program's 23 chunks are user-overridable as Slang (per material or globally) with cache-invalidation hashing, and the 68-flag feature contract specialises one library per program on both backends

## Module map

What each module holds. Earlier versions of this file carried a coverage
percentage per module; those numbers had no measured basis (no line, API or
behaviour count behind them) and were removed rather than kept as estimates.
Upstream's surface that is deliberately outside this port is listed under
Known Limitations, and `AGENTS.md` records the remaining parity items.

| Module | What it holds |
|--------|---------------|
| Core / Math | Vector2/3/4, Matrix4, Quaternion (dot/slerp/nlerp per backend), Curve, Color, Random; SIMD backends SSE, NEON, Apple, scalar, with a per-backend contract test |
| Core / Events | EventHandler, EventHandle |
| Core / Shapes | BoundingBox, BoundingSphere, OrientedBox, Plane, Ray, Tri |
| Scene / Renderer | Forward PBR, camera frame graph, 24 render-pass classes, per-frame mesh and light culling, shadow-caster collection, packed sort keys with per-layer sort modes, post-processing scheduling |
| Scene / Materials | StandardMaterial with clearcoat, sheen, iridescence, transmission/dispersion/volume, anisotropy, parallax, spec-gloss, Oren-Nayar, detail normals, displacement; opacity and shadow dither |
| Scene / Lighting | Directional/point/spot + rect/disk/sphere LTC area lights, clustered lighting with a live-resizable shadow and cookie atlas, ambient SH probes, box-projected reflection probes, volumetric fog |
| Scene / Shadows | CSM (1 cascade default, up to 4, PSSM + blending), PCF/EVSM_16F/PCSS for directional, spot/point depth maps + omni cubemaps, PCSS on local lights, saturated receiver depth |
| Scene / Shader-lib | Single-source Slang shaders: 31 programs compiled at build time to MSL + metallib and SPIR-V (with reflected layouts checked against the engine), 23 overridable forward chunks compiled at run time when overridden, 68 shared feature flags as Metal function constants / Vulkan specialization constants, a local-space vertex hook, cache-invalidation hashing |
| Scene / Graphics | Camera-frame/post stack with MSAA, bloom, SSAO, TAA, multi-pass DOF, volumetric fog, compose, colour and depth grabs under both paths, environment atlas/convolution over QuadRender, HDR cubemaps, spherical harmonics |
| Scene / GSplat | Classic 3DGS path, background depth sorting, view-dependent SH bands 1-4 on both backends, uncompressed/compressed SuperSplat PLY |
| Graphics / Metal | Buffers/textures/pipelines, ASTC/BC formats, compute, particles/culling, post-processing, volumetric fog, environment baking, GSplat, texture streaming, GPU timestamp profiling |
| Graphics / Vulkan | Vulkan 1.3 dynamic rendering/synchronization2, MRT, PBR draw binding, PCSS/VSM shadows + clustered shadow atlas, SSR, dynamic refraction, planar reflections, shadow catcher, atmosphere, opacity dither, debug passes, dual-source blending, compute/particles/culling, post-processing, async uploads, GPU profiling, persistent shader cache (run-time SPIR-V and the pipeline cache kept between runs), validation smoke test |
| Framework / ECS | Engine, Entity, component-system registry, scripts, hierarchy, lifecycle/event integration |
| Framework / Components | 18 types: Camera, Render, Light, Script, Animation, Anim (state graph), Screen, Element, Button, LayoutGroup, LayoutChild, Scrollbar, ScrollView, Collision, RigidBody, Joint, GSplat, ParticleSystem |
| Framework / Animation | GPU skinning, morph targets/weights, clips/evaluator/binder with path resolution, state graphs, transitions, blend trees, weighted layer composition |
| Framework / Gizmo | Upstream's transform gizmos: translate (arrows, plane handles, centre), rotate (half/full arc rings, view ring, orbit sphere, angle guides), scale (box-lines, planes, uniform centre); triangle picking, hover themes, snapping, drag modes, span guide lines, constant screen size; translucent outlined planes and glancing fades; overlay gizmos with draggable, snapping handles and value labels: a bounding box gizmo (corner brackets, dashed edges and face and corner handles resizing the selection), camera (frustum, clip planes, field of view), light (directional, omni, spot and area shapes, range, cone angles, area size) and shape gizmos (box, sphere, capsule, cone, cylinder), and an icon gizmo marking lights, cameras and other unseen entities; an anti-aliased reference grid (levels that follow the zoom, axes, distance fade, infinite or fixed, contrast and depth-write options) |
| Framework / Assets | Async container/texture/font loading; GLB/glTF (+Draco, meshopt, quantised attributes, texture transform, node identity for unnamed nodes), OBJ/STL/Assimp; KTX2/Basis transcoding to ASTC or BC |
| Framework / Lightmapper | CPU ray-traced baker and GPU UV-space baker |
| Overlay | Metal-only ImGui/ImPlot HUD integration, input capture, digital-twin theme, 3D-anchored labels/panels |

## Known Limitations

- Metal remains the primary graphics backend; Vulkan is functional and covers most rendering paths, but not yet at full parity. Metal-only today: texture streaming, the ImGui/ImPlot overlay, and the compute passes used by the sibling visualization project (marching cubes)
- No XR: `XrManager` is an empty placeholder
- No audio subsystem; no sprite component (sprites are data an image element shows)
- Gaussian splatting: WebP-packed SOG format and the unified octree/LOD streaming path are not ported
- Reflection probes support runtime scene-capture baking (dynamic cubemap) as well as supplied cubemaps; per-level GGX cube prefilter is deferred (roughness uses hardware trilinear cube mips)
- Texture streaming is partial (no progressive mip-level budgeting)
- Lightmap baking: the CPU baker is LDR and single-bounce with no colour+direction output or automatic UV unwrap; the GPU baker has no colour+direction (BAKE_COLORDIR) output
- Screen-space reflections have no HiZ acceleration and no temporal accumulation; the roughness cone reads the colour grab's mips rather than tracing a cone, and geometry thinner than one march step can be skipped
- Animation: the legacy `AnimationComponent` fires no animation events (the state-graph `AnimComponent` does)
