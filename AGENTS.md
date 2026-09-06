# Agent Guidelines for VisuTwin Canvas

Rules, contracts and known traps for AI agents and developers working on this
codebase. Tool-neutral by design: Claude Code reaches it through `CLAUDE.md`,
which imports this file, and any other agent that reads `AGENTS.md` gets the same
instructions.

C++23 real-time rendering engine for digital twins, geospatial scenes, and
scientific visualization. Architecture originally derived from the PlayCanvas
engine (referred to as **upstream** throughout the code and docs — never by
name), rebuilt in C++ and substantially extended. Runs on Apple Metal and
Vulkan 1.3.

## Where things are written down

- **This file (`AGENTS.md`)** — rules, contracts, live gotchas and open items.
  Read before changing code. Keep it that way: it is loaded in full at the start
  of every session, so a completed-work narrative does not belong here.
- **`CLAUDE.md`** — a stub that imports this file. Do not put content in it.
- **`ARCHITECTURE.md`** — per-subsystem reference: how each feature works, the
  call that turns it on, and its deviations from upstream. Consult when you are
  about to touch a subsystem.
- **`ENGINEERING-LOG.md`** — postmortems, migration accounting and verification
  numbers for work already finished. Consult when you need to know *why* a thing
  is the way it is, or how a class of bug was isolated before.
- **`FEATURES.md`** — user-facing inventory of what the engine implements.
- **`CONTRIBUTING.md`**, **`README.md`** — the usual.
- `docs/` is **gitignored**. Do not put tracked documentation there.

## Project Structure

```
visutwin-canvas/
  engine/          # Core 3D engine (278 .h + 220 .cpp = 498 files)
    src/core/      # Math (Vector2/3/4, Matrix4, Quaternion, SIMD multi-backend), shapes, events, tags
    src/platform/  # Graphics abstraction + Metal and Vulkan backends, input
    src/scene/     # Scene graph, renderer, materials, shader-lib, lighting, shadows
    src/framework/ # ECS (Engine, Entity, Components), asset loading, parsers, gizmos, input
    shaders/metal/chunks/   # 25 composable Metal shader micro-chunks (ShaderChunks registry)
    shaders/vulkan/chunks/  # 18 GLSL fragment chunks, same names (forward.frag #includes them)
    shaders/metal/embedded/ # self-contained MSL programs embedded at build time (particle sim/render, gsplat render)
    shaders/vulkan/         # GLSL sources compiled to SPIR-V at build time (27 files)
  examples/        # 52 example applications, all derived from ExampleApp
  tests/           # Unit tests + Vulkan validation smoke test
  assets/          # Shared assets (models, textures, HDR environments)
  tools/           # Build/utility scripts
```

Sibling repositories (separate CMake projects, same parent dir):
- `visutwin-geo/` - Geospatial: WGS84 ellipsoid, 3D Tiles, terrain tiling, globe camera, atmosphere
- `visutwin-viz/` - Scientific visualization: volume loading, marching cubes, streamlines, transfer functions

## Build

- **C++23**, CMake 3.28+, vcpkg manifest mode
- `vcpkg.json` + `CMakePresets.json` at project root
- Presets: `default` (Debug, Metal), `release`, `examples` (→ `build-examples/`), `vulkan` (→ `build-vulkan/`)
- Backends selected explicitly with `VISUTWIN_BACKEND_METAL=ON|OFF` and
  `VISUTWIN_BACKEND_VULKAN=ON|OFF`; at least one must be enabled
- `VISUTWIN_BUILD_EXAMPLES` defaults to OFF — the `examples` preset turns it on
- CLion: use "default" preset, ensure `/opt/homebrew/bin` in PATH for Ninja
- `CMAKE_IGNORE_PATH=/usr/local/include;/usr/local/lib` to exclude stale system SDL3
- Runtime backend override: `VISUTWIN_BACKEND=metal|vulkan` (lower case)

```bash
cmake --preset default
cmake --build build
```

## Dependencies (vcpkg)

**Core:** SDL3 (3.4+), spdlog (1.17+, bundled fmt, `default-features: false`), tinyobjloader, tinygltf (header-only, use `find_path`), draco, assimp, basisu (`basisu::basisu_encoder` — transcoder + encoder + CLI tool), boost-core, imgui (SDL3+Metal+docking), implot

**Vendored:** metal-cpp, stb

**Physics (`jolt` feature):** joltphysics. `VISUTWIN_PHYSICS_JOLT` (ON by default)
decides whether the Jolt-backed `PhysicsWorld` is compiled in; the seam itself is
always there, and the option degrades to a warning if the package is missing.

**Vulkan (`vulkan` feature):** vulkan-headers, vulkan-memory-allocator, vk-bootstrap

## Graphics Backends

Two production backends behind one `GraphicsDevice` abstraction, plus one planned.

**Metal** — primary and most complete. MSL shader chunks hot-reload from the
source dir per launch; `VT_FEATURE_*` flags are emitted as preprocessor defines
and each combination compiles a distinct variant.

**Vulkan 1.3** — dynamic rendering + synchronization2, MRT, PBR draw binding,
PCSS/VSM shadows + clustered shadow atlas, SSR, dynamic refraction, planar
reflections, shadow catcher, atmosphere, opacity dither, debug passes,
dual-source blending, compute/particles/culling, post-processing, async uploads,
GPU profiling. GLSL in `shaders/vulkan/` is compiled to SPIR-V at build time and
bundled by `tools/generate_vulkan_shader_bundle.py`. Feature flags arrive as
**specialization constants**, not runtime branches, in BOTH stages.

`VulkanGraphicsDevice` is split across
`vulkanGraphicsDevice{FrameSwapchain,Descriptors,Uploads,DrawBinding,Compute,ResourceInitialization}.cpp`
— add new code to the matching component, not one monolith.

**Metal-only today:** volumetric fog on the compute path, texture streaming, the
ImGui/ImPlot overlay (`viz/overlay/`, uses `imgui_impl_metal`), marching cubes,
LIC, and the gloss/thickness/refraction scalar maps.

**Planned next backend: WebGPU** — targets browser and native (Dawn/wgpu). WGSL
maps onto the same shared feature contract; the specialization-constant approach
used for Vulkan is the closer model (WGSL `override` constants) than Metal's
preprocessor variants. Keep new backend-specific code behind `GraphicsDevice`
so a third implementation stays additive.

Standard [0,1] depth (clear 1.0, `LESS_EQUAL` compare) — NOT reverse-Z. Vulkan is
natively [0,1]; Metal vertex chunks remap clip.z from GL [-1,1], and the Vulkan
vertex shaders apply the same remap, so both backends share the convention. A
custom user shader that skips the remap wins every depth test on Vulkan.

## Contracts you must not break

### Adding a `MaterialUniforms` field

`MaterialUniforms` is declared ONCE, in `scene/materials/materialUniformFields.h`,
as an X-macro field list (`X(shaderType, name, default...)` — variadic so a braced
initialiser's commas stay in one argument). Everything else is emitted from it:
the C++ struct in `material.h`, the MSL `struct MaterialData` substituted into the
`VT_MATERIAL_DATA_BLOCK` marker in `common-structs.metal`, the GLSL block written
to `shader_material.glsl` and emitted at runtime by
`ProgramLibrary::glslMaterialBlock()`, and the generator's size check. **Adding a
field is one line.**

The GLSL block is `#include`d by BOTH `forward-fragment-head.glsl` and
`forward.vert`: MoltenVK miscompiles a UBO whose member list differs between
stages. There is no `static_assert(sizeof(...) == 400)` any more; what is asserted
is the real invariant, that the size is a multiple of 16. The struct is a plain
aggregate so `alignof` is 4, not 16 — the layout works because every `vec4` in the
list lands 16-aligned by construction.

**NOT single-sourced: the LIGHTING block.** `UniformBinder::LightingUniforms` (49
fields) and `VulkanLightingUBO` (54) are genuinely DIFFERENT layouts, not copies.
Metal keeps atmosphere in its own buffer at slot 9 while Vulkan folds it into the
lighting UBO, the per-light structs differ (6 vec4s with packed uints vs 7 with
area-light data), and most shared fields sit at different indices. Unifying them
means rewriting one backend's shaders, not extracting a header. When you change it,
`VulkanLightingUBO`'s size is asserted in `vulkanRenderPipeline.cpp` AND in the
shader-bundle validator.

### Adding a shader feature

`platform/graphics/shaderFeatures.h` defines the `VT_SHADER_FEATURES` entries used
by both backends. Add the `X(Symbol, "VT_FEATURE_NAME")` line and nothing else:
indices are assigned automatically from declaration order, `ShaderFeatureSet`
stores them as `kShaderFeatureWordCount` 32-bit words and widens by one word
every 32 features, `vulkanRenderPipeline` binds one
specialization constant per word, and the bundle generator emits matching
`vtFeatureMask<N>` constants plus `vtFeatureEnabled(bit)`. Nothing persists an
index across builds, so the list may be reordered freely.

Shader variants are cached on an exact `ProgramLibrary::VariantKey` (program-name
hash + the feature set itself + chunk-override hashes), compared in full rather
than folded into one integer, so no two variants can alias.

### Adding a texture slot

Bump `MetalTextureBinder::kMaxTextureSlots` AND add the slot to the
`materialSlots` clear list in `bindMaterialTextures`. Slots 0-33 are taken today.
On Vulkan, the fragment stage already declares 15 combined image samplers and
MoltenVK inherits a 16-per-stage limit — a 16th needs the separate-image plus
shared-sampler treatment the light cookies use.

The set-1 slot list lives in exactly one place, `kMaterialTextureBindings` in
`vulkanUniformLayouts.h`. It used to be duplicated across layout creation, the
binding loop and the descriptor writes, and adding a slot to two of the three
wrote every binding to the wrong index.

### Metal buffer slots

0=vertex, 1=index, 2=model, 3=material, 4=lighting, 5=scene, 6=palette (dynamic
batch + skinning, **mutually exclusive**), 7-8=clustered (fragment) / gsplat data
and order (vertex), 9=morph deltas, 10=morph params, 11=gsplat params.

Vulkan does NOT mirror these numerically — it binds through descriptor sets
(`vulkanUniformLayouts.h`). Keep the two mappings in sync conceptually, not by
index.

The particle, gsplat and storage-draw paths SHARE slots 7 and 11. One mesh
instance is a storage draw, a particle draw, or a splat draw, never two at once.

## Physics

The engine owns NO simulation. `framework/physics/physicsWorld.h` declares
`PhysicsWorld` / `PhysicsBody` / `PhysicsBodyDesc`, and an application supplies an
implementation through `AppOptions::physicsWorld` — the same rule component
systems follow. `createJoltPhysicsWorld()` returns the Jolt-backed one.

    options.registerComponentSystem<CollisionComponentSystem>();
    options.registerComponentSystem<RigidBodyComponentSystem>();
    options.physicsWorld = createJoltPhysicsWorld();

- **With no world supplied nothing simulates and nothing breaks.** The rigid-body
  component holds its settings, and `raycastFirst`/`raycastAll` fall back to the
  CPU sweep over collision bounds that predates the seam. That is what the
  `raycast` example still uses.
- `RigidBodyComponentSystem` resolves the world on its first update, not in its
  constructor. Component systems are built from `AppOptions` before the engine has
  finished storing everything else that came with it, and a world read once at
  construction is null forever — a whole scene frozen with nothing to show why.
- The body is created lazily from the component's settings plus the sibling
  `CollisionComponent`'s shape, and any setter marks it stale so it is rebuilt.
  Authoring order therefore does not matter.
- **Static bodies do not get their transform written back** (nothing else should
  be fighting whatever placed them) and **kinematic bodies are pushed the other
  way**: the entity's transform goes INTO the simulation.
- **The world is stepped on `fixedUpdate`, not `update`.** `Engine::update` owns
  the accumulator and fires it zero or more times a frame at
  `Engine::fixedDeltaTime()`; `RigidBodyComponentSystem::step(dt)` is public for
  driving the simulation from another clock, and `setTimeScale(0)` pauses it
  (nothing steps, nothing is written back) while the rest of the engine runs on.
- **`raycastAll` returns hits NEAREST FIRST on both paths.** It used to sort only
  on the CPU fallback, so the order depended on whether a physics world had been
  supplied.
- `teleport()` rather than `setPosition()` on a simulated entity: the step would
  overwrite a bare transform, and Jolt does not wake a body that was only moved.
- `CollisionComponent::height` is the FULL height for a capsule, caps included;
  the backend converts to Jolt's cylindrical half-height.
- **A joint lives on its OWN entity, and that entity's transform is the joint
  FRAME** — its local **X axis is the primary axis** (hinge rotation, slider
  travel, ball twist). Position and orient the entity, parent it, THEN add the
  component; the frame is captured from the final world transform. That is
  upstream's convention, and it is why `JointComponent` takes `entityA` /
  `entityB` rather than an anchor offset. A null `entityB` pins that end to the
  world.
- **Constraints are built as (B, A), not (A, B).** The backend measures a
  constraint's angle and travel from body 1 toward body 2, so the ANCHOR has to go
  first for a positive motor speed or limit to move end A the way the caller
  means. Built the other way round a slider told to run at +1.5 travels at -1.5,
  which is what the unit test caught.
- **Two body locks cannot be taken separately.** Jolt asserts on the deadlock
  risk; `BodyLockMultiWrite` takes both and orders them itself. Without a Jolt
  assert handler installed this arrives as a bare SIGTRAP with nothing on stderr,
  so `joltPhysicsWorld.cpp` installs one.
- Joint limits and motor speeds are authored in DEGREES for a hinge and metres for
  a slider, matching upstream; the component converts.

## Rendering Pipeline

Forward PBR renderer with frame graph:
`Engine::render()` -> `ForwardRenderer::buildFrameGraph()` -> `FrameGraph::compile()` -> `FrameGraph::render()`

**Compose pass effect chain** (upstream `compose.js` order): CAS → DOF → SSAO →
**fringing** → bloom → **color enhance** → **color grading** → tonemap → **3D
color LUT** → vignette → gamma. Configured via
`CameraComponent::RenderingSettings` → CameraFrameOptions → RenderPassCompose →
ComposePassParams.

- DOF runs BEFORE SSAO. Occlusion multiplies the already-defocused colour and is
  not itself blurred, so it keeps full strength out of focus; the other way round
  the defocus washes it out with everything else. Where DOF is not blurring the
  order cannot matter, which is the check that says a change here landed: the
  in-focus part of the frame must come back bit-identical.
- **The CAS uniform is NEGATIVE.** `RenderPassCompose` remaps the user
  sharpness to upstream's `lerp(-0.125, -0.2, s)` and the shaders gate on `< 0`;
  a positive weight turns the same kernel into a 5-tap blur, which is what the
  pass did until 2026-09-06. Verify a sharpness change with gradient energy over
  a static crop, not by eye.
- Fringing (chromatic aberration, user intensity /1024) **must stay BEFORE
  bloom**: it re-samples the scene texture for R and B, so running it after bloom
  leaves bloom in green only. It also overwrites R and B from the raw scene
  texture, which paints magenta over occluded pixels if combined with
  compose-mode SSAO. That is upstream's own design; fix the scene, not the engine.
- The 3D LUT is a 256x16 Unreal strip with dual-LUT blend; the port loads it
  non-sRGB so the sample is pow(2.2)-decoded in-shader. Test asset:
  `assets/textures/lut-teal-orange.tga`.

**Frame graph store propagation.** `FrameGraph::compile` walks the passes and,
whenever a later pass reads a target WITHOUT clearing it, marks the earlier pass
on that target as having to STORE. The back buffer is included (`nullptr` render
target), and `_renderTargetMap` is per-frame state. Grab passes carry no color ops
of their own and must not displace the real draw pass in that map.

**Tone mapping.** 6 modes dispatched in `common.metal :: toneMap`: LINEAR (0),
FILMIC (1), ACES (3), ACES2 (4, Stephen Hill RRT+ODT fit), NEUTRAL (5), NONE (6).
Set scene-wide with `Scene::setToneMapping` or per camera with
`CameraComponent::setToneMapping` (defaults to `TONEMAP_INHERIT` = -1). NOTE:
`RenderingSettings::toneMapping` is a separate, currently **unread** field —
`applyCameraSettings` never copies it into `CameraFrameOptions`. Use
`setToneMapping`.

**Under CameraFrame the forward pass must output LINEAR HDR** and leave exposure,
tonemap and gamma to compose. The gate is bit 5 of `LightingData::flagsAndPad[0]`,
kept in step with `hdrPass()`. Every shader path that returns early — the tail,
and all three sky paths — has to check it, or compose applies gamma a second time.

## Shader System

`ProgramLibrary` with a two-level cache: variant key -> source composition ->
compiled binary. **ShaderChunks registry** (`shader-lib/shaderChunks.h`): named
micro-chunk files (file stem = chunk name) concatenated per registered program
order with `#define VT_FEATURE_*` guards. Overridable globally via
`getProgramLibrary(device)->chunks().set(name, src)` and per material via
`Material::setShaderChunk(name, src)`; resolution is material > registry >
default. Both override sets' FNV content hashes fold into the variant cache key.
Metal chunks hot-reload from the source dir per launch.

**Both backends.** `engine/shaders/vulkan/chunks/` holds 18 GLSL **fragment**
chunks under the same names, and `forward.frag` is a 26-line file that `#include`s
them, so the build-time bundle and the runtime composition share one source.

- `ProgramLibrary` registers a separate GLSL chunk order that **must stay in step
  with `forward.frag`'s `#include` order**.
- Override source must be in the device's language (`GraphicsDevice::shaderLanguage()`).
- Vulkan hands composed GLSL to `createShader` only when an override actually
  changed it; otherwise it passes an empty string and gets the prebuilt bundle.
- `forward-vertex`, `shadow-vertex` and `shadow` have no chunked GLSL form and are
  Metal-only; overriding them on Vulkan logs a warning.
- **Fog has a TYPE** (`FogParams::type`, `Scene::setFogType`): NONE/LINEAR/EXP/
  EXP2, uploaded in `fogStartEndType.z`, where 0 also means off. It used to be a
  0/1 flag, which made EXP and EXP2 unreachable on both backends. Fog depth is
  the LINEAR VIEW DEPTH (`fragViewDepth` on Vulkan, `1 / rd.position.w` on
  Metal), not the radial distance to the camera. No example uses fog, so a change
  here has to be driven deliberately to be seen at all.
- **A pass that samples the depth it has attached must call
  `RenderPass::setDepthReadOnly(true)`.** Sampling an attachment is legal only
  when it is bound read-only, and a combined image sampler can never be given
  `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`; the Vulkan backend uses the flag for the
  attachment, its transition and the descriptor alike.
  `RenderPassVolumetricFogCombine` is the one that does this today. The flag
  cannot be inferred: the depth ops set `storeDepth` to PRESERVE depth for later
  passes, which is indistinguishable from writing it, and the quad texture
  bindings are not known at `startRenderPass` because passes set them in
  `execute()`.
- **A screenshot comparison is only valid against a reference built from the
  SAME example source.** Re-instrumenting an example, then diffing the result
  against a screenshot taken before the instrumentation, reads every unrelated
  difference as a regression. That mistake invented a "sheen feature-resolution
  bug" on 2026-09-06 (see the ENGINEERING-LOG correction) and cost a full
  withdraw-and-reland cycle. Capture the reference and the change from one source
  state, in the same session.
- **When a probe says a uniform is stuck, check the SETTER first.** A
  cascade-blend "plumbing bug" was diagnosed and documented on 2026-09-06 that
  did not exist: the test hook setting it had been placed two lines above the
  example's own setter, which overwrote it, so both runs used the same value. A
  one-line `spdlog` of what the binder receives settles this class of question in
  one run; a shader probe puts a transfer curve and a bundle rebuild between you
  and the answer.
- **`common-brdf.glsl` is the twin of `common-brdf.metal` and both must change
  together.** It owns `distributionGGX`, `getVisibilitySmithGGX` (a VISIBILITY
  term — the `1/(4 NdotL NdotV)` is folded in, so call sites write `D * Vis * F`
  with no division), `getFresnel` (gloss-aware, DIRECTIONAL lights only —
  punctual lights take bare specularity), `getFresnelCC` and
  `getVisibilityKelemen`. A shading edit that lands in one language only is a
  backend divergence by construction; that is how a separable UE4 Smith term and
  an `F90 = 1` Schlick lived in `common-atmosphere.glsl` while the IBL paths used
  a third, correct spelling.
- Keep each GLSL chunk a self-contained override target. The material-flag
  constants and `applyUvTransform` live in `common-material-flags`, not in
  `common-tonemap`, so a minimal tonemap override does not drop them.

**Fullscreen effects use `QuadRender`** (`scene/graphics/quadRender.h`), not
device virtuals: a shader, up to 8 input textures on fragment slots 0-7, and one
uniform block. The block rides the per-draw MATERIAL slot (Metal buffer 3 / Vulkan
set 0 binding 0) via `GraphicsDevice::setQuadUniformData`; `kPerDrawUniformCapacity`
(512) sizes that slot, the Vulkan material descriptor's range, and the padded
allocation behind it. A smaller block is copied into the front of a full-size
allocation, so never shorten the allocation. Quad passes draw an oversized
fullscreen TRIANGLE and bind `_postSampler` (linear, clamp, no mip), not the scene
sampler.

Migrated: VSM blur, volumetric fog, CoC, DOF blur, depth-aware blur, compose,
SSAO, TAA, the whole env family (equirect-to-cube, reproject, convolve, atlas —
see `scene/graphics/envBake.h`), and the GPU particle simulation, which now runs
over the generic `Compute` seam from `scene/particles/particleSimShaders.h`.
**The effect-pass migration is DONE.**

What is still virtual is not debt. `copyRenderTarget` and `generateMipmaps` are
generic device operations. `setParticleState`, `setGSplatState` and `setMorphState`
bind per-draw resources at fixed slots — the same job as `setVertexBuffer` — and
stay three named calls rather than one tagged call because they differ in arity and
own different slots (particle and gsplat share 7/11, morph uses 9/10).

A compute effect goes through `Compute` + `GraphicsDevice::computeDispatch`, not a
new virtual. Parameters bind by NAME in sorted order (see `compute.h`); when the
parameters are a struct rather than a few scalars, use `Compute::setUniformBlock`
to supply the block verbatim instead of naming 44 floats whose order would then
depend on their spelling.

**Offline (out-of-frame) work** goes through `GraphicsDevice::beginOfflineWork` /
`endOfflineWork`. Between them the ordinary render-pass and draw API is usable, so
a bake is written once over QuadRender. The two backends differ deliberately:
Metal only batches the work into one command buffer and commits WITHOUT waiting
(its `startRenderPass` already makes a command buffer per pass, and
`reflection-probe-dynamic` re-bakes every frame, where a stall would serialise CPU
and GPU); Vulkan records into a one-shot buffer and WAITS, because that is what
makes reusing the frame-scoped uniform ring and descriptor pools safe. A bake must
also set its own blend, depth and cull state — nothing outside the frame graph
has — and `beginOfflineWork` flushes pending uploads first, because a texture
created without host data marks its tracker SHADER_READ_ONLY while the actual
transition is still sitting in the deferred upload queue.

## Examples

**Port an upstream example; do not invent one.** Upstream's example set is the
reference for what a feature demo should show, and a scene invented here cannot be
compared against anything. The upstream sources are at
`~/sources/visualization/playcanvas-engine/examples/src/examples/<category>/`;
read the `.mjs` and match its scene, poses, materials and parameters, then record
any DEVIATION in the file header where the port could not follow. Only where
upstream genuinely has no counterpart is a new scene the right answer, and that
should be said out loud in the header.

## Live gotchas

Each of these has cost real time. See `ENGINEERING-LOG.md` for the incidents.

- **A target-only Vulkan build does NOT regenerate the SPIR-V bundle.** After
  touching ANY GLSL chunk, `touch engine/shaders/vulkan/forward.frag` and build
  the whole `build-vulkan` target, or you measure the previous binary. This
  produced two separate false conclusions in one session.
- **The multi-pass DOF path is DEAD CODE.** `RenderPassCameraFrame::setupDofPass()`
  only calls `_dofPass.reset()`, so `RenderPassDof`, `RenderPassCoC` and
  `RenderPassDofBlur` are never constructed. Depth of field runs entirely through
  `applyDofSinglePass` in the compose shader. Screenshot-verifying those passes
  proves NOTHING. Reviving the path means giving `RenderPassDof` a render target.
- **A Metal quad draw must not key its uniform allocation on the material.**
  `submitPerDrawUniforms` reuses the previous ring offset when the material pointer
  is unchanged, and a quad pass has no material of its own — nothing clears the
  bound material for an offline bake either — so every quad draw after the first in
  a pass silently shared ONE uniform block. Invisible while a pass drew a single
  quad, which every effect did until the env atlas started drawing a rect list in
  one pass: the convolve draws read the reproject block and wrote nothing. `draw()`
  now passes a null material for the uniform key whenever the quad block is in use.
- **A shader that exists in MSL and GLSL is only shared by CONVENTION.** The two
  bodies in a `*Shaders.h` sit in separate raw strings, and a migration that
  unified the uniform BLOCK does not unify the code. Compose carried three stages
  whose GLSL was an older, cruder implementation — colour enhance, colour grading
  and the 3D LUT — for as long as the file has existed. Before blaming a backend's
  lighting for a brightness gap, read the two bodies of the shader that produced
  the pixel side by side.
- **`atan2(0, 0)` is undefined, and a normal of exactly +/-Y hits it** — which is
  every fragment of an unrotated ground plane, the most common surface there is.
  Metal returned an out-of-range azimuth, so `mapAmbientUv` mapped outside its
  rect and the plane read its irradiance from the ROUGHNESS column instead: a
  ground plane lit by a blue sky came back dark navy. Both `toSphericalUv` and
  `dirToEquirect` now pick azimuth 0 at the pole. Any new direction-to-equirect
  code owes the same guard.
- **An unbound Metal texture reports nonzero `get_width()` but samples zero** on
  Apple GPUs. Every optional texture sample must be gated on its flags bit or its
  runtime enable (`setEnvAtlasEnabled`, `hasSpecGlossMap` bit 21). This has bitten
  three times.
- **Depth taps in a quad pass must be POINT sampled, and neither backend does it
  for free.** A quad pass reconstructs view-space positions from depth, and a
  bilinear tap straddling a silhouette returns a depth belonging to neither
  surface — a position in mid-air the kernel then treats as an occluder. Whether
  hardware filters a depth format at all is a per-format capability, so leaving it
  to the texture's own sampler gave SSAO linear taps on Metal and part-nearest
  taps on Vulkan. Vulkan now binds `_shadowSampler` (nearest, clamp, mip-less) for
  any depth texture in the quad path and the MSL passes declare their own point
  sampler. Establish this kind of thing by making the shader REPORT it: sample at
  a texel centre, one texel across, and exactly halfway, then check whether the
  halfway tap is the average. Reading the sampler-creation code is not enough.
- **Screen-space derivatives are undefined inside the per-light loop**, which sits
  behind fragment-varying `continue`s. An undefined mip LOD reads a fully averaged
  mip — a heart-shaped cookie became a flat wash of its own average. Sample with
  an explicit LOD 0 (`level(0)` / `textureLod`).
- **A clustered SPOT must not consume one of the two main local shadow slots.**
  Its shadow comes from the LightTextureAtlas, and the main-array allocation clears
  `castShadows` when it runs out — which silently capped clustered spot shadows at
  `ShadowParams::kMaxLocalShadows` (two), whatever the atlas capacity said. Ten
  lights, two of them with any chance of a shadow, and nothing anywhere said so.
- **A clustered spot shadow takes NO depth bias in the shader.** Its projection has
  near 0.01 against a range of 150, which crushes the whole scene into about 0.001
  of depth; the non-clustered path's spot bias (upstream's `shadowBias * 20`, so
  0.08 at an authoring value of 0.4) is eighty times that range and lights every
  fragment. Upstream says so in one line of comment — "depth bias is already applied
  on render" — and biases these with hardware polygon offset, which the atlas pass
  already sets. What the shader applies is the receiver NORMAL offset, and
  `ClusterLightData::shadowNormalBias` carries it.
- **Spot cone falloff is a SMOOTHSTEP between the two cone cosines**, and local
  inverse-squared falloff is `16 / (d^2 + 1)`, not `1 / d^2`. Upstream's `spot.js`
  and `getFalloffInvSquared` define both and the Metal chunks follow them; Vulkan
  had a squared linear ramp for the cone (half the light at the middle of the
  penumbra, agreeing only at the two ends) and a bare inverse square (a light at
  four units read a fifteenth of upstream). Both fixed 2026-09-05. Vulkan carried
  THREE spellings of the cone — non-clustered, clustered, and none shared — so
  `getSpotEffect` now lives beside `distanceAttenuation` in `common-material-flags`
  and both call sites use it.
- **Spot cone angles are HALF-angles** (upstream: `cos(outerConeAngle * DEG_TO_RAD)`,
  shadow and cookie cameras use `fov = outerConeAngle * 2`). Do not halve them
  again in `renderer.cpp` or `worldClusters.cpp`.
- **An omni light's shadow casters are classified ONCE for all six faces**, in
  `omniShadowCasterClassification.cpp`, before the frame graph runs; each face pass
  then draws `LightRenderData::visibleCasters` with no culling of its own. The
  classification relies on the face cameras looking down +X, -X, +Y, -Y, +Z, -Z in
  that order. Nothing in `LightCamera::pointLightRotations` announces that — it is
  six Euler triples — and reordering them would still render six shadow maps, just
  with casters on the wrong faces. `tests/omniFaceAxisTests.cpp` is what holds the
  order; if it fails, fix the table or rewrite the classification, do not adjust
  the test.
- **Omni shadow bias is RELATIVE** — a fraction (0.2%, `omniShadowParams[2]`) of
  the receiver distance applied BEFORE the perspective projection. Cubemap shadow
  depth is crushed against 1.0, so a fixed post-projection offset erases omni
  shadows entirely at ordinary light ranges.
- **`StandardMaterial` overwrites the base-Material factors.** Set surface
  properties with `setDiffuse` / `setMetalness` / `setGloss` (+ `setGlossInvert`);
  `setBaseColorFactor` / `setMetallicFactor` / `setRoughnessFactor` are overwritten
  by `updateUniforms` when no base-color texture is present.
- **Ambient occlusion occludes the AMBIENT diffuse by default, the direct diffuse
  and a lightmap only under `occludeDirect`, and the specular through
  `occludeSpecular` mode and intensity.** That is upstream's split and both
  chunks follow it. `StandardMaterial::aoMap` and `Material::occlusionTexture`
  are one texture slot and one shader feature; the GLB parser fills the base
  property and `setAoMap` writes through to it. Clearing only one of them used
  to clear nothing, which is how the ambient-occlusion example rendered with
  its "disabled" baked AO for as long as it existed.
- **Material colours are authored in GAMMA space** and owe the shader a decode.
  The split is per-source, not per-material: `setDiffuse` stores raw, so the base
  colour FACTOR is decoded in the shader, while `setEmissive` is pre-linearised by
  `updateUniforms`, so the emissive factor is NOT. Every TEXTURE is authored in
  sRGB and is decoded, base colour and emissive alike. Getting one half wrong is
  invisible until a scene leans on it: a missing emissive-map decode left the
  `depth-of-field` room 1.6x too bright on Vulkan while every other scene looked
  fine, because only that scene has large emissive surfaces.
- **Do not draw to the back buffer after `Engine::render()`** — `frameEnd`
  presents the drawable and a stale `_frameDrawable` reuse is a pointer-auth
  SIGSEGV. Use `Renderer::addAppendPass` to append app passes to the frame graph.
- **`Compute` parameter binding is by NAME in sorted order**, because this port has
  no shader reflection: buffers 0..b-1, textures b..b+t-1, the uniform block at
  b+t, and the block's members are the scalars again in name order. A shader that
  declares them in a different order silently reads the wrong data.
- **Only ONE SIMD backend compiles per target, so a defect in another one is
  invisible here.** Apple silicon selects the Apple backend; the SSE path is now
  gated on `__SSE4_1__` (it uses `_mm_dp_ps` / `_mm_insert_ps`, so `__SSE__`
  alone could not compile) and x86 without SSE4.1 falls through to scalar. Two
  wrong SSE horizontal sums lived in `Vector2::dot` and
  `Vector4::planeNormalize` for as long as the files existed. When you touch one
  backend, check the same function in the other three, and add the contract to
  `tests/simdMathTests.cpp` — which only covers the backend the build selected,
  so an x86 CI build is what would actually guard the SSE path.
- **A block-compressed format must be asked for, not assumed, and there are
  FOUR KTX2 call sites.** `GraphicsDevice::preferredCompressedRgbaFormat()`
  picks ASTC → BC7 → DXT5 → RGBA8 from `supportsCompressedFormat()`; ASTC is
  Apple-only and BC is desktop-only, and the image creation fails rather than
  degrades. The target is chosen on the MAIN thread and passed to the worker
  (`asset.cpp` — the path examples use, `resourceLoader.cpp`, and two in
  `glbParser.cpp` for `KHR_texture_basisu`). Changing one changes nothing.
- **`pixelFormatInfo` is a map the `PixelFormat` enum does not enforce.** An
  enumerator with no entry makes `pixelFormatBytesPerPixel()` return 0, which
  the Vulkan upload path uses to size its staging copy.
  `tests/pixelFormatTests.cpp` lists every enumerator by hand — add a format
  there when you add one to the enum.
- **Transparent draws sort on SIGNED view-axis depth, not radial distance**
  (`scene/renderer/sortDistance.h`, upstream's `_calculateSortDistances`).
  Radial distance ranks an off-axis surface farther than a centred one at the
  same depth by up to 1/cos(fov/2), and cannot tell behind from in front.
  `MeshInstance::setCalculateSortDistance` overrides it per instance.
- **Leftover instance bindings follow the next draw.** The backends pick the
  instancing vertex layout by scanning bound slots, so shadow passes must unbind
  slot 5 after an instanced caster.
- **metal-cpp framework extern constants** (e.g. `MTL::CommonCounterSetTimestamp`)
  only link in the `*_PRIVATE_IMPLEMENTATION` TU. Compare string values instead
  inside the engine library.
- **`Matrix4::getElement` takes (col, row)**, not (row, col).
- **A hand-built sphere's triangle winding has to be counter-clockwise seen from
  OUTSIDE**, or its normals face inward. A mirror ball HIDES this — it still
  reflects something — so the inverted winding in the reflection-probe example went
  unnoticed until the same generator was reused with a diffuse material in
  mesh-morph and came out black. `DEBUGPASS_WORLDNORMAL` says it in one frame: a
  correct sphere is blue in the middle, an inverted one is not.
- **Large ground planes must stay shadow CASTERS but not receivers-only.** The
  directional shadow camera fits its depth range to casters, so a receiver-only
  ground falls outside it and catches no shadow; a huge caster inflates the fitted
  range into whole-plane acne (PCF) or blown-up penumbras (PCSS).

## Measuring a backend divergence

Whole-frame mean luminance is a BAD signal: scenes animate, content differs, and
the tonemap compresses whatever you are chasing. A mean over a symmetric REGION is
just as bad in a different way — it is invariant under a mirror, which is how a
horizontally flipped Vulkan sky measured 0.9999 against Metal for as long as the
backend has existed. Split every region you measure into halves, and when two
halves diverge in opposite directions, test the mirror before theorising. Instead:

1. Split the frame with `Camera::setDebugShaderPass` — `DEBUGPASS_ALBEDO` and
   `DEBUGPASS_LIGHTING` separate the material frontend from the lighting, and both
   are wired on both backends.
2. Strip the scene one term at a time (light off, no ambient, no env atlas) until
   the two backends agree, then add terms back.
3. A constant ratio across all three channels is a single scalar bug, not a colour
   or texture bug.
4. Screenshot capture is in-engine on both backends via the `VISUTWIN_SCREENSHOT`
   env var; drive examples with `run_example.py`.

Animated examples cannot be screenshot-diffed across shader changes.

## Feature notes

Per-subsystem detail lives in `ARCHITECTURE.md`: how each feature works, the call
that turns it on, and its deviations from upstream. Only the parts that bite
during unrelated work are repeated here.

- **Shadow bias convention.** `LightComponent::setShadowBias` takes upstream's
  0..1 authoring value (default 0.05) and remaps it to `Light::shadowBias` as
  `-0.01 * clamp(v,0,1)` — negative on purpose, because the passes apply
  `shadowBias * -1000` as hardware polygon offset and that product must be
  POSITIVE to push casters away from the light. Passing the component value
  straight through inverts it, so a larger bias produces MORE shadow. Hardware
  bias is skipped for PCSS and for non-clustered omni. The directional PCF shader
  uses a fixed 0.0001 receiver bias, NOT the light's.
- **PCSS `penumbraSize` has two scales.** Directional is world-space (0.02-0.05);
  local spot and omni is in shadow-map PIXELS (~10-40).
- **A lightmap REPLACES indirect diffuse**, it is not added. Upstream gates
  ambient behind `addAmbient = !lightMapEnabled`, and adding both double-counts
  what the bake already contains.
- **A captured probe cube is GAMMA-encoded and owes a decode**, like every other
  texture the shader reads. It also needs the engine's cube-convention X flip, a
  gloss-aware Fresnel rather than a raw F0 multiply, and box projection re-aimed
  from the BOX CENTRE (normalised), not the probe's position. Missing the decode
  alone washes every metallic surface out to pale pastel. And because the probe
  REPLACES the environment specular, add only `probe - indirectSpecular` to the
  accumulated colour; adding the probe outright counts both.
- **Construct a dynamic `ReflectionProbe` BEFORE the main camera.** Layer
  composition renders cameras in construction order, so the six face cameras must
  come first. The reflective object also has to sit on a layer excluded from the
  probe's capture layers, or it self-captures.
- **A wide line is one instance per SEGMENT, expanded in the vertex shader.** The
  template geometry (quad body, two discs for round caps and joins, two bevel
  triangles) comes from the VERTEX ID rather than a vertex buffer, so every
  instance draws the same vertex count and a piece the current style does not want
  collapses to zero size instead of being skipped. Widths are screen pixels by
  default, which is why the expansion happens after the projection rather than in
  world space.
- **GPU instance culling requires the 80-byte stride.** Its kernel compacts fixed
  80-byte records. The instanced shader variant follows THE DRAW, not the
  material: the renderer derives it from the mesh instance's buffer format.
- **Parallax `heightMapBase` shifts the ray's ENTRY UV, not just the depths.**
  The base is the height-map value that sits at the level of the geometry, so
  anything above it lifts off the polygon and the view ray has to enter higher up.
  Offsetting only the depths moves the ray and the field by the same amount and
  changes nothing at all — the images come back bit-identical, which is how the
  first attempt at this looked like it worked. The entry UV must move by the
  lateral distance the ray covers over that height. Base 1 is pure depth below the
  surface; the default 0.5 pivots around mid-grey, as upstream.
- **`heightMapFactor` is in TENTHS of a uv tile**, upstream's unit: a factor of 1
  asks for a relief 0.1 uv deep. Used raw, upstream's own tuned value of 0.4 smears
  brickwork into spikes, which is what the parallax-mapping port showed.
- **Parallax self-shadowing costs a second march and only the DIRECTIONAL light
  pays it.** It runs inside the light loop, which is behind fragment-varying
  control flow, so its height taps use an explicit LOD; the view march sits in
  uniform flow at the top of the shader and keeps implicit LOD so the height map
  keeps its mips. `setHeightMapShadow` defaults to 0, so nothing pays unless asked.
- **Opacity dither is an opaque-pass technique.** Keep the material
  non-transparent; alpha comes from `setOpacity` or texture alpha. `setAlphaDither`
  decouples dither density from opacity and rides in `dispersionParams.y`, where
  NEGATIVE means unset.
- **The gloss, thickness and refraction scalar maps are Metal only**, blocked on
  Vulkan by the 16-sampler limit. The uniform field is plumbed on both backends,
  so Vulkan renders the scene correctly minus the maps.
- **`detailNormalScale` blends the DETAIL map toward flat too, and the two normals
  combine with a REORIENTED blend, not by adding xy.** Adding xy treats the detail
  slope as if the base were flat, so the combined slope is wrong wherever the base
  is not. Upstream's `blendNormals` rotates the detail into the base normal's frame:
  `n1 = base + (0,0,1)`, `n2 = detail * (-1,-1,1)`, `n1 * dot(n1,n2) / n1.z - n2`,
  left unnormalized because the TBN product is normalized after. Ported to both
  backends 2026-09-05; it was the same defect on both, so this is a correctness fix
  and not an alignment one.
- **`normalScale` blends the sampled normal TOWARD FLAT.** It does not scale xy.
  Upstream's `material_bumpiness` is a `mix(vec3(0,0,1), normalMap, s)` and both
  backends now do that. Scaling xy leaves z alone, so it steepens the normal's
  slope exactly where the mix flattens it; the two agree only at 0 and 1, which is
  why the default of 1 hid this. Aligned 2026-09-05 — the earlier note here, that
  Vulkan held upstream's form and aligning would move every Metal scene, was wrong
  on both counts. There is also no Gram-Schmidt re-orthonormalization of the
  interpolated tangent any more: upstream normalizes the tangent and binormal and
  does nothing else. It measured as a no-op, so if a mesh ever shows tangent skew,
  add it back to BOTH backends rather than one.
- **The GPU lightmapper has no cast shadows yet.** It captures direct light and
  ambient only. The CPU `Lightmapper` is the quality reference.


### Atmosphere (Nishita)
Two traps, both of which silently produce no sky at all:
- **`Scene::setAtmosphereEnabled` must rebuild the sky mesh.** The atmosphere
  branch of `Sky::updateSkyMesh` requires the flag to be set ALREADY, and every
  caller writes `setSkyType` then `setAtmosphereEnabled`. The setter now calls
  `resetSkyMesh()`.
- **`planetCenterAndRadius.xyz` is CAMERA-LOCAL.** A viewer on the surface needs
  the centre one radius BELOW: `{0, -6371000, 0}`. At the origin the viewer sits at
  the planet's core and every ray starts underground.

With the ray stuck underground, changing sun direction or intensity looks like it
does nothing, which is a misleading symptom.

## Open items

- **Vulkan reflections are slightly SOFTER than Metal's.** Re-measured 2026-09-05
  with the scene frozen: `reflection-probe` matches to 0.3% in the mean, but the
  reflection carries 6.5% less horizontal gradient energy where a direct texture
  on the same frame carries 0.4% less. Hardware trilinear mips approximate the GGX
  prefilter upstream bakes per level, and the two backends round it differently.
  Not worth chasing unless a scene shows it. The probes themselves are no longer
  suspect: with the sky fixed, a probe's captured sky matches Metal exactly.
- **`ambient-occlusion-davinci`'s floor still reads ~0.92x Metal in RED** (0.96
  green, 0.98 blue), down from 0.81 once the depth tap was aligned. Its sky
  matches exactly, and its whole-frame difference is under 1/255, so the red gap
  lives in the darkest part of the floor where a 2-count difference is a large
  ratio. NOT normal mapping: aligning `normalScale` left this scene bit-identical.
  The bilateral blur multiplies whatever the SSAO pass disagrees about by roughly
  2.5, so an input difference worth 3% shows up as 8%.
- **`tools/generate-env-atlas` still does not produce a usable image.** Its
  readback calls `MTL::Texture::getBytes` on the baked atlas, which is a private-
  storage render target, so the values come back wrong even though the layout is
  now visibly correct. Fixing it means blitting to a shared staging texture first.
  Until then, verify an atlas change with the `reflection-probe-dynamic` atlas
  panel (NDC centre (0, -0.7), size (0.5, 0.4)) cropped and magnified — that panel
  is at a fixed screen position, so it compares cleanly even though the scene
  animates.
- **Example coverage gaps**: gsplat SH bands 1-3 have no example; upstream has the
  `gaussian-splatting/` folder. Detail normals have no example either, and upstream's
  (`test/detail-map`, itself flagged HIDDEN) cannot be ported faithfully yet: it
  toggles diffuse, normal and AO detail maps and only the NORMAL one exists here. Morph weight animation is covered again as of
  2026-09-05 (`mesh-morph-example.cpp`), and clustered atlas shadows have a working
  example (`clustered-spot-shadows-example.cpp`).
- **The last of the Vulkan/Metal light gap is the INDIRECT term.** With the spot
  cone and inverse-squared falloff fixed, `parallax-mapping` went from 0.93 to
  0.997 and its direct spot light matches to 1.0000. What is left: with every
  light disabled so only the environment contributes, that scene reads 0.85 of
  Metal. Treat the number with suspicion before chasing it — the frame is 20-30
  counts there and the tonemap is not linear in that range, so re-measure it
  against a brighter configuration first.
- **`clustered-spot-shadows` still differs by 19/255 on its normal-mapped cube
  faces.** Not `normalScale`, which is now aligned and which that scene leaves at
  the default anyway. Re-measure it: it predates both the spot cone fix and the
  falloff fix, and its lights are spots.

## Reference kept elsewhere

`ARCHITECTURE.md` holds the per-subsystem detail: the examples harness and its two
backend rules, layers and depth state, the ECS, the graphics abstraction, and the
asset pipeline. The rules from those sections that matter during unrelated work
are in the gotcha list above.

## SIMD Math

Multi-backend: scalar, SSE, Apple SIMD, NEON. Controlled via `USE_SIMD_MATH` /
`USE_SIMD_PREFER_NEON` in `defines.h`. `USE_SIMD_MATH` **is** set, so on Apple
Silicon the Apple SIMD backend is the active path and scalar is the fallback.


## Coding Conventions

- `shared_ptr` for ownership (replaces JS GC), raw pointers for non-owning references
- `_camelCase` for private members
- `camelCase()` for getters, `setCamelCase()` for setters
- `DEVIATION:` comments where diverging from upstream behaviour or algorithms.
  Put the deviation at the code it describes; this file records only the ones that
  would change a design decision before you open a file.
- In-code comments refer to PlayCanvas as **upstream** (never by name — the source
  is clean of the name apart from the `playcanvas-grey` / `playcanvas-cube` asset
  filenames; attribution lives in `NOTICE` and the README, which is where the MIT
  obligation is discharged)
- Shader features: `VT_FEATURE_*` prefix (not upstream `PC_*`), declared once in
  `platform/graphics/shaderFeatures.h`
