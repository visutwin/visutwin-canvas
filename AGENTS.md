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
  numbers for work already finished. NOT tracked in the repository: it is a local
  working record, so a fresh clone will not have it and nothing here may depend
  on it. Every rule that still binds is in THIS file; the log only says how a
  thing was measured and what was already ruled out. Keep appending to it where
  it exists.
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
  examples/        # 46 example applications derived from ExampleApp: upstream ports + one original scene (ambient-occlusion-davinci)
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
cmake --build --preset default
ctest --preset default
```

- **Test presets exclude the `vulkan` label.** `vulkan-validation-smoke` needs a
  GPU and a window, so `ctest --preset default` and `ctest --preset vulkan` run
  the unit tests only, and `ctest --preset vulkan-smoke` runs the smoke test on
  its own. Every preset errors when it matches no tests, so a broken filter fails
  rather than passing on nothing.
- **CI is `.github/workflows/ci.yml`**, three jobs: `simd-backends` compiles the
  header-only SIMD contract test once per backend (SSE and scalar on Linux x86,
  NEON and Apple on macOS arm64) with no vcpkg; `macos-metal` builds the `default`
  preset and runs its tests; `linux-vulkan` builds the `vulkan` preset with GCC 14
  and runs its unit tests. All three are gating; `linux-vulkan` was verified on
  an Ubuntu 24.04-based machine with the same presets and packages before it was.
- **`VISUTWIN_EXPECT_SIMD_BACKEND=sse|neon|apple|scalar` makes the SIMD test FAIL
  unless that backend is the one `defines.h` selected.** Only one backend compiles
  per build, and a missing flag falls through to another backend silently and
  still passes — testing the wrong code. Two traps it has already caught: Apple's
  x86-64 default baseline includes SSE4.1, so an x86 build meant to be scalar is
  SSE on macOS; and any x86 build on macOS without SSE4.1 selects the APPLE
  backend, so a scalar build cannot be produced on macOS at all.
- **Linking Jolt makes the whole engine an AVX2 build on x86.** vcpkg's
  `JoltConfig.cmake` exports `-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma` as
  INTERFACE compile options, so every target linking the engine — tests included —
  compiles the SSE backend and needs an AVX2 CPU, whatever the compiler's default
  baseline is. The `linux-vulkan` job therefore tests SSE, and only the
  `simd-backends` matrix builds scalar. A pre-Haswell x86 machine would SIGILL.
- **A system package a dependency only RECOMMENDS is not reliably there on CI.**
  vcpkg's `libxcrypt` port (pulled in transitively on Linux) refuses to configure
  without `libltdl-dev`. On a clean Ubuntu 24.04, `apt-get install libtool` brings
  it in as a Recommends, so a container reproduction passes; GitHub's runner image
  ships `libtool` preinstalled, the install is a no-op, and the job fails in
  configure. Name every package a port asks for explicitly in `ci.yml`. The
  failure log is only visible when signed in to GitHub (`gh run view <id>
  --log-failed`); the public API gives step names and an exit code, nothing more.
- **The `vulkan` preset needs a SYSTEM Vulkan loader.** vcpkg's `vulkan-headers`
  port supplies headers only, and `find_package(Vulkan)` wants the unversioned
  library: `libvulkan-dev` on Ubuntu (the runtime package ships only
  `libvulkan.so.1`), the Vulkan SDK's `libvulkan.dylib` on macOS. `glslc` and
  `spirv-cross` come from vcpkg's own `shaderc` and `spirv-cross` tools.
- **tinygltf 2.9.7 builds from an OVERLAY port** in `vcpkg-overlays/ports/`,
  registered in `vcpkg.json`. GitHub regenerated the tag's source archive, so the
  baseline port's SHA512 fails on any machine without tinygltf cached — which is
  every fresh CI runner. The overlay differs from the baseline port only in that
  hash, and the regenerated archive was checked file for file against the tag.
  vcpkg never re-hashed 2.9.7 (upstream moved to 3.0.0), so delete the overlay
  when the baseline is bumped past it.
- **Apple's libc++ hides missing standard includes; GCC's libstdc++ does not.**
  libc++ pulls `<cmath>`, `<cstdint>` and `<array>` in transitively, so a header
  that uses `std::sqrt`, `uint32_t` or `std::array` without including them builds
  on macOS and fails on Linux. The first Linux build found eight such headers, and
  one of them cascaded into fifty failed files. Include what you use; the Linux CI
  job is what catches it now.
- `vcpkg.json` qualifies ImGui's `metal-binding` feature to `osx`. The port
  declares it macOS-only, and an unqualified feature makes the whole manifest
  unresolvable on Linux.

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
MSAA, GPU profiling. GLSL in `shaders/vulkan/` is compiled to SPIR-V at build time and
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
`materialSlots` clear list in `bindMaterialTextures`. Slots 0-34 are taken today
(34 is the opacity map, Metal only).
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

## Input

The engine owns the input DEVICES; the application owns the event loop.

    appOptions.keyboard = std::make_shared<Keyboard>();
    appOptions.mouse    = std::make_shared<Mouse>();
    appOptions.touch    = std::make_shared<TouchDevice>();
    appOptions.gamepads = std::make_shared<GamePads>();
    // then, for every event the application polls:
    engine->handleInputEvent(event);

- **A device with no events fed to it reports nothing and breaks nothing.** Every
  accessor is safe on an absent device, which is what lets a script read
  `entity()->engine()->keyboard()` without the application having supplied one.
- **`Engine::inputUpdate` ends each device's frame**, after the app's update and the
  script phases. That is what makes `wasPressed` / `wasReleased` mean "during the
  frame that just ran", so nothing else may call `update()` on a device.
- **The edges record TRANSITIONS, not a snapshot comparison.** DEVIATION from
  upstream, which diffs this frame's key map against last frame's and therefore
  cannot see a key pressed AND released inside one frame. Auto-repeat is not a new
  press; the edge is recorded only when the key was actually up.
- **Losing window focus releases everything held.** A key or button held while
  focus moves away never sends its release, and would otherwise read as held for
  the rest of the process. The release EDGE is recorded too, so a caller watching
  for it is not left waiting.
- **`Key` enumerators ARE SDL scancodes**, so a key the list does not name still
  works through a cast, and the list needs no translation table to get wrong. They
  are POSITIONAL: `Key::W` is the key left of `Key::S` whatever it types.
- **Touch positions need the window size.** SDL reports normalized coordinates;
  `TouchDevice::setWindowSize` converts to pixels and Engine keeps it current from
  the resize event. Without it every touch lands in the top-left corner.
- Examples must not poll SDL for input. `CameraControls` reads the engine's
  keyboard and mouse; the old six-key `platform/input.h` sink is gone.

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
  CPU sweep over collision bounds that predates the seam. Every physics example
  (`falling-shapes`, `physics-joints`, `raycast`) supplies the Jolt world, as
  upstream's use a real physics engine, so only a test reaches the fallback.
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
- **Verify the measurement before believing the finding.** Two "bugs" were
  diagnosed, documented and withdrawn on 2026-09-06 that did not exist, both from
  a bad experiment rather than bad code. Three rules, each of which would have
  caught one: capture the reference and the change from ONE source state (a
  screenshot taken before an example was instrumented reads every unrelated
  difference as a regression); check the STIMULUS actually reached the code (a
  test hook placed above the example's own setter was overwritten every run); and
  prefer a one-line `spdlog` of what the binder receives over a shader probe,
  which puts a transfer curve and a bundle rebuild between you and the answer.
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
new virtual. Parameters bind by NAME in sorted order, at per-backend indices for
which the table in `compute.h` is the contract; when the parameters are a struct
rather than a few scalars, use `Compute::setUniformBlock` to supply the block
verbatim instead of naming 44 floats whose order would then depend on their
spelling.

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

Each of these has cost real time, and each is self-contained — the incident that
produced it is recorded in the local `ENGINEERING-LOG.md` where that file is
present, but the rule below never depends on reading it.

- **A target-only Vulkan build does NOT regenerate the SPIR-V bundle.** After
  touching ANY GLSL chunk, `touch engine/shaders/vulkan/forward.frag` and build
  the whole `build-vulkan` target, or you measure the previous binary. This
  produced two separate false conclusions in one session.
- **The multi-pass DOF path is DEAD CODE.** `RenderPassCameraFrame::setupDofPass()`
  only calls `_dofPass.reset()`, so `RenderPassDof`, `RenderPassCoC` and
  `RenderPassDofBlur` are never constructed. Depth of field runs entirely through
  `applyDofSinglePass` in the compose shader. Screenshot-verifying those passes
  proves NOTHING. Reviving the path means giving `RenderPassDof` a render target.
- **Reading a texture back goes through `Texture::read`, and reading one the GPU
  wrote means reading it through STAGING.** The seam is `Texture::read` →
  `gpu::HardwareTexture::read`: Metal blits into a shared-storage texture, Vulkan
  copies into a host-visible buffer on a one-shot command buffer, and both BLOCK.
  Reaching past it for the native handle is the trap, because neither backend
  reports the mistake — `MTL::Texture::getBytes` on a device-private render target
  does not fail, it answers with whatever is mapped, and what comes back is
  plausible rather than blank. `tools/generate-env-atlas` did exactly that and
  wrote a wrong PNG for as long as it existed. Round-trip a known pattern to tell
  a working readback from a convincing one; `tests/vulkanSmoke.cpp` does, under
  validation, which also checks that the read hands the subresource back in the
  layout it borrowed. The Vulkan path refuses outright while a frame or an offline
  scope is recording: that work is not submitted, so a one-shot read would run
  ahead of the very commands whose output is being asked for.
- **An offline bake still has to run INSIDE a frame.** `beginOfflineWork` batches
  the bake's own command buffer, but the per-draw uniform RINGS are frame-scoped —
  `frameStart` is what hands out the region a quad draw writes its block into.
  `tools/generate-env-atlas` ran the whole env bake with no frame open, so every
  quad draw read the same unwritten block and the atlas came out ONE FLAT COLOUR
  with its rect layout visibly correct, which is why it read for so long as a
  readback problem. `reflection-probe-dynamic`, the only other caller, bakes
  inside its frame and was never affected.
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
- **stb_image's vertical-flip flag is set ONLY through `StbVerticalFlipScope`**
  (`framework/assets/stbImageFlip.h`). stb keeps a global flag and a thread-local
  one, and once the thread-local flag is set on a thread it overrides the global
  one there for good — stb cannot unset it. The GLB parser decodes flipped through
  the thread-local flag, so any loader that cleared only the GLOBAL flag was
  ignored after a GLB on the same thread: first the environment atlas came out
  upside down, later every glyph of a bitmap font loaded after a GLB, and the OBJ
  parser's global "flip" was silently a no-op. The scope sets the thread-local flag
  and restores the enclosing value on exit, so a flip cannot leak into the next
  decode. Never call `stbi_set_flip_vertically_on_load*` directly, and never
  include `stb_image.h` from a header: stb guards its declarations but not its
  implementation, which `asset.cpp` compiles. `tests/stbImageFlipTests.cpp` loads
  an asymmetric atlas through the font path after a leaked flip.
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
- **Under clustered lighting NO local light enters the main light array.** Every
  spot and omni is in the cluster grid and its shadow comes from the
  LightTextureAtlas; the main-array allocation clears `castShadows` when its two
  slots run out, which is how clustered spot shadows were once capped at
  `ShadowParams::kMaxLocalShadows` and clustered omni shadows at two for as long as
  omnis went through the array. Clustered lighting is ON by default, as upstream,
  so a scene that needs the non-clustered path (PCSS local shadows, cookies) has to
  say `setClusteredLightingEnabled(false)`.
- **The shadow atlas is cleared per RECT, never by the pass.** It holds one-shot
  shadows of static lights beside realtime ones, so a load-action clear would erase
  the former every frame; each face clears its own viewport with a depth-1 triangle
  under the depth test at ALWAYS (`clearDepthRect`). Its rects are top-left origin
  like every texture here, and an omni face is rendered 3 px wider than 90 degrees
  with the shader UV inset to match — port both halves or the tile edges bleed.
- **A clustered spot shadow takes NO depth bias in the shader.** Its projection has
  near 0.01 against a range of 150, which crushes the whole scene into about 0.001
  of depth; the non-clustered path's spot bias (upstream's `shadowBias * 20`, so
  0.08 at an authoring value of 0.4) is eighty times that range and lights every
  fragment. Upstream says so in one line of comment — "depth bias is already applied
  on render" — and biases these with hardware polygon offset, which the atlas pass
  already sets. What the shader applies is the receiver NORMAL offset, and
  `ClusterLightData::shadowNormalBias` carries it.
- **One cluster grid per DISTINCT LIGHT SET, not one for the frame.** The local
  light list is per (camera, layer) — the gather filters on
  `LightComponent::rendersLayer` — so the grid has to be too.
  `Renderer::clustersForLightSet` keys pooled grids on an order-independent hash of
  the set, so two layers seeing the same lights still share one and it is still built
  once; `resetClusters()` drops the assignments each frame while the pool survives,
  because the cell buffers are too big to reallocate per frame. EVERY layer binds its
  own grid and params.

  What this replaced built ONE grid from whichever layer rendered first and skipped
  the whole block, BINDING INCLUDED, for every layer after — so a layer with a
  different light set was lit by another layer's cells, and a layer with NO clustered
  lights never reached the branch that zeroes the params and stayed lit by the
  previous layer's buffers. Both `clustered-lighting` and `clustered-spot-shadows`
  have exactly that shape: two distinct sets per frame, one of them empty.
- **The cluster grid is sized from the LIGHTS alone, and a spot is bounded by its
  CONE.** `WorldClusters::update` takes no camera: the bounds are the union of the
  light AABBs, as upstream's `evaluateBounds` does. They used to start from the camera
  padded by 50 units on every axis, so the grid was a 100-unit cube around the viewer
  wherever the lights actually were, and the cells came out coarse with most of them
  empty. Shrinking it loses no lighting — the shader ignores any fragment outside the
  grid, and a fragment outside the union of every light's bound is outside every
  light's range by construction, which is exactly what `lightBounds.h`'s containment
  property guarantees.

  `spotConeAabb` is the exact bound of the spherical sector, from its support
  function, and is a DEVIATION in the tighter direction from upstream's transformed
  box. The old range-sphere approximation was about thirty times the volume at a
  20-degree cone. Measured against sphere bounds in one process: the grid falls to
  0.16 of its volume on `clustered-spot-shadows` and 0.40 on `clustered-lighting`.
  `tests/lightBoundsTests.cpp` holds containment, tightness and monotonicity —
  a bound that is too small drops lighting and reads as a falloff, one that is too
  large reads as nothing at all, so neither is visible in a render.
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
- **`StandardMaterial` overwrites the base-Material factors, ALWAYS.** Set surface
  properties with `setDiffuse` / `setOpacity` / `setMetalness` / `setGloss`
  (+ `setGlossInvert`) / `setBumpiness`; `setBaseColorFactor` / `setMetallicFactor`
  / `setRoughnessFactor` / `setNormalScale` on a StandardMaterial never reach the
  GPU. This used to hold only when no base-colour texture was bound, so a GLB
  material — the parser binds its texture on the base Material — ignored every later
  scalar edit. The parsers write both sets, and `tests/standardMaterialWorkflowTests.cpp`
  pins the scalars applying with a texture bound.
- **A default `StandardMaterial` is in upstream's SPECULAR workflow and renders NO
  specular.** `useMetalness` defaults to false, `metalness` to 1 (read only once
  `useMetalness` is on), `specular` to black. `StandardMaterial::rendersSpecular()`
  is upstream's `useSpecular` rule — metalness on, a non-black specular, a spec-gloss
  map or clearcoat — and when it is false the material compiles
  `VT_FEATURE_NO_SPECULAR`, which removes direct, area, clustered and reflected
  specular on both backends. A black F0 is NOT the same thing: the gloss-aware
  Fresnel still reflects at grazing angles. So a scene that wants reflections from a
  code-created material has to say `setUseMetalness(true)`, as upstream's examples do;
  the GLB parser does it for every metallic-roughness material, as upstream's
  `createMaterial` does. The specular workflow runs through `VT_FEATURE_SPEC_GLOSS`
  with `metallicFactor` packed 0 and F0 = the specular colour, authored sRGB and
  uploaded linear. The old `setSpecularColor` / `setGlossiness` / `setUseSpecGloss`
  duplicates are gone; a KHR spec-gloss asset uses `setSpecular` (gamma-encoded) and
  `setGloss`. There is no `(1 - max(specular))` diffuse scale on either backend.
- **Material flags bits 18 and 19 are `useSkybox` OFF and `hasOpacityMap`.** Both
  came free when the sheen and iridescence map bits, which no shader ever read, were
  removed with their setters; bit 20 is the only free bit. Bit 18 is stored inverted
  so a zero flags word keeps the scene environment, and it drops only the env atlas
  (SH probes and the flat ambient remain), as upstream's `useSceneEnv` does. The
  opacity map is METAL ONLY (slot 34, multiplied into the forward and shadow alpha
  with the base-colour UV); Vulkan logs one warning per process. It multiplies ON TOP
  of the base-colour map's alpha, so a material that sets ONE texture as both — the
  text element material did until the opacity map was wired — gets alpha squared on
  Metal only, which thins every anti-aliased edge while Vulkan stays unchanged. Set
  the opacity map only when it is a different texture. The spec-gloss map and
  the clearcoat maps are Metal only too. DEVIATIONS kept on purpose, marked at the
  code: `refractionIndex` and `iridescenceIOR` are IORs where upstream stores eta, and
  sheen is colour + roughness where upstream has `sheenGloss` + `useSheen`.
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
- **An example asset standing in for upstream's must match its PIXELS, not just
  its subject.** `textures/checkboard.png` was a generated 16px checker of
  200/255 while upstream's is a 1024px 4x4 checker of 45/51 grey — thirty times
  the linear albedo. `render-to-texture` multiplies it by diffuse (3,4,2), so its
  ground rendered WHITE instead of green on both backends and read as a lighting
  bug; `world-to-screen`'s checker was light where upstream's is dark. Before
  chasing a brightness gap that is identical on Metal and Vulkan, compare the
  example's textures against upstream's (`md5`, then mean pixel value) — an
  engine bug is rarely backend-identical. The asset is regenerated to upstream's
  exact pixels. It happened AGAIN on 2026-09-16, and not through brightness:
  `seaside-rocks01-gloss.jpg` was a 2048px ambientCG encode averaging 130/255
  (0.51, max 0.73) where upstream's is 1024px averaging 191/255 (0.75, max 1.0).
  Gloss picks the environment-atlas MIP — `level = (1 - gloss) * 5` — so 0.51 read
  a heavily prefiltered level and `refraction`'s capsules came out a flat opaque
  wash, while upstream's 0.75 keeps them glassy. Proved by setting a CONSTANT gloss
  on both sides: at 0.51 upstream goes flat too, at 0.9 ours turns to glass. A
  substituted texture therefore breaks parity through any channel the shader
  READS, not just albedo; the whole seaside-rocks01 set is now upstream's bytes.
- **Do not draw to the back buffer after `Engine::render()`** — `frameEnd`
  presents the drawable and a stale `_frameDrawable` reuse is a pointer-auth
  SIGSEGV. Use `Renderer::addAppendPass` to append app passes to the frame graph.
- **`Compute` parameter binding is by NAME in sorted order, and the INDICES DIFFER
  PER BACKEND.** This port has no shader reflection, so the order is derived from
  the names — buffers, then textures, then the single uniform block, each group
  name-sorted, the block's members being the scalars again in name order. The
  order is the same on both backends; the numbering is not, because Metal gives
  each resource kind its own namespace and Vulkan has one flat descriptor set.
  Metal: `buffer(0..b-1)`, the uniform block at `buffer(b)`, textures at
  `texture(0..t-1)`. Vulkan set 0: bindings `0..b-1`, textures `b..b+t-1`, the
  block at `b+t`. So a kernel with BOTH buffers and textures needs different
  indices in its MSL and its GLSL; nothing in the tree has both yet, and the one
  shipped kernel (the particle simulation) is the case where the two agree, so the
  table in `compute.h` is the contract rather than any existing kernel. A shader
  that declares them in a different order silently reads the wrong data. Metal also
  binds NO sampler state to a compute kernel: an MSL kernel that filters declares
  its own `constexpr sampler`.
- **Only ONE SIMD backend compiles per target, so a defect in another one is
  invisible here.** Apple silicon selects the Apple backend; the SSE path is now
  gated on `__SSE4_1__` (it uses `_mm_dp_ps` / `_mm_insert_ps`, so `__SSE__`
  alone could not compile) and x86 without SSE4.1 falls through to scalar. Two
  wrong SSE horizontal sums lived in `Vector2::dot` and
  `Vector4::planeNormalize` for as long as the files existed. When you touch one
  backend, check the same function in the other three, and add the contract to
  `tests/simdMathTests.cpp` — which only covers the backend the build selected,
  so an x86 CI build is what would actually guard the SSE path.
- **GCC and clang both fuse `a * b + c` into FMA by default**, and GCC does it even
  in strict C++ mode once `-mfma` is on — which Jolt's exported flags turn on for
  every x86 engine target. Verified with objdump on Apple clang (arm64) and GCC 14
  (x86). A fused scalar expression rounds differently from the same arithmetic in
  SIMD intrinsics, so a scalar path can silently stop matching its SIMD twin. The
  engine's FMA-fused scalar code is not a bug; comparing it bit for bit against
  anything is.
- **A SIMD kernel ships with its scalar reference and a bit-exact test.** Today:
  `scene/gsplat/gsplatSortKeys` (splat depth and sort key) and
  `framework/lightmapper/lightmapperBvh` (4-box slab test). Each exposes the scalar
  form publicly, its test compares the two exactly, the kernel file is built with
  `-ffp-contract=off` where a multiply-add could fuse (`engine/CMakeLists.txt`), and
  `VISUTWIN_KERNELS_FORCE_SCALAR` builds the scalar path for measurement. These
  kernels gate on `__SSE2__` and `__ARM_NEON && __aarch64__` DIRECTLY, not on
  `USE_SIMD_*`, so on Apple silicon the maths classes use the Apple backend while
  the kernels use NEON; `VISUTWIN_EXPECT_KERNEL_BACKEND` makes their tests fail on a
  silent fall-through, as `VISUTWIN_EXPECT_SIMD_BACKEND` does for the maths. Two
  traps the slab test had to avoid: a zero direction component gives
  `0 * inf = NaN`, which `std::min` / `std::max` IGNORE and NEON's `vminq` /
  `vmaxq` PROPAGATE, so the SIMD form selects on comparisons instead; and splat
  depths must be clamped to the bin range before any integer conversion, because
  the unclamped negative-to-`uint32_t` cast is undefined — x86 wrapped a splat
  nearer than the nearest bound corner to the FARTHEST key.
- **The CPU lightmapper's BVH skipped most of every tree until 2026-09-13.** It
  stored only a node's left child and walked `left` and `left + 1`, but children
  are built depth-first, so `left + 1` is the right sibling only when the left child
  is a leaf. On a 24,800-triangle scene it answered every ray that should have hit
  as a miss: no shadows and no AO. `LightmapperBvh` stores both children, and
  `tests/lightmapperBvhTests.cpp` checks any-hit against brute force over every
  triangle — the only oracle that cannot share a tree bug. Nothing visual caught
  it because `lightmap-bake` starts with the GPU bake; the CPU bake runs only when
  C is pressed, so a default screenshot proves nothing about the CPU path.
- **A glTF attribute is not always float, and refusing a quantised one drops the
  whole primitive in silence.** `TEXCOORD_n` and `COLOR_n` may be normalized
  byte/short in CORE glTF, and `KHR_mesh_quantization` extends that to `POSITION`,
  `NORMAL` and `TANGENT`. One `decodeComponent` in `glbParser.cpp` does the spec's
  de-quantisation for every reader, sparse overrides included, so a new reader
  should go through `readElement` rather than casting to `const float*`. The gate
  that actually rejected such a file was not the readers: FOUR per-primitive guards
  tested `componentType != FLOAT` and `continue`d, so the mesh simply was not there
  and nothing was logged. Verify a change here by rendering the quantised asset
  against the same geometry written as floats — they must agree to rounding.
- **`KHR_texture_transform` cannot be copied from upstream, because this parser
  flips V into the vertex and upstream does not.** The composed transform is
  derived in the comment above `applyTextureTransforms`; what matters outside it is
  that a sign error in the V term is INVISIBLE under a pure scale (the two
  spellings differ by a whole number of tiles, which REPEAT wrapping hides) and
  that `toy_car.glb`, the only shipped asset with the extension, puts it on a
  black fabric where it cannot be seen either. Test it by rendering a transform
  against the same transform baked into the mesh UVs, with a rotation AND an
  offset, not just a scale.
- **A texture's row 0 is the TOP of the image, and v = 0 samples it — for loaded
  images AND render targets, on both backends.** That is upstream's
  RENDERTARGET_ORIGIN_TOP, and it is the only origin here, so there is no `flipY`
  on `RenderTargetOptions` (it was stored and never read). Every built-in primitive
  writes upstream's `(u, 1 - v)`, putting v = 0 at the TOP: +Y on the box sides,
  sphere and cone bodies, -Z on the plane. The plane wrote plain `v` until
  2026-09-15, so every image on a plane was upside down against upstream — seen
  only as render-to-texture's tv showing its sky at the bottom, because a lying
  plane with a checkerboard or a rock texture does not look wrong either way.
  `tests/primitiveGeometryTests.cpp` pins the orientation; test anything new here
  with an ASYMMETRIC image, never a checker.
- **Primitive tangents are DERIVED from the UVs, never written by hand, and the
  bitangent `cross(n, t) * w` points toward DECREASING v** — the image's top row,
  where a normal map's green channel points. `calculateTangents`
  (`scene/geometry/geometryUtils.h`) is upstream's Lengyel accumulation with that
  handedness, which is a DEVIATION from upstream's own `calculateTangents` (it
  points toward +v) but matches what upstream actually shades primitives with: its
  primitive cache builds them WITHOUT tangents, and its derivative TBN negates the
  dP/dv axis. Until 2026-09-15 the box wrote (1, 0, 0) on every face (parallel to
  the normal on +/-X, which rendered those faces black), and the sphere and capsule
  had tangent AND bitangent reversed — a 180-degree turn of the normal map that
  reads as light from the wrong side, not as an error. Change a primitive's UVs and
  the frame follows; the test checks every corner against its triangle's UV
  gradient. `DEBUGPASS_WORLDNORMAL` on a normal-mapped box beside a plane wall shows
  a wrong frame in one frame: matching faces must match in colour. The frame is not
  only a normal-map concern: Metal's anisotropic IBL bends the reflection toward the
  bitangent, so its SIGN picks sky or ground, and the `anisotropy` spheres changed
  on Metal while Vulkan (a roughness-only approximation) stayed bit-identical.
- **A glTF material property must be written to the STANDARDMATERIAL slot, not the
  base Material one.** `StandardMaterial::updateUniforms` pushes its own per-map
  tiling/offset/rotation into `Material`'s `TextureTransform` fields on every pack,
  so `setBaseColorTransform` from the parser was overwritten before it ever reached
  the GPU — the same trap as `setDiffuse` versus `setBaseColorFactor`, one field
  further out. The parser writes `setDiffuseMapTiling` and its four siblings.
- **`extensionsRequired` is consulted, and the list of what the parser supports
  lives in `warnUnsupportedRequiredExtensions`.** Add an extension there when you
  implement it, or a file that needs it keeps warning; leave it out when you only
  half-implement one, or the warning that would have named the cause goes quiet.
  DEVIATION from upstream, which does not read the field at all.
- **A device capability is ASKED FOR, and the answer has a home on
  `GraphicsDevice`.** `maxTextureSize` / `maxCubeMapSize`, `maxAnisotropy`,
  `textureHalfFloatRenderable` / `textureFloatRenderable`, `maxSamples`,
  `maxFramesInFlight`, `supportsCompressedFormat`, `supportsDualSourceBlending`,
  `supportsCompute`, `supportsGpuInstanceCulling` and `supportsTimestampQuery` are
  the whole list; a new one goes there rather than into a backend header, or only
  one backend can be asked. Two failure shapes this closes, both silent: a LITERAL
  standing in for a limit — five call sites clamped to 4096, which is a quarter of
  what either backend actually allows — and the SAME limit spelled differently per
  backend, which is what a hard-coded 16x anisotropy on Metal and a queried one on
  Vulkan were. The float-renderable pair defaults to FALSE and the dimensions to
  the 4096 they replaced, so a backend that answers nothing degrades instead of
  allocating a target the driver refuses. `supportsTimestampQuery` is derived from
  `gpuProfiler()` rather than stored, since both backends build the profiler only
  after finding timestamp support and a second flag could only disagree with it.
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
- **The forward sort key packs its fields; it must never XOR them.** The layout is
  in `scene/renderer/sortKey.h` — draw bucket, alpha test, material ID, mesh — each
  owning its own bits, and `tests/sortKeyTests.cpp` holds it. The key it replaced
  XORed overlapping ranges (the depth-state key and the emissive-texture bit both at
  bit 4, the alpha mode and the occlusion bit both at bit 3), so two materials
  differing in one of those hashed equal and interleaved; and its one caller shifted
  the result left by 32 and discarded the half holding the shader variant key, so the
  most expensive state change in a frame contributed nothing to the order. Material
  IDENTITY is what is sorted on, as upstream, because consecutive draws of one
  material skip binding entirely — state similarity cannot deliver that.
- **A layer carries a sort mode per sublayer** (`Layer::opaqueSortMode` /
  `transparentSortMode`, upstream's SORTMODE_*), defaulting to MATERIALMESH and
  BACK2FRONT, which is what the renderer always did. The two pull in opposite
  directions on purpose: opaque wants the fewest state changes, transparent has to
  composite back to front. SORTMODE_CUSTOM with a null callback leaves the order
  ALONE rather than falling back to a mode nobody asked for.
- **Reordering opaque draws moves pixels, and that is the scene, not a bug.** Where
  two surfaces are coplanar, `LESS_EQUAL` lets whichever draws last win, so any
  change to the order flips those pixels. `depth-of-field` has about 0.5% of them:
  reversing the opaque comparator on ONE build moves 8,863 pixels at a max of 64
  counts, the same amplitude a sort-key change produces. Measure that reversal before
  calling such a difference a regression — neither order is correct for depth-equal
  geometry.
- **Transparent draws sort on SIGNED view-axis depth, not radial distance**
  (`scene/renderer/sortDistance.h`, upstream's `_calculateSortDistances`).
  Radial distance ranks an off-axis surface farther than a centred one at the
  same depth by up to 1/cos(fov/2), and cannot tell behind from in front.
  `MeshInstance::setCalculateSortDistance` overrides it per instance.
- **A loop that gathers components for a frame must test `Component::active()`,
  not `enabled()`.** `enabled()` is the component's OWN flag and says nothing
  about an entity — or a parent entity — that was switched off; `active()` is both
  halves, and it is the same condition `onEnable` / `onDisable` fire on. Every one
  of the ten light-gathering loops tested `enabled()` alone until 2026-09-11, so a
  light on a disabled entity went on lighting the scene and casting its shadow
  while the mesh instances on that same entity correctly vanished (the render loop
  had been patched for this hole by hand; lights had not). `LightComponent` also
  syncs `active()` into its backing `Light`, because `shadowRenderer`,
  `shadowRendererLocal` and the cookie pass gate on `Light::enabled()` rather than
  on the component. Two sweeps deliberately take EVERY instance and say so in a
  comment: the lightmapper's layer backup, which must restore a light it widened
  even if that light is switched off mid-bake, and the camera's render-data purge,
  where a disabled light is exactly the one holding a stale pointer.

  SCRIPTS had the same hole and the same fix: every phase — initialize,
  postInitialize, fixedUpdate, update, postUpdate — gates on `active()`, and
  `Script::enabled()` folds in its component's active state, so a script on a
  disabled entity stops running rather than merely stopping being drawn.
  `ScriptComponent::onEnable` is what initializes a script created while the
  component was inactive; that logic used to live in its `setEnabled` override,
  which saw only the component's own flag, so a script created on an entity that
  was enabled LATER never initialized at all.
- **Component lifecycle runs in `Component::order()`, not container order.**
  Lowest first on enable, reverse on disable, creation order as the tiebreak;
  `RigidBodyComponent` returns -1 so its body exists before anything can move or
  query it. `onPostStateChange()` then runs over every component, which is where
  one wires itself to a sibling that had to exist first. This used to iterate an
  `unordered_map`, so the order varied per run.
- **Component systems emit `add` / `beforeremove` / `remove`, and
  `removeComponent(Entity*)` is the way to take a component away.** A system that
  needs its own bookkeeping on destruction must not declare an overload named
  `removeComponent` — that hides the virtual (`ScriptComponentSystem` calls its
  one `unregisterComponent` for exactly this reason).
- **`ComponentSystemRegistry::add` REJECTS a duplicate id, or a second system for
  the same component type, and keeps the first.** It used to overwrite the lookup
  maps while leaving the original alive, owned and still subscribed behind an id
  that no longer resolved to it. `remove` erases from the owning vector and both
  maps together — partial erasure is the bug this pairing exists to prevent.
- **`Entity::destroy()` is the teardown path, and it does NOT free the node.**
  Descendants first, disable in order, `destroy` event, then each component
  released THROUGH the system that owns it (so `beforeremove` / `remove` fire for
  a destroyed entity exactly as for an explicit removal), in reverse creation
  order. It is idempotent and the destructor calls it. Ownership stays with the
  parent's `unique_ptr`: freeing inside `destroy()` would leave `this` dangling
  for the rest of the call. Note the `_destroying` guard in
  `removeComponentInstance` — teardown has already disabled everything in order,
  and without it each component would get a second `onDisable`.
- **`Engine::start()` must be called AFTER the scene exists.** It fires the
  initialize phase (`start`, then systems `initialize` / `postInitialize`, then
  the app's `initialize` / `postinitialize`) and then ticks. `ExampleApp` starts
  the engine in `run()` once `create()` has returned, for exactly this reason —
  starting it in `initEngine()` initialized an empty world and rendered an empty
  first frame. Every script initializes before any script post-initializes.
- **The per-draw uniform rings GROW, and the growth is why an overflow is only
  ever one bad frame.** Both backends size a frame region for a draw count, count
  what the frame actually ASKED for (including what did not fit), and reallocate at
  the next frame boundary. Growth cannot happen mid-frame: every offset already
  handed out is interpreted against the one buffer bound at the start of the render
  pass (Metal) or named by the persistent dynamic-UBO descriptor sets (Vulkan). So
  it happens behind a full drain — Metal waits out the other two in-flight regions
  on its own semaphore, Vulkan calls `vkDeviceWaitIdle` and then REWRITES the two
  descriptor sets through `writeUniformRingDescriptors`, which is also what
  initialization calls so the two cannot describe the buffer differently.
  The overflowing frame itself still degrades, and the two backends degrade
  differently because their allocators differ: Metal's fixed-slot ring hands the
  excess draws the last slot's uniforms, while Vulkan's variable-size bump
  allocator cannot alias safely (the previous allocation may be a different struct
  entirely) and skips them. Both say so in one message per frame. Note that the
  demand measured DURING an overflowing frame is an underestimate on Vulkan,
  because a skipped draw never asks for what it would have needed — which is why
  growth is `max(requested, current * 2)` and why a first overflow can take two
  frames to settle.
- **A CPU-written, GPU-read buffer needs `GraphicsDevice::maxFramesInFlight()`
  copies on Metal, and the write must happen at most once a frame.** A Metal
  `setData` is a memcpy into shared storage the GPU reads directly, and the ring
  semaphores let the CPU run three frames ahead, so a two-buffer ping-pong comes
  back round onto a buffer the frame before last has not finished with. Vulkan
  cannot tear the same way — `VulkanVertexBuffer::unlock` stages the bytes and
  the queue orders the copy behind the reads already submitted — so this is a
  Metal-only rule that a Vulkan run will never reproduce, and it costs one extra
  copy of the buffer. The splat ORDER buffer is the one such buffer today
  (`GSplatInstance`); upstream has no rule here because every upload it makes is
  queue-ordered. The once-a-frame half is what makes the cycle long enough — it
  is keyed on `GraphicsDevice::renderVersion()`, because the same splat drawn by
  several cameras in one frame would otherwise walk several slots and lap the
  frames still in flight. The symptom is torn splat ordering for one frame under
  continuous camera movement, which is exactly when nobody is looking closely.
- **Mesh instances are culled ONCE per (camera, layer) per frame, into a cache both
  sublayers read.** `ForwardRenderer::buildFrameGraph` registers the pairs it will
  render (`Renderer::requestMeshInstanceCull`) and culls them in one batch
  (`executeMeshInstanceCull`), which is where the `precull` and `postcull` events
  fire — once per camera, upstream's contract, which a lazy per-layer cull could not
  give. `renderForwardLayer` then reads its own bucket. It used to sweep every
  `RenderComponent` in the scene and run the frustum test itself, for the OPAQUE
  sublayer and then again for the TRANSPARENT one, each discarding the half that
  belonged to the other.

  **The cache is keyed on the FRUSTUM, not just the pair.** Culling happens while the
  graph is built and the sets are read while it renders, and on the first frame a
  camera's aspect ratio can still change between the two because its render target is
  not sized yet — so the cached set answers for a differently shaped view. The entry
  stores the frustum it used and re-culls when the camera's differs; a miss also
  covers a camera the composition never registered, such as an app-appended pass,
  which must render its layer rather than nothing. Do not "optimise" that comparison
  away, and do not give it a tolerance: both frusta come from the same code, so they
  are bit-identical unless the camera really changed.

  `Camera::cullingMask` is ANDed with `MeshInstance::mask()` here. Verify a change to
  any of this by running the OLD sweep inline alongside the new one and comparing the
  two sets in one process — the animated examples cannot be screenshot-diffed, and
  that comparison found the aspect-ratio hole above, which no screenshot did.
- **Lights are CULLED per frame, and `Light::visibleThisFrame` answers a different
  question from the per-camera light list.** `visibleThisFrame` is a UNION over every
  camera — "does this light's shadow map and cookie need rendering at all" — cleared
  once by `Renderer::resetLightVisibility` and set by `Renderer::cullLights` per
  camera. The per-camera local-light list in `renderForwardLayer` does its OWN
  frustum test, because a light only a reflection camera can see must not light the
  main view. Do not substitute one for the other.

  Three things have to stay true. The cull runs at the TOP of
  `ForwardRenderer::buildFrameGraph`, before any shadow or cookie pass is built from
  its result — the obvious home, the per-camera loop further down that already culls
  shadow maps, is AFTER the local shadow passes are built, and a frame-late cull is
  worse than none. A directional light is never culled, having no bounds to test.
  And `ShadowRenderer::needsShadowRendering` is PURE: it used to consume a
  `SHADOWUPDATE_THISFRAME` request, which was survivable only while every light was
  visible, and with culling live it answered "no" and consumed the request in the
  same breath, losing the shadow the caller asked for. `Renderer::consumeOneShotShadows`
  does that after the frame graph is built.

  The eight-slot main light array is ranked by `Camera::screenSize`, not by component
  order, so the slots go to the lights covering most of the picture. `tests/lightCullingTests.cpp`
  pins the bounds geometry and the cull decision, because NO example in the tree has a
  light off screen — every one of them keeps its lights in view, so a culled light is
  never exercised by a render at all.
- **A splat cloud's bounds carry each splat's EXTENT, taken from the covariance
  DIAGONAL.** A splat is an ellipsoid, so the cloud reaches past the hull of its
  centres by the size of whatever sits on its rim; bounding the centres alone culls
  the whole thing while part of it is still on screen, and a one-splat cloud gets a
  zero-size box. `Sigma = R S^2 R^T` is what `GSplatData` stores (rotation and scale
  are discarded at load), and `Sigma_dd` IS the variance along model axis d — so
  `2 * sqrt(Sigma_dd)` is the exact 2-sigma bound per axis, upstream's convention by
  a tighter route than either bound upstream computes. Use the diagonal, not the
  scales, and never pad axis d by one scale component: a rotated splat's extent on x
  comes from whichever scale the rotation points along x, which is the half that
  looks right in every render and is wrong in the numbers.
  `tests/gsplatAabbTests.cpp` pins it by writing PLYs whose answer is closed-form.
  The default gsplat example pose cannot see a bounds change at all — the cloud is
  wholly in view, so culling never fires and the frame must come back bit-identical.
- **A splat's clip z is CLAMPED to the depth range, and that only works because
  its screen-space kernel is clamped too.** A gaussian splat is a quad built around
  ONE projected centre, so the whole quad carries that centre's depth: an unclamped
  centre crossing the near plane clips the entire splat away while its footprint
  still covers visible pixels, and the surface nearest the camera pops out whole as
  you walk into a cloud. Upstream's `gsplatCenter.js` clamps, and both backends now
  do (to `[0, clip.w]`, after the GL-to-[0,1] remap). The catch is that the same
  near-plane splats are the ones whose perspective Jacobian — it divides by view.z —
  blows their footprint up without bound, and the z clip was silently hiding that:
  clamp z without `gsplatCorner.js`'s `vmin = min(1024, viewport)` kernel clamp and
  the frustum x/y cull, and ONE splat covers the screen. Vulkan had neither and went
  entirely flat; it has all three now. Do not port one of these without the others.
  The default gsplat example pose cannot see any of it — nothing there straddles a
  plane, and the frame must come back bit-identical, which is the control that says
  a change here is confined to the splats that actually cross.
- **Leftover instance bindings follow the next draw.** The backends pick the
  instancing vertex layout by scanning bound slots, so shadow passes must unbind
  slot 5 after an instanced caster.
- **metal-cpp framework extern constants** (e.g. `MTL::CommonCounterSetTimestamp`)
  only link in the `*_PRIVATE_IMPLEMENTATION` TU. Compare string values instead
  inside the engine library.
- **`Matrix4::getElement` takes (col, row)**, not (row, col).
- **A hand-built sphere's triangle winding has to be counter-clockwise seen from
  OUTSIDE**, or its normals face inward. A mirror ball HIDES this — it still
  reflects something — so the inverted winding in a reflection-probe example went
  unnoticed until the same generator was reused with a diffuse material in
  mesh-morph and came out black. `DEBUGPASS_WORLDNORMAL` says it in one frame: a
  correct sphere is blue in the middle, an inverted one is not.
- **A normal is carried by the INVERSE TRANSPOSE of the model matrix, and both
  backends now compute it.** Under non-uniform scale the bare 3x3 and the inverse
  transpose differ, and lighting reads the difference directly: a flattened sphere
  shades as if it were still round. Metal uploads the matrix per draw
  (`metalUniformBinder.cpp` builds it from 3x3 cofactors, cheaper than a 4x4
  inverse, then divided by the SIGNED determinant); Vulkan computes the cofactor
  matrix per vertex in `shaders/vulkan/normal_matrix.glsl`, which every vertex
  module includes, and applies the determinant's sign explicitly. The two are
  equal by construction — a cofactor matrix is the inverse transpose times the
  determinant, and the shader normalizes, so the magnitude cancels and the sign is
  all that has to be put back. Do not "simplify" either side to `mat3(model)`:
  that is the defect this replaced, and it is invisible in any scene whose scales
  are uniform or whose surfaces are axis-aligned (a scaled plane or box keeps its
  normals either way — only curved or rotated surfaces show it). The INSTANCED and
  DYNAMIC BATCH paths deliberately keep the bare 3x3 on both backends, as upstream
  does: their model matrix arrives per instance and upstream does not pay for an
  inverse there. A skinned draw composes the two — the node's inverse transpose
  applied after the skin matrix's bare 3x3.
- **A mirrored mesh flips BOTH its winding and its normals, and the two halves
  only make sense together.** `Renderer::applyNodeScaleFlip` swaps which face is
  culled when `GraphNode::worldScaleSign()` is negative, and the normal matrix
  carries the determinant's sign so the surviving faces get outward normals. That
  is what the glTF specification asks of a renderer — reverse the winding when the
  node's global transform has a negative determinant, and transform normals by the
  inverse transpose — and it is upstream's behaviour. Flipping one half alone
  lights a mirrored mesh INSIDE OUT, which is what both backends did until
  2026-09-11: Metal cancelled the sign with a `normalSign` uniform (now deleted)
  and Vulkan had no sign to cancel. Assets really do carry such nodes — one node
  of `leonardo_da_vinci.glb` is mirrored, and it is the only place a shipped
  example shows this at all.
- **A batch is one vertex layout, one primitive type and ONE pair of shadow
  flags.** `BatchManager` merges by reinterpreting a source vertex buffer as the
  parsers' 56-byte packed vertex, so a mesh instance that is not exactly that
  layout may never enter a batch: a skinned mesh (88 bytes) or a point cloud (28)
  tagged into a batch group used to merge as garbage geometry, read past the end
  of its own storage on the way, and say nothing.
  A component with ANY skinned or morphed mesh instance contributes NONE of them
  (`entityIsBatchable`, upstream's whole-entity rule): merging bakes each source's
  world transform into a shared buffer, so a deforming mesh loses the thing that
  deforms it. A skinned mesh is caught by its 88-byte stride anyway; a MORPHED one
  carries the ordinary packed layout with its deltas in a separate buffer, so nothing
  about its format says no — it would batch cleanly and then sit still.
  `framework/batching/batchSplit.h` is where the rules live —
  `splitBatchLists` divides a (group, material) bucket into one list per batch on
  the format's `batchingHash`, the primitive type, castShadow/receiveShadow and
  `BatchGroup::maxAabbSize`, and `formatIsPackedVertexLayout` is the predicate the
  merge paths owe their cast. Note the two hashes are not interchangeable:
  `batchingHash` ignores offsets and stride, which is what makes it the right key
  for GROUPING and the wrong one for a `reinterpret_cast`; the rendering hash pins
  the byte layout. A rejected list is not a failure — the originals stay visible
  and unbatched, which costs draw calls and nothing else. The split happens once,
  at `prepare()`, from the transforms in place THEN, so a dynamic batch whose
  instances wander apart later keeps the grouping it was built with.

  `prepare()` is `generate()` over every registered group; `generate(scene, ids)`
  rebuilds only those, and `markGroupDirty` queues one for `updateAll()` to pick up
  next frame. Regeneration does nothing until `prepare()` has run once — it needs a
  scene to register the batch mesh instances with the group's layers, and during
  setup the app has not tagged anything yet — which is why `addGroup` can mark dirty
  unconditionally without building a batch too early. NOTE a batch's layers come from
  the GROUP, not from its sources; upstream marks its own per-instance layer split
  "legacy" for that reason, so there is nothing to split on there.
- **A camera frame OWNS the scene-colour grab, and three things have to line up
  for it.** `CameraComponent::requestSceneColorMap` is the request on both paths;
  `RenderPassCameraFrame::applyCameraSettings` copies it into
  `CameraFrameOptions::sceneColorMap` (upstream reads the same flag off
  `CameraFrame.rendering`), and `ForwardRenderer` must NOT end a block of render
  actions at the depth layer for a camera that has post-processing — a camera frame
  is built from one such block, and splitting it hands the frame only the actions
  after the grab while the opaque world and the sky go to the back buffer for
  compose to overwrite. That is a black frame, not a missing reflection. Outside a
  camera frame the standalone grab pass copies the back buffer as before.
- **The grabbed scene colour is LINEAR HDR under a camera frame and GAMMA-encoded
  otherwise**, so every consumer gates its decode on bit 5 of
  `LightingData::flagsAndPad[0]`, the same bit the sky and the tail check. There are
  four such consumers — refraction and SSR, in each language — and a decode applied
  unconditionally darkens whatever samples it by roughly a stop.
- **Dynamic refraction was dark and opaque on BOTH backends for three stacked
  reasons, found 2026-09-15 on `post-processing`'s amber (measured amber region
  [47,53,41] -> [139,105,48]).** (1) The
  fragment-stage `lighting.viewProjection` was uploaded TRANSPOSED — both binders
  passed `getElement(row, col)` — so every refracting fragment projected to a
  negative w and its grab UV clamped into a corner: one flat colour over the whole
  surface, which reads as "opaque", not as "wrong offset". SSR reads the same matrix.
  (2) The camera frame's scene pass stops at `lastGrabLayerId` (the skybox), and
  `findActionIndex` searched only render actions, which exist for ENABLED layers;
  a disabled Skybox layer (usual for an env-atlas-only scene) matched nothing, the
  pass took every action, and the grab ran AFTER the transparent layers, so the
  surface refracted itself from last frame. It now places the stop by composition
  position, as upstream's `addCameraLayers` does. (3) The refraction was tinted by
  `baseColor^(thickness + 1)`; upstream applies the diffuse albedo ONCE (the
  refraction mixes into `dDiffuseLight`, which `combineColor` multiplies by albedo).
  Bug (1) hid bug (2) completely: fixing the order alone moved nothing. Probe it
  the way that found it — output the grab at a FIXED uv (valid texture?), the
  flags `uv in range` / `w > 0` (valid projection?), and the raw sample + 0.05 (a
  feedback loop runs away to white within 120 frames).

  CLOSED: re-measured 2026-09-16 against upstream pinned to the same pose, the
  amber matches (ours a touch brighter). Do NOT reopen it from a thumbnail — the
  old "dimmer than upstream" claim came from an unpinned capture against a
  differently-posed thumbnail, twice. Still open: upstream scales the refraction
  offset by the model's world scale (x60 here) and the fragment stage has no model
  matrix, so a hard-coded x60 is not adopted blindly; and the amber projects LARGER
  here than upstream at identical camera parameters. Numbers in `ENGINEERING-LOG.md`.
- **Vulkan's clip space is NOT Y-down for this engine.** The backend rasterises
  through a negated-height viewport so Metal projection matrices work unchanged,
  which puts NDC +Y at the TOP row of every target, back buffer and offscreen
  alike. A point projected in a shader therefore maps to a texture coordinate
  exactly as it does on Metal, `* vec2(0.5, -0.5) + 0.5`. Two GLSL call sites
  believed otherwise and sampled the grab upside down.
- **A grab pass OWNS the texture it publishes, and the device only borrows a raw
  pointer to it.** So a grab pass is persisted across frames rather than rebuilt
  (the next frame's scene pass binds the pointer before the grab re-runs), and its
  destructor clears `setSceneColorMap` / `setSceneDepthGrabMap` when the device
  still points at it — `requestSceneColorMap(false)` and any camera-frame option
  change that rebuilds render targets both destroy one.
- **Every caster sweep goes through `collectShadowCasters`, and it takes the
  CAMERA.** Batch mesh instances belong to no `RenderComponent` — `BatchManager`
  registers them straight with the scene layers — so a hand-written sweep of
  `RenderComponent::instances()` misses them. The directional FIT and the directional
  PASS each had their own sweep and disagreed about exactly that: the fit sized the
  shadow map's depth range to the unbatched scene while the pass drew batches into
  it, so a batch outside that range was clipped out of the map and its shadow was
  simply absent. Measured on `dynamic-batching`: the fitted depth span was 54 where
  it should have been 104, with the near plane 55 units past the batches. The camera
  argument filters components by layer, and a caller that fits or draws for one camera
  must pass it. Two sweeps of the same thing will drift again; use the collector.
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
that turns it on, its deviations from upstream, and — under its own "Live gotchas"
— the traps that bite while working ON that subsystem: MSAA and render targets,
the shadow pass's opacity frontend, shadow-map invalidation and the bias
convention, the SSAO/prepass pairing, camera priority, PCSS, lightmaps and probes,
wide lines, parallax, opacity dither, the Metal-only scalar maps, normal-map
scaling and the Nishita atmosphere. Read that file before touching any of them.

What stays HERE is only what bites during UNRELATED work.

## Open items

- **A DEBUG Vulkan build crashes before its first frame, inside the validation
  layer.** Found 2026-09-16. An example built from the `vulkan` preset defines
  `VISUTWIN_DEBUG_GPU_VALIDATION` (Debug config), which makes
  `GraphicsDeviceOptions::enableValidation` default true, and the process dies with
  SIGSEGV — EXC_BAD_ACCESS at 0x200 — in `libVkLayer_khronos_validation` during
  `SDL_Vulkan_CreateSurface`: `vvl::GetDispatchInstance` ->
  `vulkan_layer_chassis::CreateMetalSurfaceEXT` -> `vkCreateMetalSurfaceEXT` ->
  `Cocoa_Vulkan_CreateSurfaceViaMetalView` -> `VulkanGraphicsDevice::initInstance`.
  The last line logged is "Vulkan validation enabled", so it READS like a hang or a
  silent startup failure and is neither — check the exit code (139) and
  `~/Library/Logs/DiagnosticReports`, not the log. A RELEASE Vulkan build never
  loads the layer and runs normally, which is both the workaround and the proof.
  It is NOT engine code: it reproduces with the examples' HUD disabled entirely,
  and `vulkan-validation-smoke` passes only because it creates its window
  `SDL_WINDOW_HIDDEN`. Suspect the loader/layer pairing — this machine has four
  `libvulkan` dylibs in `/usr/local/lib` (system 1.4.357) while the vcpkg build
  resolves its own 1.4.341.
- **`DeviceVRAM::ub` and `sb` are still zero, and the texture figure is a LOWER
  BOUND.** `tex`, `vb` and `ib` are live as of 2026-09-16 — before that the whole
  texture side was dead code (`Texture::_gpuSize` was declared and never assigned,
  so `adjustVramSizeTracking` was never reached and the HUD deliberately printed no
  texture figure at all). What is still missing: the backends' uniform and storage
  pools are not wired to it, so `ub` and `sb` cannot be shown; and the texture
  figure is CONTENT size from `TextureUtils::calcGpuSize`, fixed at construction, so
  it counts no driver padding and under-counts any texture whose mips are generated
  on the GPU afterwards (`setMipmaps` does not move `_numLevels`). That is a stable
  under-count, not a drift — the same figure is added and subtracted — but nothing
  may present this as an exact allocation total. The HUD's compact view labels
  the sum `VRAM`, as upstream labels its `vram.totalUsed`, and it is that same
  lower bound; the detailed view spells the parts out.

  The `texShadow` / `texAsset` / `texLightmap` SPLIT is live as of 2026-09-16 too,
  and the three sub-buckets DELIBERATELY DO NOT SUM to `tex`. A texture joins one
  only where its creation site sets `TextureOptions::profilerHint`: the parsers
  (glb/obj/assimp), the four texture-asset paths in `asset.cpp` and the font atlas
  are ASSET; `ShadowMap::create` (depth map and VSM blur temp) and the clustered
  shadow atlas are SHADOWMAP; both lightmap bakes are LIGHTMAP. Everything else —
  render targets, the post-processing chain, env atlases, area-light LUTs,
  reflection probes, scene grab — stays `TEXHINT_NONE` on purpose, because it is
  none of those things. So the HUD shows the remainder as "other" rather than
  letting a subtraction that does not balance read as a bug. **Add the hint when you
  add a texture creation site**, or its bytes land only in the undifferentiated total.
- **Vulkan reflections are slightly SOFTER than Metal's.** Re-measured 2026-09-05
  with the scene frozen, on the since-removed `reflection-probe` scene (re-measure
  on `reflection-probe-dynamic` before chasing): it matched to 0.3% in the mean, but the
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
- **Under a camera frame nothing publishes the sampleable depth COPY.** The frame
  publishes scene DEPTH from its own attachment, and it owns the colour grab, but
  `sceneDepthGrabMap` — the post-opaque depth copy that only SSR reads — is
  produced solely by the standalone `RenderPassDepthGrab`, which that path skips.
  It is absent rather than stale (the pass clears it on destruction). Nothing in
  the tree drives SSR, so this is untested either way; giving the camera frame its
  own depth grab means letting that pass take an explicit source render target.
- **Example coverage gaps.** Nothing exercises: gsplat SH bands 1-3, detail
  normals (upstream's `test/detail-map` cannot be ported faithfully — it toggles
  diffuse, normal and AO detail maps and only NORMAL exists here), fog of any
  type, sheen, or iridescence. The last three mean a change to those paths has to
  be driven deliberately to be seen at all.
- **What is left of the Vulkan/Metal gap is the INDIRECT term.** Direct lighting
  is done: after the shared BRDF landed (2026-09-06) `parallax-mapping` reads
  1.0003 of Metal. `clearcoat`, which is environment-dominated, still reads
  0.986. Treat that number with suspicion before chasing it — the frame is 20-30
  counts in the region that differs and the tonemap is not linear there, so
  re-measure against a brighter configuration first.
- **`clustered-spot-shadows`'s 19/255 difference on its normal-mapped cube faces
  is UNMEASURED since 2026-09-06.** It predates the spot cone fix, the falloff fix
  and the shared BRDF, all of which touch what it measures. Re-measure before
  treating it as a finding.

- **Upstream scales ambient diffuse light by `(1 - specularity)`; neither backend
  does.** Its forward backend applies it after `addAmbient`, per channel, whenever
  specular is on, in BOTH workflows. This port's shading has its own energy terms
  and never had this one, on either backend, so it is a parity item rather than a
  regression — found while aligning the default workflow, left for a shading pass.

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
