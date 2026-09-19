# Features

A detailed inventory of what VisuTwin Canvas implements today, and how far each
module has been carried. For an overview and getting-started instructions, see
the [README](README.md).

## Rendering & materials
- **Forward PBR renderer** (metalness/roughness) with multi-light support (directional, point, spot, rectangular/disk/spherical **area lights** via LTC) and a frame-graph pass scheduler
- **StandardMaterial** with clearcoat, anisotropy, sheen, iridescence, transmission, **parallax occlusion mapping** (height base + self-shadowing), **spec-gloss** (KHR_materials_pbrSpecularGlossiness), **Oren-Nayar** diffuse, **detail normals** (UDN), and vertex-stage **displacement mapping**
- **Wide lines**: connected polylines with per-point colour and width, caps, joins and dash patterns, batched into one instanced draw
- **Rigid-body physics** behind an application-supplied `PhysicsWorld` seam, with a Jolt backend: box/sphere/capsule/cylinder shapes, static/dynamic/kinematic bodies, forces, impulses, raycasts, and **joints** (fixed, ball, hinge, slider, 6dof) with limits, motors and break impulses
- **Image-based lighting**: environment atlas (GGX/Lambert prefiltered), HDR cubemap skybox, **ambient SH light probes**, and **box-projected cubemap reflection probes** (parallax-corrected local reflections)
- **Dynamic grab-pass refraction** with chromatic **dispersion** and KHR_materials_volume Beer-law attenuation
- **Screen-space reflections**: a per-fragment world-space march against the scene depth and colour grabs, reaching 0.4 of the camera range, with bisection-refined hits, a roughness cone that reads the colour grab's mips, and env/probe fallback; works under the standalone and the camera-frame (post-processing) paths
- **Vertex colors**, **point-size** primitives, **opacity dither** (Bayer8 order-independent transparency), and **lightmap** (UV1) sampling
- **GPU instancing** with per-instance color and optional per-frame GPU frustum culling (compute-driven indirect draw), plus **dynamic batching** with a bone-index matrix palette
- **MSAA** on the offscreen scene target and **dual-source blending** (`BLENDMODE_SRC1_*`) for custom blend setups

## Lighting & shadows
- **Cascaded shadow maps** (one cascade by default as upstream, up to four PSSM cascades with cross-cascade blending and distance fade); receivers beyond the caster-fitted range are shadowed, so a receive-only ground works
- **PCF**, **EVSM_16F** (Exponential Variance Shadow Maps: separable Gaussian blur, Chebyshev sampling, caster-AABB depth tightening), and **PCSS** contact-hardening soft shadows for directional lights
- **Spot/point** 2D depth maps and **omnidirectional cubemap** shadows, with PCSS also supported on spot/omni local lights
- **Clustered lighting** for many-light scenes with a packed **shadow atlas** whose resolution can change at runtime, plus a **shadow catcher** material for compositing
- **Lightmap baking**, two bakers: a **CPU** baker that ray-traces ambient occlusion and soft shadows (LDR, single bounce) and a **GPU** baker that rasterises each mesh in UV space lit by the scene's own shadow maps in one frame
- **Volumetric fog**: shadow-sampled directional ray march (Henyey-Greenstein phase, height falloff, Beer-Lambert extinction) at reduced resolution with a depth-aware upsample

## Animation & geometry
- **GPU skinning** (4-bone weighted blend) and **morph targets**, with skinned-mesh bone-AABB frustum culling
- **Animation state graph** (state machine, 1D/2D/directional/direct blend trees, typed parameters, crossfades) with **layers composed per node by weight** — overwrite or additive blend type, per-layer node masks, optional weight normalisation — alongside the legacy animation component; glTF animation binds by node PATH, so unnamed and duplicate-named nodes animate
- **Morph-weight animation** from glTF `weights` channels

## Gaussian splatting & particles
- **Gaussian splatting** (classic path): 3DGS PLY loading, CPU-precomputed covariance, background depth sorter, EWA screen-space projection — with **view-dependent spherical harmonics** (bands 1–3) and the SuperSplat **`.compressed.ply`** quantized format
- **GPU particle system**: compute-simulated pool, curve-driven size/color/alpha over life, box/sphere emitters, sprite-sheet animation, additive/normal/premultiplied blending

## Post-processing & tooling
- **TAA**, **SSAO** (post-compose or per-material lighting mode), **bloom** (configurable chain depth), **depth of field** (multi-pass bokeh: circle of confusion, CoC-premultiplied far downsample, concentric near/far blur, composed by CoC), **edge detection**, and a compose chain with **color grading**, **3D LUT**, chromatic **fringing**, **color enhance**, **vignette**, and tone mapping (Linear, Filmic, ACES, **ACES2**, Neutral, None)
- **Planar reflections** with distance-based blur, **atmosphere/sky scattering** (Nishita), and **surface LIC** flow visualization
- **Debug shader passes**: replace the forward output with a single surface quantity (albedo, world normal, opacity, specularity, gloss, metalness, AO, emission, lighting, UV0) — one variant, mode switched at runtime with no recompile
- **GPU timestamp profiler** (per-pass timings on both backends) and a **MiniStats** ImGui HUD built on it
- **In-engine measurement hooks** in the examples harness: env-driven screenshot capture (by frame or by time, single or burst), a uniform SH probe, and a mirror floor with a pillar for screen-space-reflection checks
- **KTX2/Basis compressed textures** transcoded to ASTC 4×4 on the loader thread
- **ImGui overlay** (Metal/SDL3 bindings) for digital-twin HUDs, **immediate-mode** debug rendering, **transform gizmos**, and an **outline renderer** + **view cube** (extras)

## Foundation
- **Scene graph** with an entity-component system (14 component types) and layer composition with render-action scheduling
- **GLB/glTF loading** with Draco decompression, plus OBJ/STL/Assimp parsers
- **Screen-space UI** with anchored elements, buttons, and text rendering
- **SIMD math** with SSE, ARM NEON, and Apple SIMD backends (Apple SIMD active on Apple Silicon)
- **ShaderChunks registry**: 25 named, user-overridable Metal micro-chunks with cache-invalidation hashing, plus build-time-embedded standalone shaders; the Vulkan backend compiles a parallel GLSL set (20 chunks and 19 stage programs, every file a build dependency of the bundle) to SPIR-V and drives the same 58-flag feature contract through specialization constants
- **XR / ARKit** framework (in development)

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
| Scene / Renderer | Forward PBR, camera frame graph, 22 render-pass classes, per-frame mesh and light culling, shadow-caster collection, packed sort keys with per-layer sort modes, post-processing scheduling |
| Scene / Materials | StandardMaterial with clearcoat, sheen, iridescence, transmission/dispersion/volume, anisotropy, parallax, spec-gloss, Oren-Nayar, detail normals, displacement; opacity and shadow dither |
| Scene / Lighting | Directional/point/spot + rect/disk/sphere LTC area lights, clustered lighting with a live-resizable shadow and cookie atlas, ambient SH probes, box-projected reflection probes, volumetric fog |
| Scene / Shadows | CSM (1 cascade default, up to 4, PSSM + blending), PCF/EVSM_16F/PCSS for directional, spot/point depth maps + omni cubemaps, PCSS on local lights, saturated receiver depth |
| Scene / Shader-lib | 25 overridable Metal chunks, 20 GLSL chunks, 58 shared feature flags (Metal defines / Vulkan specialization constants), cache-invalidation hashing, 2 embedded MSL programs |
| Scene / Graphics | Camera-frame/post stack with MSAA, bloom, SSAO, TAA, multi-pass DOF, volumetric fog, compose, colour and depth grabs under both paths, environment atlas/convolution over QuadRender, HDR cubemaps, spherical harmonics |
| Scene / GSplat | Classic 3DGS path, background depth sorting, view-dependent SH bands 1-3 on both backends, uncompressed/compressed SuperSplat PLY |
| Graphics / Metal | Buffers/textures/pipelines, ASTC/BC formats, compute, particles/culling, post-processing, volumetric fog, environment baking, GSplat, texture streaming, GPU timestamp profiling |
| Graphics / Vulkan | Vulkan 1.3 dynamic rendering/synchronization2, MRT, PBR draw binding, PCSS/VSM shadows + clustered shadow atlas, SSR, dynamic refraction, planar reflections, shadow catcher, atmosphere, opacity dither, debug passes, dual-source blending, compute/particles/culling, post-processing, async uploads, GPU profiling, validation smoke test |
| Framework / ECS | Engine, Entity, component-system registry, scripts, hierarchy, lifecycle/event integration |
| Framework / Components | 14 types: Camera, Render, Light, Script, Animation, Anim (state graph), Screen, Element, Button, Collision, RigidBody, Joint, GSplat, ParticleSystem |
| Framework / Animation | GPU skinning, morph targets/weights, clips/evaluator/binder with path resolution, state graphs, transitions, blend trees, weighted layer composition |
| Framework / Gizmo | Interactive translate/rotate/scale handles with axis picking and snapping |
| Framework / Assets | Async container/texture/font loading; GLB/glTF (+Draco, quantised attributes, texture transform, node identity for unnamed nodes), OBJ/STL/Assimp; KTX2/Basis transcoding to ASTC or BC |
| Framework / Lightmapper | CPU ray-traced baker and GPU UV-space baker |
| Viz / Overlay | Metal-only ImGui/ImPlot HUD integration, input capture, digital-twin theme, 3D-anchored labels/panels |

## Known Limitations

- Metal remains the primary graphics backend; Vulkan is functional and covers most rendering paths, but not yet at full parity. Metal-only today: volumetric fog, texture streaming, the ImGui/ImPlot overlay, and the compute passes used by the sibling visualization project (marching cubes, LIC)
- No audio subsystem; no Sprite / layout / scroll-view UI components
- Gaussian splatting: WebP-packed SOG format and the unified octree/LOD streaming path are not ported
- Reflection probes support runtime scene-capture baking (dynamic cubemap) as well as supplied cubemaps; per-level GGX cube prefilter is deferred (roughness uses hardware trilinear cube mips)
- Texture streaming is partial (no progressive mip-level budgeting)
- Lightmap baking: the CPU baker is LDR and single-bounce with no colour+direction output or automatic UV unwrap; the GPU baker has no bounce passes, no ambient-occlusion virtual lights and no dilate/denoise
- Screen-space reflections have no HiZ acceleration and no temporal accumulation; the roughness cone reads the colour grab's mips rather than tracing a cone, and geometry thinner than one march step can be skipped
- Animation: no animation events
