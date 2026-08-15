# Features

A detailed inventory of what VisuTwin Canvas implements today, and how far each
module has been carried. For an overview and getting-started instructions, see
the [README](README.md).

## Rendering & materials
- **Forward PBR renderer** (metalness/roughness) with multi-light support (directional, point, spot, rectangular/disk/spherical **area lights** via LTC) and a frame-graph pass scheduler
- **StandardMaterial** with clearcoat, anisotropy, sheen, iridescence, transmission, parallax, **spec-gloss** (KHR_materials_pbrSpecularGlossiness), **Oren-Nayar** diffuse, **detail normals** (UDN), and vertex-stage **displacement mapping**
- **Image-based lighting**: environment atlas (GGX/Lambert prefiltered), HDR cubemap skybox, **ambient SH light probes**, and **box-projected cubemap reflection probes** (parallax-corrected local reflections)
- **Dynamic grab-pass refraction** with chromatic **dispersion** and KHR_materials_volume Beer-law attenuation
- **Screen-space reflections** (per-fragment world-space ray march against a scene depth+color grab, with env/probe fallback)
- **Vertex colors**, **point-size** primitives, **opacity dither** (Bayer8 order-independent transparency), and **lightmap** (UV1) sampling
- **GPU instancing** with per-instance color and optional per-frame GPU frustum culling (compute-driven indirect draw), plus **dynamic batching** with a bone-index matrix palette
- **MSAA** on the offscreen scene target and **dual-source blending** (`BLENDMODE_SRC1_*`) for custom blend setups

## Lighting & shadows
- **Cascaded shadow maps** (4-cascade PSSM with cross-cascade blending and distance fade)
- **PCF**, **EVSM_16F** (Exponential Variance Shadow Maps: separable Gaussian blur, Chebyshev sampling, caster-AABB depth tightening), and **PCSS** contact-hardening soft shadows for directional lights
- **Spot/point** 2D depth maps and **omnidirectional cubemap** shadows, with PCSS also supported on spot/omni local lights
- **Clustered lighting** for many-light scenes, plus a **shadow catcher** material for compositing
- **Volumetric fog**: shadow-sampled directional ray march (Henyey-Greenstein phase, height falloff, Beer-Lambert extinction) at reduced resolution with a depth-aware upsample

## Animation & geometry
- **GPU skinning** (4-bone weighted blend) and **morph targets**, with skinned-mesh bone-AABB frustum culling
- **Animation state graph** (state machine, 1D/2D/directional/direct blend trees, typed parameters, crossfades) alongside the legacy animation component
- **Morph-weight animation** from glTF `weights` channels

## Gaussian splatting & particles
- **Gaussian splatting** (classic path): 3DGS PLY loading, CPU-precomputed covariance, background depth sorter, EWA screen-space projection — with **view-dependent spherical harmonics** (bands 1–3) and the SuperSplat **`.compressed.ply`** quantized format
- **GPU particle system**: compute-simulated pool, curve-driven size/color/alpha over life, box/sphere emitters, sprite-sheet animation, additive/normal/premultiplied blending

## Post-processing & tooling
- **TAA**, **SSAO** (post-compose or per-material lighting mode), **bloom** (configurable chain depth), **depth of field**, **edge detection**, and a compose chain with **color grading**, **3D LUT**, chromatic **fringing**, **color enhance**, **vignette**, and tone mapping (Linear, Filmic, ACES, **ACES2**, Neutral, None)
- **Planar reflections** with distance-based blur, **atmosphere/sky scattering** (Nishita), and **surface LIC** flow visualization
- **Debug shader passes**: replace the forward output with a single surface quantity (albedo, world normal, opacity, specularity, gloss, metalness, AO, emission, lighting, UV0) — one variant, mode switched at runtime with no recompile
- **GPU timestamp profiler** (per-pass timings on both backends) and a **MiniStats** ImGui HUD built on it
- **KTX2/Basis compressed textures** transcoded to ASTC 4×4 on the loader thread
- **ImGui overlay** (Metal/SDL3 bindings) for digital-twin HUDs, **immediate-mode** debug rendering, **transform gizmos**, and an **outline renderer** + **view cube** (extras)

## Foundation
- **Scene graph** with an entity-component system (13 component types) and layer composition with render-action scheduling
- **GLB/glTF loading** with Draco decompression, plus OBJ/STL/Assimp parsers
- **Screen-space UI** with anchored elements, buttons, and text rendering
- **SIMD math** with SSE, ARM NEON, and Apple SIMD backends (Apple SIMD active on Apple Silicon)
- **ShaderChunks registry**: 24 named, user-overridable Metal micro-chunks with cache-invalidation hashing, plus build-time-embedded standalone shaders; the Vulkan backend compiles a parallel GLSL shader set to SPIR-V and drives the same 51-flag feature contract through specialization constants
- **XR / ARKit** framework (in development)

## Implementation Status

| Module | Coverage | Notes |
|--------|----------|-------|
| Core / Math | ~80% | Vector2/3/4, Matrix4, Quaternion, Curve, Color, Random (SIMD multi-backend) |
| Core / Events | ~95% | EventHandler, EventHandle |
| Core / Shapes | ~70% | BoundingBox, BoundingSphere, OrientedBox, Plane, Ray, Tri |
| Scene / Renderer | ~80% | Forward PBR, camera frame graph, 25 render-pass classes, frustum/shadow-caster culling, layer sorting, and post-processing scheduling |
| Scene / Materials | ~85% | StandardMaterial with clearcoat, sheen, iridescence, transmission/dispersion/volume, anisotropy, parallax, spec-gloss, Oren-Nayar, detail normals, and displacement |
| Scene / Lighting | ~80% | Directional/point/spot + rect/disk/sphere LTC area lights, clustered lighting/cookie atlas, ambient SH probes, box-projected reflection probes, and volumetric fog |
| Scene / Shadows | ~85% | 4-cascade CSM (PSSM + blending), PCF/EVSM_16F/PCSS for directional, spot/point depth maps + omni cubemaps, PCSS on local lights |
| Scene / Shader-lib | ~80% | 24 overridable Metal chunks, 51 shared feature flags (Metal defines / Vulkan specialization constants), cache-invalidation hashing, and 3 embedded standalone shaders |
| Scene / Graphics | ~70% | Camera-frame/post stack with MSAA, bloom, SSAO, TAA, DOF, volumetric fog, compose, color/depth grabs, environment atlas/convolution, HDR cubemaps, and spherical harmonics |
| Scene / GSplat | ~60% | Classic 3DGS path, background depth sorting, view-dependent SH bands 1–3 on both backends, and uncompressed/compressed SuperSplat PLY |
| Graphics / Metal | ~70% | Buffers/textures/pipelines, ASTC/BC formats, compute, particles/culling, post-processing, volumetric fog, environment baking, GSplat, texture streaming, and GPU timestamp profiling |
| Graphics / Vulkan | ~75% | Vulkan 1.3 dynamic rendering/synchronization2, MRT, PBR draw binding, PCSS/VSM shadows + clustered shadow atlas, SSR, dynamic refraction, planar reflections, shadow catcher, atmosphere, opacity dither, debug passes, dual-source blending, compute/particles/culling, post-processing, async uploads, GPU profiling, and validation smoke coverage |
| Framework / ECS | ~75% | Engine, Entity, component-system registry, scripts, hierarchy, and lifecycle/event integration |
| Framework / Components | ~55% | 13 types: Camera, Render, Light, Script, Animation, Anim (state graph), Screen, Element, Button, Collision, RigidBody, GSplat, ParticleSystem |
| Framework / Animation | ~75% | GPU skinning, morph targets/weights, clips/evaluator/binder, state graphs, transitions, and blend trees |
| Framework / Gizmo | ~75% | Interactive translate/rotate/scale handles with axis picking and snapping |
| Framework / Assets | ~65% | Async container/texture/font loading; GLB/glTF (+Draco), OBJ/STL/Assimp; KTX2/Basis transcoding to ASTC or BC |
| Viz / Overlay | ~45% | Metal-only ImGui/ImPlot HUD integration, input capture, digital-twin theme, and 3D-anchored labels/panels |

## Known Limitations

- Metal remains the primary graphics backend; Vulkan is functional and covers most rendering paths, but not yet at full parity. Metal-only today: volumetric fog, texture streaming, the ImGui/ImPlot overlay, and the compute passes used by the sibling visualization project (marching cubes, LIC)
- No audio subsystem; no Sprite / layout / scroll-view UI components
- Gaussian splatting: WebP-packed SOG format and the unified octree/LOD streaming path are not ported
- Reflection probes support runtime scene-capture baking (dynamic cubemap) as well as supplied cubemaps; per-level GGX cube prefilter is deferred (roughness uses hardware trilinear cube mips)
- Texture streaming is partial (no progressive mip-level budgeting)
- The lightmapper baker is CPU-only (LDR, single bounce, no color+dir or auto-UV-unwrap)
- Screen-space reflections march per-fragment (no HiZ acceleration or roughness cone) and are sharp-only, with no temporal accumulation
