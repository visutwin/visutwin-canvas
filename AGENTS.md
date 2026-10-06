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
  engine/          # Core 3D engine (353 .h + 249 .cpp + 1 .mm = 603 files)
    src/core/      # Math (Vector2/3/4, Matrix4, Quaternion, SIMD multi-backend), shapes, events, tags
    src/platform/  # Graphics abstraction + Metal and Vulkan backends, input
    src/scene/     # Scene graph, renderer, materials, shader-lib, lighting, shadows
    src/framework/ # ECS (Engine, Entity, Components), asset loading, parsers, gizmos, input
    shaders/metal/chunks/   # 25 composable Metal shader micro-chunks (ShaderChunks registry)
    shaders/vulkan/chunks/  # 20 GLSL fragment chunks, same names (forward.frag #includes them)
    shaders/metal/embedded/ # self-contained MSL programs embedded at build time (particle sim/render, gsplat render)
    shaders/vulkan/         # GLSL stages + shared includes compiled to SPIR-V at build time (20 files)
  examples/        # 69 example applications derived from ExampleApp: upstream ports + one original scene (ambient-occlusion-davinci)
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
- **The Vulkan backend picks its DRIVER by ID, preferring MoltenVK.** The macOS Vulkan SDK
  (1.4.363 on) registers Mesa's KosmicKrisp beside MoltenVK for the same GPU, with the same
  device name, so the first suitable device was whichever ICD the loader listed first.
  Everything here is written and measured against MoltenVK; `VISUTWIN_VULKAN_DRIVER=kosmickrisp`
  (any case-insensitive part of the driver name) selects another, and the "Vulkan device"
  log line names the driver it got. Compare backends on MoltenVK unless you mean otherwise.

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

- **Test presets exclude the `vulkan`, `golden` and `gpu` labels.** `vulkan-validation-smoke`,
  `golden-images` and `gpu-instance-cull` need a GPU and a window, so `ctest --preset default`,
  `vulkan` and `sanitize` run the unit tests only, `ctest --preset vulkan-smoke` runs the smoke
  test, `ctest --preset golden` the golden images and `ctest --preset gpu` the tests that
  run kernels on this build's real device (`ctest -L gpu` in a Vulkan build). Every preset errors when it
  matches no tests, so a broken filter fails rather than passing on nothing.
- **CI is `.github/workflows/ci.yml`**, four jobs: `simd-backends` compiles the
  header-only SIMD contract test once per backend (SSE and scalar on Linux x86,
  NEON and Apple on macOS arm64) with no vcpkg; `macos-metal` builds the `default`
  preset WITH every example (`-DVISUTWIN_BUILD_EXAMPLES=ON`, built not run) and runs
  its tests; `macos-sanitize` builds the `sanitize` preset and runs the unit tests
  under AddressSanitizer and UndefinedBehaviorSanitizer; `linux-vulkan` builds the
  `vulkan` preset with GCC 14 and runs its unit tests. All are gating. The examples
  are built on macOS only — building them on Linux wants verifying on a Linux machine
  with the same presets and packages first.
- **`VISUTWIN_SANITIZE` (the `sanitize` preset: `address;undefined`) instruments this
  project's own targets, not vcpkg's**, and undefined behaviour aborts rather than
  printing and carrying on. A lifetime bug rarely changes a test's RESULT — a freed
  joint or batch source read back still looks plausible, which is how the tests for
  those were written to check the contract instead — so the sanitizer build is what
  catches the rest.
- **The tree builds with ZERO warnings under `-Wall -Wextra`** (`VISUTWIN_WARNINGS`, on
  by default; `-Wmissing-field-initializers` is off because `Desc{name}` partial
  aggregate init is an idiom here). The macOS CI jobs add
  `-DVISUTWIN_WARNINGS_AS_ERRORS=ON`, so a new warning fails the build; the Linux GCC
  job does not yet, because GCC's extra checks have not been cleared there. Keep a
  parameter a virtual default ignores by commenting its NAME out, not by `(void)`; a
  variable only an `assert` reads needs `[[maybe_unused]]` or an `#ifndef NDEBUG`,
  because Release drops the assert and a Release build warns where Debug does not.
- **Golden images are LOCAL ONLY** (`tools/golden_images.py`, `ctest --preset golden`
  on the `examples` build for Metal; the script with `--backend vulkan` on a Release
  Vulkan examples build — a Debug one runs the validation layer and is far slower). Nine
  deterministic examples render under `VISUTWIN_FIXED_DT`, are downscaled 4x and
  compared with `tests/golden/<backend>/<density>x/`: one reference set PER DISPLAY DENSITY
  (2x Retina, 1x a standard monitor), picked from the "Display pixel density" line the
  example harness logs. The examples render the SAME frame on both (one pixel per point,
  the same MSAA), so the two sets hold the same images; they stay separate so that a change
  that does depend on density is caught. The display is the one the window opens on (an
  external 1x monitor, or a sleeping display that comes back at 1x). The script strips
  `VISUTWIN_ANTIALIAS` and `VISUTWIN_MAX_PIXEL_RATIO` from the examples' environment. A density with no set
  SKIPS its cases; `--update` writes the set for the density it runs at, so a rendering change
  that is intended needs re-capturing at BOTH densities, on two displays, and on BOTH
  backends: a change re-captured for Metal alone leaves the Vulkan set failing for a
  reason nobody remembers, and the next real Vulkan difference hides behind it (skybox
  rotation did this to four Vulkan cases, which then masked a Metal-only clearcoat bug).
  Before re-capturing either set, compare its capture with the OTHER backend's: they agree
  to a fraction of a count, so a disagreement is a bug, not a new reference.
  A capture ignores the person at the machine: while `VISUTWIN_SCREENSHOT` is set the
  example harness drops every keyboard, mouse, touch, pen and gamepad event before anything
  sees it (window and quit events still arrive; `VISUTWIN_ISOLATE_INPUT=0/1` overrides), so a
  drag across the window no longer orbits the camera and fails a case for nothing. Both backends reproduce every reference bit for bit run to run, and
  a 1.03 factor on every lit colour fails all eight original cases, so a failure is
  real. When a
  rendering change is intended, look at the images it writes to
  `<examples-dir>/golden-failures`, then re-capture with `--update` and commit the new
  references with the change. The script needs numpy and Pillow: CMake checks
  `VISUTWIN_GOLDEN_PYTHON` at configure time (Homebrew's Python has neither;
  `/usr/bin/python3` does here).
- **`VISUTWIN_EXPECT_SIMD_BACKEND=sse|neon|apple|scalar` makes the SIMD test FAIL
  unless that backend is the one `defines.h` selected.** Only one backend compiles
  per build, and a missing flag falls through to another backend silently and
  still passes — testing the wrong code. Two traps it catches: Apple's
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
- **`ci.yml`'s `VCPKG_COMMIT` must equal `vcpkg.json`'s `builtin-baseline`.** CI checks
  vcpkg out at that commit and keys its binary cache on it; bump the two together. A
  local vcpkg must have that commit CHECKED OUT (or be newer): the baseline is read from
  git history, but each port's version entries from the working tree, so a clone that is
  only fetched fails with "no version database entry".
- **tinygltf comes from the baseline (3.0.0), through its v2 header `tiny_gltf.h`.** 3.0.0
  ships the v2 C++ API unchanged beside a new v3 header (`tiny_gltf_v3.h`), which the
  parser does not use. The overlay that pinned 2.9.7 over a regenerated archive's hash is
  gone; `vcpkg-overlays/ports/` now holds only the `visutwin-canvas` port for consumers.
- **Apple's libc++ hides missing standard includes; GCC's libstdc++ does not.**
  libc++ pulls `<cmath>`, `<cstdint>` and `<array>` in transitively, so a header
  that uses `std::sqrt`, `uint32_t` or `std::array` without including them builds
  on macOS and fails on Linux, and one such header can cascade into dozens of failed
  files. Include what you use; the Linux CI job is what catches it.
- **The Metal compiler's path changes when the system remounts its toolchain.** It lives
  under `/var/run/com.apple.security.cryptexd/mnt/...MetalToolchain-<ver>.<random>/`, and
  the random suffix can change with the version unchanged; every configured tree then
  fails to regenerate ("not a full path to an existing compiler tool"), because
  `enable_language(Metal)` records the compiler once in
  `CMakeFiles/<cmake version>/CMakeMetalCompiler.cmake`. `engine/CMakeLists.txt` deletes
  that record when the path it names is gone, so detection runs again. A worktree checked
  out at an older commit lacks the guard: delete the file by hand. If the default build's
  cache comes back without the vcpkg toolchain's variables (spdlog "not found" with the
  package installed), `cmake --preset default --fresh` rebuilds it.
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
ImGui/ImPlot overlay (`viz/overlay/`, uses `imgui_impl_metal`), marching cubes
and the spec-gloss map.

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
stages. What is asserted is the real invariant, that the size is a multiple of 16,
not a fixed size. The struct is a plain
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

What IS single-sourced is the block's CONTENT: `deriveLighting`
(`platform/graphics/lightingDerivation.h`) decides every value both layouts carry — the
sRGB decode of ambient, light and fog colours, the SH and view-projection packing, the
area-light up axis, the local shadow slots (spot matrix, omni near / far / RELATIVE bias,
the unused-slot defaults), the cookie slots, when a directional slot counts as active,
each light's colour / intensity split and which clustered lights the draw accepts — and
each backend's `setLightingUniforms` only lays the result out. The split is
`lightRadiance` (`platform/graphics/lightRadiance.h`), a reproduced quirk: at intensity 1
or more the colour is decoded and then scaled, below 1 it is scaled BEFORE the sRGB decode
(0.5 shades at about 0.22, not 0.5). Binders pack `DerivedLight::intensity`, never
`GpuLightData::intensity`, and anything else that shades a light (clusters, volumetric
fog, the GPU lightmapper's split sun copies) goes through the same function. A new
lighting value is decided there, then copied by both binders; a value derived inside
one binder lets the two backends drift apart.

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

**A material remembers the forward shader it resolved to, and three things key that
memo.** `getForwardShader` rebuilds the options, the program name and the variant key
only when the material's `uniformsVersion()` moved, the draw's flags differ, the
library's frame switches differ (`forwardFrameBits`) or the chunk registry changed;
otherwise it returns `Material::forwardShaderMemo()` (two entries, held weakly). So:
a material mutator that changes a FEATURE without `markUniformsDirty()` keeps the old
shader, and a new frame switch on `ProgramLibrary` must be added to
`forwardFrameBits()` or it changes no variant for a material already drawn. Debug
builds resolve in full on every memo hit and assert the two agree;
`tests/forwardShaderMemoTests.cpp` moves each input.

### Adding a texture slot

Bump `MetalTextureBinder::kMaxTextureSlots` AND add the slot to the
`materialSlots` clear list in `bindMaterialTextures`. Metal fragment SAMPLER slots 1-6 carry
the texture's own sampler for base colour, normal, metal-rough, occlusion, emissive and
lightmap (`kMaterialSamplerTextureSlots`, built from `textureSamplerState`, the same mapping
Vulkan's per-texture samplers use) — exactly the maps Vulkan binds as combined samplers. A
new map Vulkan binds as a combined sampler needs a sampler slot there, within the budget
comment in `metalTextureBinder.h`; a separate-image map keeps `defaultSampler` on both. Slots 0-38 are taken today
(31-33 the gloss, thickness and refraction maps, 34 the opacity map; 35 is the
second directional shadow map, 36 the clustered cookie atlas and 37-38 the VSM spot
moments, scene slots, not material ones). The SCENE slots are 2 and 6 and those listed
from 35; slot 7 is the clearcoat intensity map, a MATERIAL slot (a scene depth bound there
overwrote it on every draw and Metal rendered every clearcoat map as absent), so check
`bindSceneTextures` for a collision before giving a scene texture a slot.
On Vulkan, MoltenVK inherits a 16-SAMPLER-per-stage limit across all sets and the
fragment stage is at it, so a new material texture is a SEPARATE image
(`texture2D`) read through the shared sampler at set-1 binding 24, the treatment
the parallax height map, the detail normal, the displacement map, the three
clearcoat maps (7/13/14) and the gloss/thickness/refraction/opacity maps (31-34)
already get; the light cookies do the same on set 3.

The set-1 slot list lives in exactly one place, `kMaterialTextureBindings` in
`vulkanUniformLayouts.h`, and `vulkanMaterialBindingIsSeparateImage` beside it is
the ONE predicate for which of those are images rather than combined samplers —
the layout, the descriptor writes and the draw binding all call it, and the bundle
validator's expected table (`generate_vulkan_shader_bundle.py`) has to name the
binding too. APPEND a new binding to the list, never insert it in numeric order:
a quad pass's texture slot i is the i-th entry, so an insertion moves every quad
input after it. Do not copy the list into layout creation, the binding loop or the
descriptor writes: with copies, adding a slot to two of the three writes every
binding to the wrong index.

Set 3 (per-pass scene textures) is sized by `kSceneTextureBindingCount` and typed by
`vulkanSceneDescriptorType`: the layout, the descriptor writes and the pipeline's
reflection check all read those two, and the reflection check compares each
scene binding's KIND, not just its number. The bundle validator's table in
`generate_vulkan_shader_bundle.py` is the one other place to update.

### Metal buffer slots

0=vertex, 1=index (the scene block, vertex AND fragment), 2=model, 3=material,
4=lighting, 5=scene, 6=palette (dynamic batch + skinning, **mutually exclusive**),
7-8=clustered (fragment) / gsplat data and order, particle pool and sort order (vertex),
9=morph deltas / a mesh emitter's vertices, 10=morph params, 11=gsplat / particle params
(particle params reach the fragment stage too).

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
  press; the edge is recorded only when the key was actually up, and a RELEASE edge
  only when it was actually held (a key-up whose key-down was withheld by the UI, or
  one arriving after a focus loss already released it, is not a second release).
- **Losing window focus releases everything held.** A key or button held while
  focus moves away never sends its release, and would otherwise read as held for
  the rest of the process. The release EDGE is recorded too, so a caller watching
  for it is not left waiting.
- **`Key` enumerators ARE SDL scancodes**, so a key the list does not name still
  works through a cast, and the list needs no translation table to get wrong. They
  are POSITIONAL: `Key::W` is the key left of `Key::S` whatever it types.
- **Touch positions need the window size.** SDL reports normalized coordinates;
  `TouchDevice::setWindowSize` converts to window POINTS (the space `Mouse` and
  `ElementInput` use) and Engine keeps it current from `canvasSize()` on resize. Without it
  every touch lands in the top-left corner. Only a touch SCREEN's (direct) fingers are
  touches (`isDirectTouchDevice`): a trackpad's contacts arrive as finger events too.
- Examples must not poll SDL for input. `CameraControls` reads the engine's
  keyboard and mouse; there is no `platform/input.h` key sink.

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
  `CollisionComponent`'s shape, so authoring order does not matter. Mass, friction,
  restitution and damping then update the LIVE body (`PhysicsBody::setMass` / `setFriction`
  / `setRestitution` / `setDamping`) and an unchanged value does nothing; only the type, or a
  mass the backend refuses in place (0, "from the shape"), rebuilds it, and a rebuilt dynamic
  body keeps its velocities. Velocity, impulse, torque impulse, force and torque given before
  the body exists (or while a rebuild is pending) are STORED and applied at creation, before
  the step: a projectile launched in the frame it is spawned must not be dropped.
- **Static bodies do not get their transform written back** (nothing else should
  be fighting whatever placed them) and **kinematic bodies are pushed the other
  way**: the entity's transform goes INTO the simulation.
- **The world is stepped on `fixedUpdate`, not `update`.** `Engine::update` owns
  the accumulator and fires it zero or more times a frame at
  `Engine::fixedDeltaTime()`; `RigidBodyComponentSystem::step(dt)` is public for
  driving the simulation from another clock, and `setTimeScale(0)` pauses it
  (nothing steps, nothing is written back) while the rest of the engine runs on.
  `Engine::update` clamps the frame time to `maxDeltaTime()` (0.1 s) and multiplies it by
  `timeScale()` before anything sees it, the accumulator included; `VISUTWIN_FIXED_DT`
  raises the clamp to its own value, so a magnifying 2.0 step still gets through.
- **The CPU raycast fallback skips colliders that are not `active()`.** Testing the
  components' own `enabled()` alone would still hit a collider on a disabled entity,
  or under a disabled parent; `tests/raycastFallbackTests.cpp`.
- **`raycastAll` returns hits NEAREST FIRST on both paths**, so the order does not
  depend on whether a physics world has been supplied.
- **Only an ACTIVE body is in the world.** Disabling the rigid body, its entity, a parent or
  the sibling collision component destroys the body at once (joints let go first through
  `bodyWillBeDestroyed`), so nothing collides with it and world raycasts miss it; enabled
  again, it is recreated on the next step at the entity's CURRENT transform, at rest. A
  disabled joint destroys its constraint and is rebuilt when enabled again. Both systems
  visit INACTIVE components too, so the sync can remove them.
- **A joint lets go of its constraint BEFORE the world frees it.** The world destroys
  every joint touching a body it destroys, so `RigidBodyComponent` calls
  `JointComponent::bodyWillBeDestroyed(registry(), entity)` before each `destroyBody` (a setter
  that rebuilds the body, `releaseBody`, the destructor); each joint naming that entity
  drops its constraint and is rebuilt against the new body. A joint also watches its
  ends' `destroy` events: a destroyed end drops the joint and clears the reference, and
  the joint stays gone until an end is set again rather than re-pinning that end to the
  world. Without it the component would keep the freed pointer and call `isBroken()`
  on it every update. `tests/jointLifetimeTests.cpp` runs the components against a world
  that never frees its joints and counts every call on a dead one.
- **A dynamic body on a MIRRORED entity writes back the rotation it turned through,
  not a world rotation** (`RigidBodyComponent::setMirroredTransform`, upstream #9500).
  The rotation read from a mirrored world transform is not the entity's: 
  `Quaternion::fromMatrix4` negates the X axis of a mirrored basis, and two negative
  scale factors read as a 180-degree turn. Written back as the world rotation it would
  turn the entity on its first step (negative local Y, Y and Z, or a mirrored parent; a
  lone negative X is exactly what the extraction undoes and is right either way). The
  delta is
  applied to the LOCAL rotation, expressed in the parent's space and reflected through
  YZ under a mirrored parent. `tests/mirroredBodyTests.cpp` drives it with a world
  that holds bodies still or turns them by a known rotation.
- `teleport()` rather than `setPosition()` on a simulated entity: the step would
  overwrite a bare transform, and Jolt does not wake a body that was only moved.
- `CollisionComponent::height` is the FULL height for a capsule, caps included, and
  defaults to 2; the backend converts to Jolt's cylindrical half-height. A joint defaults
  to FIXED, and a ball joint's swing and twist limits of 0 mean UNLIMITED here.
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
  means. Built the other way round a slider told to run at +1.5 travels at -1.5.
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
ComposePassParams. The colour settings (vignette, fringing, grading, enhance, LUTs) are
ONE struct, `ComposeColorSettings` (`scene/graphics/composeColorSettings.h`), which both
settings structs derive from and the compose pass takes whole: a new one is a field
there plus its line in `RenderPassCompose::execute`, not a copy at every hop.

- DOF runs BEFORE SSAO. Occlusion multiplies the already-defocused colour and is
  not itself blurred, so it keeps full strength out of focus; the other way round
  the defocus washes it out with everything else. Where DOF is not blurring the
  order cannot matter, which is the check that says a change here landed: the
  in-focus part of the frame must come back bit-identical.
- **The CAS uniform is NEGATIVE.** `RenderPassCompose` remaps the user
  sharpness to upstream's `lerp(-0.125, -0.2, s)` and the shaders gate on `< 0`;
  a positive weight turns the same kernel into a 5-tap blur. Verify a sharpness change
  with gradient energy over
  a static crop, not by eye.
- Fringing (chromatic aberration, user intensity /1024) **must stay BEFORE
  bloom**: it re-samples the scene texture for R and B, so running it after bloom
  leaves bloom in green only. It also overwrites R and B from the raw scene
  texture, which paints magenta over occluded pixels if combined with
  compose-mode SSAO. That is upstream's own design; fix the scene, not the engine.
- **Bloom has a THRESHOLD** (`RenderingSettings::bloomThreshold`, upstream
  `CameraFrame.bloom.threshold`, 192121560): a soft-knee high pass (knee = half the
  threshold) compiled into the FIRST bloom downsample only while the threshold is
  above zero, applied to the Karis-filtered result rather than per tap. It is in
  scene-referred units, before exposure. Crossing zero rebuilds the bloom chain; at 0
  no variant exists and the frame is bit-identical to a build without it (check it
  on `post-processing`, both backends, driven by `VISUTWIN_BLOOM_THRESHOLD`).
- The 3D LUT is a 256x16 Unreal strip with dual-LUT blend; the port loads it
  non-sRGB so the sample is pow(2.2)-decoded in-shader. Test asset:
  `assets/textures/lut-teal-orange.tga`.

**Frame graph store propagation.** `FrameGraph::compile` walks the passes and,
whenever a later pass reads a target WITHOUT clearing it, marks the earlier pass
on that target as having to STORE. The back buffer is included (`nullptr` render
target), and `_renderTargetMap` is per-frame state. Grab passes carry no color ops
of their own and must not displace the real draw pass in that map. **The graph's edits
to a pass last ONE frame**: passes persist while the graph is rebuilt every frame, so
`compile()` records each store it raises and each cubemap mip generation it drops
(`RenderPass::setAttachmentFlagByGraph`) and undoes them at the next compile before
deriving them afresh, restoring only a flag that still holds the graph's value; a
one-way edit would let one frame's adjacency stick to a pass for good. A pass that must
keep its target for a LATER frame (history, persistent accumulation) sets its own store:
the graph only sees one frame. `tests/frameGraphTests.cpp` holds propagation, merging,
before-pass order and the undo; `tests/shaderCompositionTests.cpp` holds `forward.frag`'s
include order against ProgramLibrary's registered GLSL order, override precedence and
variant keys.

**Tone mapping.** 6 modes dispatched in `common.metal :: toneMap`: LINEAR (0),
FILMIC (1), ACES (3), ACES2 (4, Stephen Hill RRT+ODT fit), NEUTRAL (5), NONE (6).
Set scene-wide with `Scene::setToneMapping` or per camera with
`CameraComponent::setToneMapping` (defaults to `TONEMAP_INHERIT` = -1). NOTE:
`RenderingSettings::toneMapping` is a separate, currently **unread** field —
`applyCameraSettings` never copies it into `CameraFrameOptions`. Use
`setToneMapping`.

**The forward tail tone-maps the lit colour UNCLAMPED on both backends**; only the gamma
encode clamps at 0. A channel driven negative (an albedo above 1 under metalness: the
ambient is scaled by 1 - F0) comes back from ACES through its own formula and clamp, as
upstream's does; clamping before the curve turned it black on one backend (`refraction`'s
blue ring objects were yellow on Metal).

**Tone mapping NONE applies NEITHER curve NOR exposure**, as upstream's
`tonemappingNone`. On Vulkan exposure is applied inside the dispatch
(`toneMapExposed` in `common-tonemap.glsl`, the twin of Metal's `toneMap(color,
exposure, mode)`); a forward caller that multiplies by exposure first exposes NONE
on Vulkan alone. The compose pass does the same in both languages.

**Under CameraFrame the forward pass must output LINEAR HDR** and leave exposure,
tonemap and gamma to compose. The gate is bit 5 of `LightingData::flagsAndPad[0]`,
kept in step with `hdrPass()`. Every shader path that returns early — the tail,
all three sky paths and the unlit path — has to check it, or compose applies gamma a
second time. The UNLIT return is the easy one to miss: without the check an unlit HDR
emissive under a camera frame (`post-processing`'s World-layer label) renders grey and
does not bloom. The unlit path also sRGB-decodes its emissive map, as the lit path does,
on both backends (`area-picker` shows a missing decode).

## Shader System

`ProgramLibrary` with a two-level cache: variant key -> source composition ->
compiled binary. **ShaderChunks registry** (`shader-lib/shaderChunks.h`): named
micro-chunk files (file stem = chunk name) concatenated per registered program
order with `#define VT_FEATURE_*` guards. Overridable globally via
`getProgramLibrary(device)->chunks().set(name, src)` and per material via
`Material::setShaderChunk(name, src)`; resolution is material > registry >
default. Both override sets' FNV content hashes fold into the variant cache key.
Metal chunks hot-reload from the source dir per launch.

**Both backends.** `engine/shaders/vulkan/chunks/` holds 20 GLSL **fragment**
chunks under the same names, and `forward.frag` is a 28-line file that `#include`s
them, so the build-time bundle and the runtime composition share one source.

- `ProgramLibrary` registers a separate GLSL chunk order that **must stay in step
  with `forward.frag`'s `#include` order**.
- Override source must be in the device's language (`GraphicsDevice::shaderLanguage()`).
- Vulkan hands composed GLSL to `createShader` only when an override actually
  changed it; otherwise it passes an empty string and gets the prebuilt bundle.
- `forward-vertex`, `shadow-vertex` and `shadow` have no chunked GLSL form and are
  Metal-only; overriding them on Vulkan logs a warning.
- **Fog has a TYPE** (`FogParams::type`, `Scene::setFogType`): NONE/LINEAR/EXP/
  EXP2, uploaded in `fogStartEndType.z`, where 0 also means off. Fog depth is
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
- **Verify the measurement before believing the finding.** A bad experiment, not bad
  code, can produce a "bug" that does not exist. Three rules, each of which catches
  one: capture the reference and the change from ONE source state (a
  screenshot taken before an example was instrumented reads every unrelated
  difference as a regression); check the STIMULUS actually reached the code (a
  test hook placed above the example's own setter was overwritten every run); and
  prefer a one-line `spdlog` of what the binder receives over a shader probe,
  which puts a transfer curve and a bundle rebuild between you and the answer.
  A fourth: when a change moves a LOT of pixels by very LITTLE (a few percent of the
  frame at one count, with a thin tail at silhouettes), suspect something that moved the
  CAMERA or the geometry, not the shading — and when two targeted reverts each change the
  count by nothing, stop guessing and BISECT. Revert every changed file in a scratch
  clone, confirm that build reproduces the reference EXACTLY (0 pixels; without that
  check the bisection proves nothing), then add the files back in halves. For example, a
  camera forward vector rebuilt through a quaternion that agrees with the trig it
  replaced only to 3.6e-7 is enough to shift every pixel by a count.
- **`common-brdf.glsl` is the twin of `common-brdf.metal` and both must change
  together.** It owns `distributionGGX`, `getVisibilitySmithGGX` (a VISIBILITY
  term — the `1/(4 NdotL NdotV)` is folded in, so call sites write `D * Vis * F`
  with no division), `getFresnel` (gloss-aware, DIRECTIONAL lights only —
  punctual lights take bare specularity), `getFresnelCC` and
  `getVisibilityKelemen`. A shading edit that lands in one language only is a
  backend divergence by construction, and a private copy of a BRDF term in another
  chunk is the same divergence inside one language.
- Keep each GLSL chunk a self-contained override target. The material-flag
  constants and `applyUvTransform` live in `common-material-flags`, not in
  `common-tonemap`, so a minimal tonemap override does not drop them.

**Fullscreen effects use `QuadRender`** (`scene/graphics/quadRender.h`), not
device virtuals, and get their shader from `getOrCreateQuadShader` (`quadShader.h`: the
device cache, the source built only on a miss) with the vertex stage, vertex input and
`QuadVarying` from `quadShaderSource.h` (`VT_QUAD_MSL_PRELUDE`, `VT_QUAD_MSL_VERTEX(name)`,
`VT_QUAD_GLSL_VERTEX`) rather than a copy of them. A quad pass is otherwise a shader,
up to 8 input textures on fragment slots 0-7, and one uniform block. The block rides the per-draw MATERIAL slot (Metal buffer 3 / Vulkan
set 0 binding 0) via `GraphicsDevice::setQuadUniformData`; `kPerDrawUniformCapacity`
(640; `MaterialUniforms` itself is 560 bytes and is asserted to fit) sizes that slot, the Vulkan material descriptor's range, and the padded
allocation behind it. A smaller block is copied into the front of a full-size
allocation, so never shorten the allocation. Quad passes draw an oversized
fullscreen TRIANGLE and, on Metal, bind `_postSampler` (linear, clamp, no anisotropy),
not the scene sampler; Vulkan samples a quad input through the texture's own sampler.
**Both must reach a texture's MIPS**: Metal's post sampler has a linear mip filter because
Metal's default ("not mipmapped") samples level 0 whatever `level()` asks for. Without it the
env bake's Lambert convolution (LOD 4.5 and up) read the full-resolution cube, and an atlas
baked from one HDR had an ambient rect matching Vulkan's at a correlation of 0.35 (now
0.9999, and 1.0 for the reflection levels). Single-mip render targets cannot tell the
difference; the golden images stayed bit-identical.

Migrated: VSM blur, volumetric fog, CoC, DOF blur, depth-aware blur, compose,
SSAO, TAA, the whole env family (equirect-to-cube, reproject, convolve, atlas —
see `scene/graphics/envBake.h`), and the GPU particle simulation, which runs
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
Metal encodes the bake into its open command buffer like any other pass and never
WAITS (inside a frame it is submitted with the frame, outside one it is committed
when the scope ends; `reflection-probe-dynamic` re-bakes every frame, where a stall
would serialise CPU and GPU); Vulkan records into a one-shot buffer and WAITS, because that is what
makes reusing the frame-scoped uniform ring and descriptor pools safe. A bake must
also set its own blend, depth and cull state — nothing outside the frame graph
has — and `beginOfflineWork` flushes pending uploads first, because a texture
created without host data marks its tracker SHADER_READ_ONLY while the actual
transition is still sitting in the deferred upload queue. `endOfflineWork` flushes
AGAIN before submitting, as the frame path does before its own submit: building
the first `RenderTarget` over such a texture RECREATES its image
(`Texture::setRenderTargetUse`), which for a bake happens inside the scope, and the
new image's queued transition must land ahead of the barriers recorded against its
tracker; landing after them, every env bake renders, mips and samples a cube the GPU
still has UNDEFINED (VUID-vkCmdDraw-None-09600 in the smoke test). Any new one-shot or
offline submit owes the
same flush immediately before `vkQueueSubmit`.

## Examples

**Shared example and test code.** Examples build UI elements through `examples/uiElements.h`
(each example passes its own defaults), screens and UI systems through `ExampleApp::createScreen`
/ `registerUi`, read assets through `Asset::resourceAs<T>()`, and share `grid.h`,
`transformGizmoExample.h` and `ssaoKeys.h`. Unit tests use `tests/support/`: `check.h`
(`check`, `near` / `nearStrict` with an explicit epsilon, `finish`), `stubDevice.h` (one
configurable stub device), `testEngine.h` and `gltfModel.h`, and register through
`visutwin_add_unit_test` in `tests/CMakeLists.txt`. Do not paste a local copy of any of them.

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

- **The SPIR-V bundle depends on EVERY file under `engine/shaders/vulkan/`**
  (`file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` in `engine/CMakeLists.txt`), so a
  chunk or header edit regenerates it in any build that reaches the engine target,
  and a new chunk file joins without a reconfigure. Do not go back to a hand list:
  one that names only the top-level stages leaves a chunk edit unregenerated even in
  a full build (so you measure the previous binary), and it drifts from the
  generator's own list.
  Note the bundle is one header included by many engine sources, and Ninja
  rebuilds by mtime, so a comment-only chunk edit still recompiles those files.
- **A fullscreen quad's v runs DOWN the screen** (row 0 at the top, as every texture
  here), so a pass that rebuilds a view ray from its uv takes NDC y = 1 - 2v, not
  2v - 1. Upstream writes `uv * 2 - 1` because WebGL's uv0 runs up; copied as is, the
  volumetric fog marched every ray mirrored top to bottom, burying the top of the frame
  in the dense low fog and hiding a mountain peak behind it, with nothing else wrong to
  see. `volumetric-fog-local-lights` shows the peak.
- **Depth of field is the MULTI-PASS pipeline, and a Vulkan quad slot is an INDEX
  into the material binding list.** `RenderPassCameraFrame::setupDofPass` builds
  upstream's FramePassDof — a CoC pass from the scene depth, a far pass that box-
  downsamples the scene premultiplied by the far CoC, a bokeh blur over upstream's
  concentric kernel (generated in the shader; the radius is a fraction of a 540-row
  reference frame), and `applyDof` in compose mixing by the CoC sum, with a 3x3
  CoC-weighted upsample at low quality. It relies on the frame graph's handling of a
  targetless orchestrator pass (`RenderPass::render` skips an uninitialised target and
  the graph recurses into its before-passes). The compose keeps `applyDofSinglePass`
  only as the fallback when no CoC texture is bound; it has no near blur. The box
  downsample implements `premultiplyTexture` as a variant keyed on the channel. On
  Vulkan quad slot i is `kMaterialTextureBindings[i]`: 0-5 are bindings 0-5, slot 6 is
  binding 17 (a SEPARATE image — declare `texture2D` and sample through the extra
  sampler at 24) and slot 7 is binding 19 (`sampler2D`). Mapping a quad slot to the
  set-1 binding of the SAME NUMBER leaves slots 6 and 7 with no binding, and a quad
  shader declaring `(set = 1, binding = 6)` makes MoltenVK's translation drop the
  samplers of OTHER textures ("undeclared identifier _NSmplr" on bloom). Also:
  `textureSize()` on a combined sampler does not survive the MSL translation either;
  pass sizes as uniforms.
  Under TAA the blur must read the TAA OUTPUT, which alternates between two history
  textures: the camera frame retargets the half-resolution copy and (upstream #9591) the
  high-quality far pass (`RenderPassDof::setSceneTexture`) every frame; a far pass that
  blurs the raw jittered frame shimmers with a static camera.
- **A buffer the GPU wrote is read back through `VertexBuffer::read`**, the buffer's twin
  of `Texture::read` below: it blocks, Metal flushes and fences then copies from shared
  storage, Vulkan stages through a one-shot copy and refuses while recording. `storage()`
  is the CPU copy the buffer was made from, never what a kernel wrote since.
- **Reading a texture back goes through `Texture::read`, and reading one the GPU
  wrote means reading it through STAGING.** The seam is `Texture::read` →
  `gpu::HardwareTexture::read`: Metal blits into a shared-storage texture, Vulkan
  copies into a host-visible buffer on a one-shot command buffer, and both BLOCK.
  Reaching past it for the native handle is the trap, because neither backend
  reports the mistake — `MTL::Texture::getBytes` on a device-private render target
  does not fail, it answers with whatever is mapped, and what comes back is
  plausible rather than blank. Round-trip a known pattern to tell
  a working readback from a convincing one; `tests/vulkanSmoke.cpp` does, under
  validation, which also checks that the read hands the subresource back in the
  layout it borrowed. The Vulkan path refuses outright while a frame or an offline
  scope is recording: that work is not submitted, so a one-shot read would run
  ahead of the very commands whose output is being asked for.
- **An offline bake needs the per-draw uniform RINGS, which are frame-scoped, and a bake
  BEFORE THE FIRST FRAME (an env atlas built at load time, as upstream's examples do) is
  the special case.** `frameStart` is what advances a ring onto a region of its own. Before
  the first one, Metal's rings write into region 0 (a frame index of -1 used to put every
  write at a NEGATIVE offset: a SIGBUS in `MetalUniformRingBuffer::allocate` from inside
  `generateAtlas`), and `endOfflineWork` WAITS for that work and starts the region over
  (`resetBeforeFirstFrame`), because frame 0 reuses it. Load time only: once frames run, an
  offline scope outside a frame is committed and not waited on (`reflection-probe-dynamic`
  re-bakes every frame in `postRender`). Both backends bake an atlas before the first frame
  byte-identically to one baked five frames later (`annotations`, read back with
  `Texture::read`); test a change here the same way.
- **A Metal quad draw must not key its uniform allocation on the material.**
  `submitPerDrawUniforms` reuses the previous ring offset when the material pointer
  is unchanged, and a quad pass has no material of its own — nothing clears the
  bound material for an offline bake either — so keyed on the material, every quad
  draw after the first in a pass silently shares ONE uniform block. That is invisible
  while a pass draws a single quad and breaks a pass that draws a rect list (the env
  atlas: its convolve draws would read the reproject block). `draw()`
  passes a null KEY whenever the quad block is in use. A key is what the material-slot
  block came from: the material, null for a block nothing may share, or
  `MetalUniformBinder::sharedDefaultBlockKey()` for a draw with no material at all, whose
  default block is uploaded once a pass (every opaque shadow caster and prepass draw
  carries it).
- **A shader that exists in MSL and GLSL is only shared by CONVENTION.** The two
  bodies in a `*Shaders.h` sit in separate raw strings, and a migration that
  unified the uniform BLOCK does not unify the code: one language can carry an older,
  cruder implementation of a stage unnoticed. Before blaming a backend's
  lighting for a brightness gap, read the two bodies of the shader that produced
  the pixel side by side.
- **stb_image's vertical-flip flag is set ONLY through `StbVerticalFlipScope`**
  (`framework/assets/stbImageFlip.h`). stb keeps a global flag and a thread-local
  one, and once the thread-local flag is set on a thread it overrides the global
  one there for good — stb cannot unset it. The GLB parser decodes flipped through
  the thread-local flag, so a loader that clears only the GLOBAL flag is ignored
  after a GLB on the same thread: an environment atlas comes out upside down, a
  bitmap font's glyphs flip, and an OBJ "flip" through the global flag is silently a
  no-op. The scope sets the thread-local flag
  and restores the enclosing value on exit, so a flip cannot leak into the next
  decode. Never call `stbi_set_flip_vertically_on_load*` directly, and never
  include `stb_image.h` from a header: stb guards its declarations but not its
  implementation, which `asset.cpp` compiles. `tests/stbImageFlipTests.cpp` loads
  an asymmetric atlas through the font path after a leaked flip.
- **`atan2(0, 0)` is undefined, and a normal of exactly +/-Y hits it** — which is
  every fragment of an unrotated ground plane, the most common surface there is.
  Metal returns an out-of-range azimuth, so `mapAmbientUv` maps outside its rect
  and the plane reads its irradiance from the ROUGHNESS column instead: a ground
  plane lit by a blue sky comes back dark navy. Both `toSphericalUv` and
  `dirToEquirect` pick azimuth 0 at the pole. Any new direction-to-equirect
  code owes the same guard. An atlas baked FROM a cube samples it along d with NO X flip;
  only the shading lookups negate X (as the sky does with its cube). A flip in the bake
  mirrors every reflection and the ambient against the sky drawn from the same HDR, which
  no single image shows wrong; `tests/envAtlasOrientationTests.cpp` (label `gpu`) bakes an
  asymmetric equirect and holds it.
- **A Metal texture is SHARED unless it is a render target created without host
  data, which is PRIVATE.** Shared storage is what `replaceRegion` uploads need; a
  render target in shared storage forgoes lossless framebuffer compression on Apple
  GPUs. `RenderTarget`'s
  constructor marks its attachments (`Texture::renderTargetUse`); the GPU object
  already exists by then (the texture constructor creates it), so the mark
  RECREATES one that holds no host data, and a later CPU write into a private
  texture stages through a blit (`MetalTexture::writeRegion`). Two traps:
  `Texture::hasLevels()` is true for EVERY texture (the constructor sizes the level
  table) — `hasHostData()` is the "created empty" test; and `replaceRegion` on a
  private texture is a Metal assertion, not an error.
- **A GPU-time claim needs both builds in ONE session, run interleaved.** This
  machine's GPU clock state moves ambient-occlusion's frame by a millisecond
  between days and by half of one between consecutive runs, which is the whole
  size of most effects worth chasing. Single-run ablations "find" a millisecond that
  measures 0.0 once the previous commit is copied out as a second binary and the two
  are alternated three times each. Keep the old binary, alternate, take medians.
  Better still, alternate INSIDE ONE PROCESS: a setting that can change at runtime
  (MSAA sample count through `setRendering`, a shader option) toggled every 120
  frames shares one clock state by construction, and the passes the setting
  cannot touch are the control that says the windows are aligned (measured that way,
  4x MSAA costs the forward pass about 0.05 ms at 900x700, with the same limiter mix
  at both counts). A pass the setting cannot touch reading slower between separate
  recordings — SSAO under MSAA, say — is the signature of a clock-state difference.
- **Our HUD's GPU figure and upstream's are not comparable as read, for three reasons
  that are not the profiler** (both profilers reproduce `xctrace` per-encoder intervals
  within 10%). (1) PIXELS: since 2026-10-06 both render 900x700 pixels for a 900x700
  window (`ExampleApp` at one pixel per point, upstream's `GraphicsDevice` capping
  `maxPixelRatio` at 1); measurements before that date ran ours at a 1800x1400 drawable
  on Retina, four times the pixels. Both multisample the back buffer 4x unless the example
  turns it off, as upstream's do. (2) CLOCK: the
  `gpu-performance-state-intervals` table shows the GPU in its MINIMUM state 55-88% of the
  time under either engine, and every pass costs 2-3x more there than at Maximum; bucket
  per-frame costs by state, or run both engines at once so they share one clock. (3) A
  pass interval is a WALL interval on a shared GPU: with our window on screen WindowServer
  composites a 29-encoder buffer every frame at 120 Hz INSIDE our forward pass's interval,
  and the hardware counters show the forward window as ~1 ms of shading followed by ~1.4 ms
  at zero utilization with the GPU still "Active" — a stall, not our work — which is why
  the forward pass's interval barely changes between 900x700 and 1800x1400. Two traps in
  the measurement itself: `xctrace record --launch` leaves a SECOND instance of the example
  running after its time limit (start the binary yourself and record `--all-processes`;
  `pgrep -fl visutwin-ambient` before every recording), and a browser's frames must be
  grouped by command buffer with 5+ encoders, not by Metal's `frame-number`.
- **`xctrace` gives a GPU capture without the Xcode GUI.** `xcrun xctrace record
  --template 'Metal System Trace' --time-limit 8s --env VISUTWIN_BACKEND=metal
  --launch -- <binary>`, then `xctrace export --xpath '/trace-toc/run[@number="1"]/
  data/table[@schema="metal-gpu-intervals"]'` for per-encoder vertex and fragment
  intervals (values are id/ref compressed; resolve refs), and
  `--instrument 'Metal GPU Counters'` adds `gpu-counter-value` (ALU, texture,
  imageblock, interpolation limiters, occupancy, partial renders, sampled every 10
  us — join to the intervals by time). The trace holds EVERY process's GPU work,
  and the process name sits at the end of each interval's label, "(name (pid))" —
  filter on it FIRST. This engine submits a frame as ONE command buffer with an encoder
  per pass, in pass order (`ambient-occlusion`: prepass, SSAO, blur H, blur V, forward,
  compose, overlay), split in two or three where a pass with many draws committed early
  (see the open command buffer below); an encoder's fragment work is split into several
  rows (depth 1, 2) when another process's work preempts it, so SUM the rows per
  encoder. A browser running upstream's example shows one seven-encoder buffer per frame
  as well, so the process name is the only thing that tells the two apart. Encoder labels and debug groups set on the encoder do NOT
  reach the export's labels; only Xcode's own capture shows them. Before recording,
  `pgrep -fl visutwin` — a stray instance of the example inflates every duration in
  the trace with its overlapping passes.
- **An unbound Metal texture reports nonzero `get_width()` but samples zero** on
  Apple GPUs. Every optional texture sample must be gated on its flags bit or its
  runtime enable (`setEnvAtlasEnabled`, `hasSpecGlossMap` bit 21).
- **Depth taps in a quad pass must be POINT sampled, and neither backend does it
  for free.** A quad pass reconstructs view-space positions from depth, and a
  bilinear tap straddling a silhouette returns a depth belonging to neither
  surface — a position in mid-air the kernel then treats as an occluder. Whether
  hardware filters a depth format at all is a per-format capability, so leaving it
  to the texture's own sampler gives linear taps on Metal and part-nearest taps on
  Vulkan. Vulkan binds `_shadowSampler` (nearest, clamp, mip-less) for any depth
  texture in the quad path and the MSL passes declare their own point sampler
  (`depthPointSampler`) — SSAO, the depth-aware blur, SSR, TAA, CoC, volumetric fog
  (march and combine) and compose's single-pass DOF fallback; none reads depth through
  the pass's LINEAR sampler. A tap that lands on a depth texel centre barely notices;
  it matters where a tap does not, as in the fog upsample's offset taps, which no
  example drives. Establish this kind of thing by making the shader REPORT it: sample at
  a texel centre, one texel across, and exactly halfway, then check whether the
  halfway tap is the average. Reading the sampler-creation code is not enough.
- **A pass that reconstructs a position from a depth tap must SNAP the tap's UV to
  the centre of the texel it reads** (`snapToDepthTexelCenter` in both SSAO
  bodies). Point sampling returns the texel's depth, but an unsnapped UV places
  the reconstructed point up to half a texel away from where that depth was
  rendered — a plane comes back as a staircase and the SSAO kernel occludes a flat
  surface with ITSELF (upstream #9112). Unsnapped, the raw factor on a flat wall or
  floor falls well below 1.0 and carries a dither that the depth-aware blur turns into
  3-4 px stripes across every flat surface. Read the RAW factor, not the frame: divide
  a combine-mode capture with blur off by one with SSAO off over a flat region — a
  plane must return 1.0 with zero variance, while the stripes in the finished image
  are under one count and read as shadow acne.
- **Screen-space derivatives are undefined inside the per-light loop**, which sits
  behind fragment-varying `continue`s. An undefined mip LOD reads a fully averaged
  mip — a cookie becomes a flat wash of its own average. Sample with an explicit
  LOD 0 (`level(0)` / `textureLod`). That includes EVERY shadow tap, on both
  backends: the cascade dither puts neighbouring pixels in different atlas
  quadrants, and an implicit-LOD `texture()` under an anisotropic sampler (Vulkan)
  then averages other cascades into the tap, so each dithered pixel comes out darker
  than either cascade alone.
- **`cascadeBlend` is a FRACTION, as upstream, and 0 turns off both of its jobs.**
  It dithers the cascade pick from `cascadeBlend x` each cascade's end distance to
  that end (upstream's `ditherShadowCascadeIndex`, its hash included), and fades the
  shadow to lit by `smoothstep(cascadeBlend x distance, distance, depth)`; beyond
  the shadow distance nothing is sampled. It is not a WIDTH in world units (read that
  way, `shadow-cascades`' 0.1 is a 0.1-unit cross-fade, i.e. none). Note upstream's own
  JSDoc says 0.1 fades "the last 10%" while its
  shader, which the port follows, fades from 10% of the distance on. A dithered
  cascade pick must look the same whichever cascade a pixel lands in; if it shows
  as noise, force the pick to always and never switch and compare the three.
- **Under clustered lighting NO local light enters the main light array.** Every
  spot and omni is in the cluster grid and its shadow comes from the
  LightTextureAtlas; the main-array allocation clears `castShadows` when its two
  slots run out, so a clustered light routed through the array would lose its shadow
  past `ShadowParams::kMaxLocalShadows`. Only lights VISIBLE this frame take clustered
  atlas slots (the split follows their count), and `resetLightVisibility` clears every
  light's allocation each frame, so a culled light never keeps a rect another light now
  owns. A light enters the grid only with a dynamic or
  lightmapped mask (`clusterAdmitsLight`: a bake-only light must not light at runtime), and
  each draw accepts a clustered light only when the light's bits (`areaHalfHeight.w`) meet
  the one its mesh mask selects (`clusterParams2.y`: dynamic if the mesh has it, otherwise
  lightmapped). Clustered lighting is ON by default, as upstream,
  so a scene that needs the non-clustered path (PCSS local shadows) has to say
  `setClusteredLightingEnabled(false)`. Clustered COOKIES come from a cookie atlas laid
  out like the shadow atlas (same slot rects, `RenderPassCookieRenderer` copies each
  cookie in when its light gets a slot), and only while `LightingParams::cookiesEnabled`
  is set (off by default, as upstream). A cookie light takes an atlas slot whether or
  not it casts a shadow; a cookie-only spot gets its projection from
  `LightCamera::evalSpotCookieMatrix(light, viewport)`. As upstream, a clustered omni
  cookie ignores the light's rotation (the faces are world-aligned).
- **The clustered shadow atlas follows `LightingParams::shadowAtlasResolution` LIVE.**
  `LightTextureAtlas::configure` only records the values and runs every frame, right
  before `update()` in `ForwardRenderer::buildFrameGraph`; a changed resolution
  RESIZES the atlas texture and target in place (the ShadowMap wrapper the lights
  hold and the raw pointer the device binds stay valid, and each backend retires the
  old GPU image itself), bumps the version so every light is re-slotted, and re-arms
  the one-shot shadows for one render. Resizing is safe: toggling 512 and 2048 every
  five frames hangs neither backend. Configure it where it updates, not
  later in the frame: the first update creates the texture from whatever was
  recorded, and a configure that runs afterwards costs a 2048 allocation on frame
  one and a resize on frame two.
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
- **A clustered OMNI receiver's normal offset is scaled by (1 - NdotL) and by the
  DISTANCE to the light** — upstream's `normalOffsetPointShadow`, on the GEOMETRIC
  normal; a clustered spot keeps the flat `N * normalBias`
  (`getShadowCoordPerspZbufferNormalOffset`). A torch mounted on its own wall lights
  that wall
  at ~90 degrees from tens of units away, where the flat 0.2 units is ~30x short of
  upstream's; the non-clustered omni path still applies NO receiver offset.
- **The X of dark wedges around each `ambient-occlusion` torch is the torch mesh's
  OWN shadow, not an atlas seam.** The light sits at the mesh's aabb centre, as
  upstream places it, and a live upstream frame shows the same four sectors.
  Diagnose a suspected seam by making the shader REPORT it — force the visibility
  to 1, then to 0, within a fraction of the face edge and see whether the artefact
  follows the zone (this X does not: its sectors extend to ~20% of the face) — and a
  suspected caster by turning off `castShadows` on that mesh alone (the X vanishes).
  The
  omni shadow term of a frame is the ratio of a capture with omni shadows on to one
  with them off, the same trick that recovers the raw SSAO texture.
- **One cluster grid per DISTINCT LIGHT SET, not one for the frame.** The local
  light list is per (camera, layer) — the gather filters on
  `LightComponent::rendersLayer` — so the grid has to be too.
  `Renderer::clustersForLightSet` keys pooled grids on an order-independent hash of
  the set, so two layers seeing the same lights still share one and it is still built
  once; `resetClusters()` drops the assignments each frame while the pool survives,
  because the cell buffers are too big to reallocate per frame. EVERY layer binds its
  own grid and params.

  ONE grid built from whichever layer renders first, with the whole block — BINDING
  INCLUDED — skipped for every layer after, lights a layer with a different light set
  by another layer's cells, and leaves a layer with NO clustered lights (which never
  reaches the branch that zeroes the params) lit by the previous layer's buffers. Both
  `clustered-lighting` and `clustered-spot-shadows` have exactly that shape: two
  distinct sets per frame, one of them empty.
  `Renderer::lightSetHash` is the key and `bindLayerClusters` the per-layer bind;
  `tests/clusterGridSharingTests.cpp` holds sharing, the pool and the zeroing, and fails
  with the grid shared frame-wide.
- **The cluster grid is sized from the LIGHTS alone, and a spot is bounded by its
  CONE.** `WorldClusters::update` takes no camera: the bounds are the union of the
  light AABBs, as upstream's `evaluateBounds` does. Bounds started from the camera
  (padded on every axis) make the grid a cube around the viewer wherever the lights
  actually are, with coarse cells and most of them empty. Bounding by the lights loses
  no lighting — the shader ignores any fragment outside the
  grid, and a fragment outside the union of every light's bound is outside every
  light's range by construction, which is exactly what `lightBounds.h`'s containment
  property guarantees.

  `spotConeAabb` is the exact bound of the spherical sector, from its support
  function, and is a DEVIATION in the tighter direction from upstream's transformed
  box. A range-sphere bound is about thirty times the volume at a 20-degree cone.
  `tests/lightBoundsTests.cpp` holds containment, tightness and monotonicity —
  a bound that is too small drops lighting and reads as a falloff, one that is too
  large reads as nothing at all, so neither is visible in a render.
- **Spot cone falloff is a SMOOTHSTEP between the two cone cosines**, and local
  inverse-squared falloff is `16 / (d^2 + 1)`, not `1 / d^2`. Upstream's `spot.js`
  and `getFalloffInvSquared` define both and both backends follow them. A squared
  linear ramp for the cone gives half the light at the middle of the penumbra,
  agreeing only at the two ends, and a bare inverse square makes a light at four units
  read a fifteenth of upstream. `getSpotEffect` lives beside `distanceAttenuation` in
  `common-material-flags` and both Vulkan call sites (non-clustered and clustered) use
  it; do not spell the cone a second time.
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
- **Every shadowed local light culls from ONE caster list a frame, and its passes draw
  the lists that cull prepared.** `ShadowRendererLocal::cullLocalLights` collects the
  scene's casters once (`collectLightIndependentShadowCasters`: the collector, then
  `visible()` and the caster rules that depend on no light, with each caster's bounds
  copied beside it), and per light keeps what its faces see — an omni through the
  six-face classification, a spot through its cone's frustum — in
  `LightRenderData::visibleCasters`, stamped with the frame's `renderVersion`, as the
  directional cascades are. Collecting or filtering per light is a sweep of the whole
  scene per light for the same list every time (64 shadowed omnis over 5,000 casters
  spend more than half the frame there). Only a light whose shadow renders this frame
  (`needsShadowRendering`) is culled; `drawLocalShadowFace` draws a list stamped for its
  frame and otherwise collects and culls for itself, so a list is never used stale (it
  holds raw pointers). The per-light test must stay the one the pass would make, on the
  same frustum: `tests/localShadowCasterTests.cpp` holds a spot's list against that
  reference in both lighting modes. The work is counted in `cullTime`.
- **Omni shadow bias is RELATIVE** — a fraction (0.2%, `omniShadowParams[2]`) of
  the receiver distance applied BEFORE the perspective projection. Cubemap shadow
  depth is crushed against 1.0, so a fixed post-projection offset erases omni
  shadows entirely at ordinary light ranges.
- **A StandardMaterial MAP setter writes through to the base slot, and its getter falls
  back to the base texture** — `setDiffuseMap`, `setNormalMap`, `setMetalnessMap`,
  `setEmissiveMap` and `setAoMap`. A glTF material binds the BASE slots, so a setter that
  only stored its own pointer would make `setDiffuseMap(nullptr)` on a loaded material
  clear nothing. Packing never writes back to the material either: StandardMaterial's UV
  transforms go straight into the block (`Material::packTextureTransform`), and both
  backends draw from the cached `Material::packedUniforms()`.
- **`StandardMaterial` overwrites the base-Material factors, ALWAYS.** Set surface
  properties with `setDiffuse` / `setOpacity` / `setMetalness` / `setGloss`
  (+ `setGlossInvert`) / `setBumpiness`; `setBaseColorFactor` / `setMetallicFactor`
  / `setRoughnessFactor` / `setNormalScale` on a StandardMaterial never reach the
  GPU. That holds with a base-colour texture bound too, so a GLB material — the parser
  binds its texture on the base Material — takes every later scalar edit. The parsers
  write both sets, and `tests/standardMaterialWorkflowTests.cpp`
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
  uploaded linear. There is no `setSpecularColor` / `setGlossiness` / `setUseSpecGloss`;
  a KHR spec-gloss asset uses `setSpecular` (gamma-encoded) and
  `setGloss`. There is no `(1 - max(specular))` diffuse scale on either backend.
- **Material flags bits 18 and 19 are `useSkybox` OFF and `hasOpacityMap`.** Bit 20
  is the only free bit. Bit 18 is stored inverted
  so a zero flags word keeps the scene environment, and it drops only the env atlas
  (SH probes and the flat ambient remain), as upstream's `useSceneEnv` does. The
  opacity map (slot / set-1 binding 34, multiplied into the forward and shadow alpha
  with the base-colour UV) is on both backends. It multiplies ON TOP of the
  base-colour map's alpha, so a material that sets ONE texture as both gets alpha
  squared, which thins every anti-aliased edge: set the opacity map only when it is a
  different texture, or leave the base colour without one (upstream's decal does).
  The spec-gloss map is Metal only; the clearcoat intensity/gloss/normal maps are on BOTH
  backends (Vulkan reads them as separate images through the shared material
  sampler, gated on flag bits 14/15/16 as Metal is). DEVIATIONS kept on purpose,
  marked at the code: `refractionIndex` and `iridescenceIOR` are IORs where upstream
  stores eta, and
  sheen is colour + roughness where upstream has `sheenGloss` + `useSheen`.
- **Ambient occlusion occludes the AMBIENT diffuse by default, the direct diffuse
  and a lightmap only under `occludeDirect`, and the specular through
  `occludeSpecular` mode and intensity.** That is upstream's split and both
  chunks follow it. `StandardMaterial::aoMap` and `Material::occlusionTexture`
  are one texture slot and one shader feature; the GLB parser fills the base
  property and `setAoMap` writes through to it, so clearing either one clears the AO.
- **`StandardMaterial::ambient` tints the AMBIENT diffuse and nothing else** (upstream
  `material_ambient`, which #9538 routes through `litArgs_ambient`): authored sRGB, packed
  linear into `MaterialUniforms::ambientTint`, multiplied right after the ambient is added
  and scaled by `(1 - specularity)` and before occlusion, on both backends. A lightmap
  replaces the tinted term, and a lightmap BAKE keeps its ambient untinted. White, the
  default, packs exactly 1, so a material that never sets it renders bit-identically.
  `lights`' ground uses it (upstream `Color.GRAY`).
- **SH light probes replace the ambient DIFFUSE only; the environment atlas
  still supplies the SPECULAR.** That is upstream's split (ambient and reflections
  are separate decisions), on both backends. Probes and atlas in one `if / else if`
  would make a scene carrying both lose every environment reflection the moment its
  probes are enabled. No example in the tree sets probes, so drive them:
  `VISUTWIN_AMBIENT_SH=r,g,b` in the examples
  harness puts a UNIFORM probe on any example's scene, whose diffuse is known
  exactly (a flat ambient of r,g,b), so the probes-on frame isolates everything
  else the probe path changes. Verify a change here with probes OFF bit-identical
  and the probes-on delta matching Metal's.
- **Material colours are authored in GAMMA space** and owe the shader a decode.
  The split is per-source, not per-material: `setDiffuse` stores raw, so the base
  colour FACTOR is decoded in the shader, while `setEmissive` is pre-linearised by
  `updateUniforms`, so the emissive factor is NOT. Every TEXTURE is authored in
  sRGB and is decoded, base colour and emissive alike. Getting one half wrong is
  invisible until a scene leans on it: a missing emissive-map decode shows only in a
  scene with large emissive surfaces, such as `depth-of-field`.
- **An example asset standing in for upstream's must match its PIXELS, not just
  its subject.** A stand-in with the wrong pixel values reads as a lighting bug that
  is identical on both backends. Before chasing a brightness gap that is identical on
  Metal and Vulkan, compare the example's textures against upstream's (`md5`, then
  mean pixel value) — an engine bug is rarely backend-identical. A substituted
  texture breaks parity through any channel the shader READS, not just albedo: gloss
  picks the environment-atlas MIP — `level = (1 - gloss) * 5` — so a gloss map that
  averages 0.51 where upstream's averages 0.75 reads a heavily prefiltered level and
  turns `refraction`'s capsules into a flat opaque wash. Settle it by setting a
  CONSTANT value on both sides. `textures/checkboard.png` and the whole seaside-rocks01
  set (`seaside-rocks01-gloss.jpg` included) are upstream's exact bytes.
- **On macOS a wait for the display or the GPU must keep the main run loop serviced**
  (`waitServicingRunLoop`, `platform/graphics/runLoopWait.h`). When the pointer is pressed
  over a window, macOS holds the SYSTEM pointer until the app's main thread services its run
  loop; a main thread blocked instead (the frame gate, `nextDrawable`, `vkWaitForFences`,
  `vkAcquireNextImageKHR`: most of a vsynced frame) is given up on only after ~200-250 ms,
  and the press arrives with the movement made meanwhile folded into one jump: every
  click-and-drag hangs, then lurches. Both backends' frame waits go through it (the wait runs
  on a serial queue while the main thread sits in `CFRunLoopRunInMode`). Proven with bare
  SDL probes: sleeping between frames reproduces it in a window with no renderer at all,
  idling in SDL's own wait does not, and a plain Cocoa window never shows it. A new blocking
  wait on the main thread owes the same treatment.
- **A loop waits for the next frame BEFORE it polls input** (`Engine::waitForNextFrame`):
  Metal takes the frame-gate slot and the drawable there, Vulkan the frame fence. Waited
  for inside `render()` instead (where the drawable used to be taken, at the first
  back-buffer pass), every frame shows input one display interval older than it need be:
  input sampled to frame complete measured 18 ms at 120 Hz, 7 ms with the wait moved, at an
  unchanged 120 fps. `ExampleApp` does it; an application with its own loop owes the call.
  It is optional and idempotent. The camera's own smoothing is separate: rotate, move and
  zoom damping default to 0.9 (DEVIATION from upstream's 0.98, which trails a drag by ~75 ms
  and settles in ~0.25 s; 0.9 settles in ~45 ms; the focus glide keeps 0.98). UI interaction
  is UNDAMPED while the pointer is down: a dragged element or scroll content follows the
  pointer exactly from its first movement, and the annotation tooltip shows and hides at
  once (`setTooltipFadeSeconds`, DEVIATION from upstream's 0.2 s fade). A scroll view's
  friction and bounce act only after release.
- **A Texture, RenderTarget, VertexBuffer or IndexBuffer may outlive its GraphicsDevice.**
  The device keeps a registry of each (`_liveTextures` and siblings, one mutex) and detaches
  them in `releaseGpuReferences` (both backends call it first thing in their destructors) and
  again in `~GraphicsDevice` (`detachResources`): targets first, then buffers, then textures
  (a target's attachments are textures). Each releases its GPU objects while the device can
  still free them (a target through `destroyFrameBuffers`, a buffer through the backend's
  `releaseGpuBuffer`; Vulkan defers the frees and drains them right after), gives back its
  VRAM share and forgets the device; after that upload, resize and read are no-ops and the
  CPU copy stays. Without it, a resource held by a global (a mesh or asset destroyed after
  `main` returns) wrote the freed device's VRAM counters, and a backend target asked the
  freed device for its native handles. A new subclass that owns GPU memory overrides the
  release hook, and its upload and read paths must check for the detached state.
  `tests/textureOutlivesDeviceTests.cpp` and `tests/resourcesOutliveDeviceTests.cpp` fail
  under ASan with the detach disabled. Shaders are not registered and need not be: Metal's
  objects are reference counted, and a Vulkan shader skips its frees once the device's alive
  token has expired. They are only COUNTED (the HUD's Shaders row), through a counter each
  `Shader` co-owns with its device, so the count survives the device too.
- **Metal frame pacing is display sync ON with THREE drawables.** Display sync
  is what gives an even dt (SDL's renderer, whose layer the device borrows, may
  have switched it off); the drawable count does not affect pacing once sync is
  on. Two drawables halve the frame rate in a fullscreen space, where the display
  holds a drawable through the next flip, and input lags with it, while windowed
  mode never shows it. Measure pacing as the
  frame-dt distribution (median, p5, p95, counts under 10 and over 25 ms), and
  measure it in fullscreen as well as windowed before touching either setting.
- **The scissor is clamped to the pass's attachments on BOTH backends** (Vulkan in
  `applyScissor`). Unclamped on Metal, a camera rect reaching past the target wraps a
  negative x to a huge `NS::UInteger` or leaves the attachment, which Metal forbids
  (upstream #9516 fixed the same on WebGPU).
- **TAA clamps and mixes in PREMULTIPLIED space and writes the CURRENT alpha**
  (upstream `taaResolve.js`), not the history alpha.
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
  invisible here.** Apple silicon selects the Apple backend; the SSE path is gated
  on `__SSE4_1__` (it uses `_mm_dp_ps` / `_mm_insert_ps`, so `__SSE__` alone could
  not compile) and x86 without SSE4.1 falls through to scalar. A wrong SSE
  horizontal sum (in `Vector2::dot`, say, or `Vector4::planeNormalize`) passes every
  test on an Apple build. When you touch one
  backend, check the same function in the other three, and add the contract to
  `tests/simdMathTests.cpp` — which only covers the backend the build selected,
  so an x86 CI build is what would actually guard the SSE path.
- **GCC and clang both fuse `a * b + c` into FMA by default**, and GCC does it even
  in strict C++ mode once `-mfma` is on — which Jolt's exported flags turn on for
  every x86 engine target (objdump shows it on Apple clang arm64 and GCC 14 x86). A
  fused scalar expression rounds differently from the same arithmetic in
  SIMD intrinsics, so a scalar path can silently stop matching its SIMD twin. The
  engine's FMA-fused scalar code is not a bug; comparing it bit for bit against
  anything is.
- **Quaternion blending lives in `Quaternion` — `dot`, `slerp`, `nlerp`, `operator+` —
  and nowhere else.** Each is written once per backend in `quaternion.inl` (SSE
  `_mm_dp_ps`, Apple `simd_dot`, NEON pairwise sum, scalar), so a caller that spells
  `ax*bx + ay*by + ...` out of `getX()..getW()` leaves the backend and can round
  differently from `lengthSquared()`, which is `dot(*this)`. No private scalar
  `slerpQuat` or `lerpVec3` belongs anywhere (`AnimEvaluator`, `AnimTrack` and
  `Skeleton` included). `Vector3::lerp` is the vector twin. Contracts for all of them are in
  `tests/simdMathTests.cpp`, which is per-backend by construction.
- **The same rule holds for component-wise vector work: use the core, not
  `getX()..getZ()`.** `Vector3` and `Vector4` carry `min`, `max`,
  `abs`, component-wise divide and multiply, `floor`, `clamp`, `minComponent` /
  `maxComponent`, `operator[]`, `lerp`, exact `==`, `perspectiveDivide` (a true
  division, so it rounds as `x / w` does) and `load` / `store` to raw floats;
  `Matrix4` has `load` / `store` (sixteen column-major floats), `normalMatrix`,
  `determinant3x3`, `translation(Vector3)` and a SIMD `mulAffine`. AABB accumulation
  and 16-element `getElement` copies into uniform blocks are where hand-spelled
  versions turn up. `min` / `max` answer
  EXACTLY `std::min` / `std::max` per lane on every backend, NaN included — `(b < a)
  ? b : a` — because the native instructions disagree (NEON's `vminq` propagates a
  NaN, Apple's `simd_min` drops it) and an AABB grown over a NaN position must not
  depend on the backend; each backend therefore selects on a comparison rather than
  calling its min instruction. `Matrix4` is trivially copyable, so a `memcpy` of one
  is well-defined; prefer `store`. `Vector2` is two plain floats and its operators
  are scalar on purpose.
  To check all four backends of `tests/simdMathTests.cpp` from a Mac: Apple and NEON
  (`-DUSE_SIMD_PREFER_NEON`) build natively, scalar builds if the standard headers
  are included first and `__APPLE__` / `__ARM_NEON` are then undefined, and SSE
  only COMPILES (`-arch x86_64 -msse4.1 -fsyntax-only`) — running it needs an x86
  machine or Rosetta, which is not installed here.
- **A SIMD kernel ships with its scalar reference and a bit-exact test.** Today:
  `scene/gsplat/gsplatSortKeys` (splat depth and sort key) and
  `framework/lightmapper/lightmapperBvh` (4-box slab test). Each exposes the scalar
  form publicly, its test compares the two exactly, the kernel file is built with
  `-ffp-contract=off` where a multiply-add could fuse (`engine/CMakeLists.txt`: only
  `gsplatSortKeys.cpp` — the slab test is `(box - origin) * inv`, a subtract then a
  multiply, which cannot contract into an FMA), and
  `VISUTWIN_KERNELS_FORCE_SCALAR` builds the scalar path for measurement. These
  kernels gate on `__SSE2__` and `__ARM_NEON && __aarch64__` DIRECTLY, not on
  `USE_SIMD_*`, so on Apple silicon the maths classes use the Apple backend while
  the kernels use NEON; `VISUTWIN_EXPECT_KERNEL_BACKEND` makes their tests fail on a
  silent fall-through, as `VISUTWIN_EXPECT_SIMD_BACKEND` does for the maths. Two
  traps: a zero direction component in the slab test gives
  `0 * inf = NaN`, which `std::min` / `std::max` IGNORE and NEON's `vminq` /
  `vmaxq` PROPAGATE, so the SIMD form selects on comparisons instead; and splat
  depths must be clamped to the bin range before any integer conversion, because
  the unclamped negative-to-`uint32_t` cast is undefined — x86 wraps a splat
  nearer than the nearest bound corner to the FARTHEST key.
- **A baked lightmap belongs to the MESH INSTANCE, not the material.**
  `MeshInstance::setLightMap` owns it (a `shared_ptr`); the renderer hands it to the
  device per draw (`GraphicsDevice::setInstanceLightMap`, cleared after the draw loop),
  both backends bind it OVER the material's slot (`applyInstanceLightMap`, slot
  `kLightMapTextureSlot`), and it switches the lightmap variant on by itself. Upstream
  0cd268478. Written into a SHARED material, meshes sharing it show whichever bake was
  applied last, and an unbaked mesh sharing it carries the bake — `lightmap-sources`
  shows all of it. Two more rules: a bake variant never samples a lightmap (otherwise
  a previous bake still attached is written back into the new one as its indirect
  light), and Metal
  rebinds the material textures when only the instance lightmap changed, since its
  "same material, skip binding" shortcut would otherwise keep the last mesh's bake.
  A lightmap is sampled through UV1, and the built-in box, cylinder, cone and capsule
  carry upstream's UV1 unwrap: every face or part in its own cell, padded by 8/64 of
  it (`PrimitiveGeometry::uvs1`; the plane and sphere use UV0, as upstream). Copying
  UV0 into UV1 would write all six faces of a baked box into one square and show
  their blend; `tests/primitiveGeometryTests.cpp` holds the cells disjoint.
- **The CPU lightmapper's BVH stores BOTH children of a node.** Children are built
  depth-first, so `left + 1` is the right sibling only when the left child is a
  leaf; a walk of `left` and `left + 1` skips most of the tree and answers rays that
  should hit as misses (no shadows, no AO). `LightmapperBvh` stores both children,
  and `tests/lightmapperBvhTests.cpp` checks any-hit against brute force over every
  triangle — the only oracle that cannot share a tree bug. `lightmap-bake` starts
  with the GPU bake; the CPU bake runs only when C is pressed, so a default
  screenshot proves nothing about the CPU path.
- **A glTF attribute is not always float, and refusing a quantised one drops the
  whole primitive in silence.** `TEXCOORD_n` and `COLOR_n` may be normalized
  byte/short in CORE glTF, and `KHR_mesh_quantization` extends that to `POSITION`,
  `NORMAL` and `TANGENT`. One `decodeComponent` in `glbParser.cpp` does the spec's
  de-quantisation for every reader, sparse overrides included, so a new reader
  should go through `AccessorReader` (built once per accessor, sparse substitution
  resolved up front, a base-less accessor reading as zeros plus its overrides) rather than
  casting to `const float*` or reading the base view alone. Never gate a
  primitive on `componentType != FLOAT`: a guard that `continue`s drops the mesh with
  nothing logged. Verify a change here by rendering the quantised asset
  against the same geometry written as floats — they must agree to rounding.
- **A glTF triangle primitive without NORMAL is UNWELDED into a flat-shaded triangle
  list** (`applyFlatNormals`): its vertex count grows and its index buffer goes. Any new
  per-vertex stream must be remapped through the source list it returns, and tangents are
  derived after it, from the final normals. DEVIATION: upstream keeps smooth normals and
  flat-shades through a material flag this port does not have.
- **glTF `COLOR_0` on a static triangle primitive uses the 72-byte coloured layout and a
  copy of its material with variant bit 21**, stored LINEAR as the file has it (alpha
  written 1: it tints diffuse, not opacity). **Vertex colours are linear unless the
  material says otherwise**: the vertex stages decode `pow(c, 2.2)` only under
  `StandardMaterial::setVertexColorGamma(true)` / `VT_FEATURE_VERTEX_COLOR_GAMMA` (variant
  bit 35 for a material that is not a StandardMaterial), off by default. A mesh authored
  with gamma-space colours must set it (`mesh-decals` does); one that does not is drawn
  brighter in the mid-tones. An all-white stream is dropped (it would only cost the batchable
  layout); a skinned or morphed primitive drops its colour with a warning, since no
  skinned or morphed vertex stage reads one.
- **The combine order is sheen, then clearcoat, then EMISSION, then the planar
  reflection, on both backends.** Emission added before the layers is attenuated by the
  coat's Fresnel and the sheen's albedo scaling. `setUseLighting(false)` means NO DIRECT
  LIGHTS (`VT_FEATURE_NO_LIGHTS`: ambient, reflections, fog and the combine still run); the
  `base + emissive` path is `setUnlit(true)` (DEVIATION), which UI, MSDF text, outlines,
  the view cube and glTF unlit materials take.
- **An animation layer's weight is a CONTRIBUTION, composed per node across layers;
  nothing but the component writes an animated node.** Each layer's `AnimEvaluator`
  has a pose sink (`setPoseSink`) that hands its per-node result to the
  `AnimComponent`, which blends the layers in order — OVERWRITE lerps toward the
  layer by its weight, ADDITIVE adds the layer's offset from the node's REST value
  (captured the first time a layer drives that property) scaled by it, a mask
  restricts a layer to listed node paths, and `setNormalizeWeights` divides by the
  total and drops the layers beneath the topmost OVERWRITE one, all as upstream's
  `AnimTargetValue`; a layer that wrote the nodes itself would make a 0.25 layer a
  full overwrite whenever it ran last. Two consequences
  for new code: an evaluator used OUTSIDE a component (no sink) still writes nodes
  directly, and a zero-weight layer keeps advancing its clocks (upstream does), it
  just contributes nothing. `tests/animLayerBlendTests.cpp` holds the closed-form
  cases.
- **Animation evaluation hashes no string per frame.** `AnimTrack::addCurve` gives each
  curve a `targetIndex` (one per distinct `nodeName`, first-seen order, `targets()`) and
  a `property` enum, replacing whatever the caller wrote in those two fields; a track
  evaluates into an array indexed by target. `AnimEvaluator` keeps a persistent SLOT per
  node path (the blend accumulates there, counters reset on the first contribution of
  each update) and maps each track's targets onto its slots once, keyed by
  `AnimTrack::serial()` (a copy gets a new one). The pose sink receives `(slot, path,
  value)`; `AnimComponent` maps a layer's slot to its target once, caches the binder's
  node and morph answers per target, and a layer's `drives()` per target against
  `AnimComponentLayer::maskVersion()`. Both caches are keyed on
  `AnimBinder::version()`: a binder whose answers can change (an `unresolve`) MUST bump
  it, or animation keeps writing the node it resolved first. Do not reintroduce
  path-keyed maps on the per-frame path. Only a value's flagged fields (`hasPosition`
  ...) carry an update's result;
  the unflagged ones are left over from earlier updates.
- **A glTF node's identity is its name or `node_<index>`, and an animation target
  is a PATH of those names.** `glbNodeName` in `glbParser.cpp` is the one spelling,
  used by the node payload the container instantiates, the animation channels and
  the skin's bone list; a new site that names a node must call it, or an unnamed
  node exists under one name and is animated under another. Channels carry
  `Root/Arm/Wheel`, upstream's `constructNodePath`, and `DefaultAnimBinder` walks
  the path from the bound entity (as its children, as itself, or anchored deeper)
  before falling back to the leaf name, so two "Wheel"s in different branches
  animate their own entity. Skipping a channel whose node has no name, or binding by
  bare `findByName`, silently loses the animation of an exporter that names only
  meshes and bones, and drives the first of two duplicate names twice.
  Same-named SIBLINGS are made unique in their parent's child order (`Wheel`, `Wheel1`,
  `Wheel2`; roots keep their names) by `glbNodeNames`, the one table the node payload, the
  channels and the skin bones read. `tests/glbAnimationBindingTests.cpp` builds the models in
  memory and holds all of it. Hand-authored tracks keep working with bare names.
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
  on `RenderTargetOptions`. Every built-in primitive writes upstream's `(u, 1 - v)`,
  putting v = 0 at the TOP: +Y on the box sides, sphere and cone bodies, -Z on the
  plane. A flipped plane hides well: a lying plane with a checkerboard or a rock
  texture does not look wrong either way, and only render-to-texture's tv shows it
  (its sky at the bottom).
  `tests/primitiveGeometryTests.cpp` pins the orientation; test anything new here
  with an ASYMMETRIC image, never a checker.
- **The picker renders an id buffer, as upstream, and unprojects through the pixel
  CENTRE.** `Picker::prepare` draws private clones of the candidates (hidden mesh
  instances, `visible() == false`, are not candidates; their material
  cloned with `Material::setPick`, `VT_FEATURE_PICK`: after the alpha test the fragment
  writes a 24-bit id into rgb) into an RGBA8 + depth target inside an offline scope, and
  reads both back with `Texture::read`; a selection is what is VISIBLE, and
  `getWorldPoint` unprojects the depth at the pixel centre (upstream 5cc6269d5), NDC z
  `depth * 2 - 1` on the GL-style projection. The pick layer's content changes under an
  unchanged frustum, so the picker calls `Renderer::invalidateCulledInstances` before
  drawing; the cull cache would otherwise hand back last prepare's clones. A device
  that cannot read back (a test stub) falls back to bounds: projected boxes, and a ray
  from the NEAR plane out against bounding spheres (a ray from the far plane back makes
  the nearest hit the FAR side). `tests/pickerTests.cpp` holds the fallback.
- **Primitive tangents are DERIVED from the UVs, never written by hand, and the
  bitangent `cross(n, t) * w` points toward DECREASING v** — the image's top row,
  where a normal map's green channel points. `calculateTangents`
  (`scene/geometry/geometryUtils.h`) is upstream's Lengyel accumulation with that
  handedness, which is a DEVIATION from upstream's own `calculateTangents` (it
  points toward +v) but matches what upstream actually shades primitives with: its
  primitive cache builds them WITHOUT tangents, and its derivative TBN negates the
  dP/dv axis. A hand-written tangent fails two ways: (1, 0, 0) on every box face is
  parallel to the normal on +/-X and renders those faces black, and a reversed
  tangent AND bitangent is a 180-degree turn of the normal map that reads as light
  from the wrong side, not as an error. The sphere, capsule and cone emit NO
  triangle that collapses to a line at a pole or tip, and each pole or tip vertex has its u
  centred on the one triangle that uses it (upstream #9597); their caps index from
  the vertices made so far, so a zero height or a zero radius does not index past them.
  Change a primitive's UVs and
  the frame follows; the test checks every corner against its triangle's UV
  gradient. `DEBUGPASS_WORLDNORMAL` on a normal-mapped box beside a plane wall shows
  a wrong frame in one frame: matching faces must match in colour. The frame is not
  only a normal-map concern: Metal's anisotropic IBL bends the reflection toward the
  bitangent, so its SIGN picks sky or ground: the `anisotropy` spheres on Metal move
  with it, while Vulkan (a roughness-only approximation) does not.
- **A glTF material property must be written to the STANDARDMATERIAL slot, not the
  base Material one.** `StandardMaterial::updateUniforms` pushes its own per-map
  tiling/offset/rotation into `Material`'s `TextureTransform` fields on every pack,
  so `setBaseColorTransform` from the parser is overwritten before it ever reaches
  the GPU — the same trap as `setDiffuse` versus `setBaseColorFactor`, one field
  further out. The parser writes `setDiffuseMapTiling` and its four siblings.
- **A glTF file loads through ONE pipeline, whatever the entry point.** `parse()` and
  `parseFromMemory()` load the model and call `createFromModel()`, which is
  `prepareFromModel()` + `createFromPrepared()` on the calling thread; `loadAsync` runs
  the same two halves with the first on a worker. Materials come from
  `createGltfMaterial`, textures from `createPreparedTexture`, vertices from
  `extractTrianglePrimitive`, point clouds from `appendPointVertices`. A per-entry-point
  copy drifts, and no example loads asynchronously, so a drifted async path goes
  unseen. `tests/glbMaterialPathsTests.cpp` and
  `tests/glbPointCloudTests.cpp` build models in memory and check both halves. A new
  glTF feature goes into the shared step, never into one entry point.
- **A glTF's images are decoded in `prepareFromModel`, all at once, one per thread; the
  tinygltf callback decodes nothing.** `GlbParser::loadImageData` keeps each image's
  encoded bytes and marks it `as_is` (KTX2 payloads are recognised by their magic);
  `prepareFromModel` decodes or transcodes every image in a `parallelFor`, straight to
  RGBA8 through `StbVerticalFlipScope`, which is per thread. Decoding inside the
  callback decodes a model's images one after another on the loading thread — three
  quarters of what a textured model takes to load. An image that is NOT `as_is` holds
  pixels someone else decoded (a model built in memory) and only gets widened to RGBA.
  The body of that loop may touch nothing shared: each image fills its own slot.
  `tests/glbImageDecodeTests.cpp` holds every channel count, the flip and 48 images
  staying in their own slots.
- **glTF cameras and `KHR_lights_punctual` lights are imported DISABLED**, as upstream's
  `createCamera` / `createLight` build them; an app enables the ones it wants
  (`findComponents<CameraComponent>()`). A camera sits on its node (glTF and this engine
  both look down -Z). A light sits on an extra CHILD entity named after the node and
  turned 90 degrees about X, because a glTF light shines down -Z and a light here down
  -Y; code that walks a glTF hierarchy by name will meet that extra node. The file's
  intensity is photometric, so it is stored twice: as `LightComponent::luminance`
  (times upstream's `getLightUnitConversion`), which a scene shines with under
  `Scene::setPhysicalUnits(true)`, and clamped to [0, 2] as the intensity every other
  scene uses. Physical units cover the LIGHTS only — there is no camera aperture,
  shutter or sensitivity, so set the matching `Scene::setExposure` yourself (the
  `glb-loader` example does). Anything that reads a light's strength for rendering goes
  through `LightComponent::renderIntensity(physicalUnits)`, not `intensity()`.
  `tests/glbCameraLightTests.cpp` checks both load paths.
- **The four remaining glTF extensions are FACTORS-ONLY where they touch materials.**
  `KHR_materials_sheen`, `_specular`, `_iridescence` and `_anisotropy` apply their
  factors (`applySheen` and siblings in `glbParser.cpp`, beside `applyClearcoat`) and
  IGNORE their textures with one warning per extension — there are no sheen,
  iridescence, specular or anisotropy maps (the Vulkan fragment stage is at MoltenVK's
  sampler limit). DEVIATION: an absent `sheenColorFactor` is the spec's black, where
  upstream substitutes white. `EXT_mesh_gpu_instancing` builds one 64-byte TRS matrix
  per instance in the node's space and shares the buffer across instantiations.
  `KHR_materials_variants` is upstream's container API (`getMaterialVariants`,
  `applyMaterialVariant(entity, name)`, `applyMaterialVariantInstances`); the swapped-in
  material is CO-OWNED (`MeshInstance::setMaterial(shared_ptr)`), an unmapped primitive
  keeps its material, and an empty name puts each primitive's OWN material back
  (DEVIATION: upstream assigns the engine default). `KHR_gaussian_splatting` primitives
  never reach the point-cloud path: their ACTIVATED values (linear scale, post-sigmoid
  opacity, xyzw rotation) go through `GSplatData::fromActivated`, which shares the PLY
  loader's packing and extent-carrying bounds, and the node gets a `GSplatComponent`
  (further splat sets on children named `<node>_gsplat_<n>`). `tests/glbExtensionsTests.cpp`
  checks all of it through both load paths.
- **An instance matrix places the instance in its NODE's space, as upstream
  (`matrix_model * instance`).** The renderer uploads the node's world matrix for every
  instanced draw — forward, GPU-culled and depth-only — and both vertex stages compose
  it; uploading identity would make instances WORLD transforms that ignore their entity,
  which no shipped example shows because every one keeps it at the origin. Two things
  follow the same rule: the instancing bounds are a
  LOCAL union (`_instancingLocalAabb` behind `_customAabb`) that `aabb()` carries through
  the node each time, and the GPU culler gets the frustum planes carried INTO the node's
  space (`M^T * plane`, which keeps world distances, with the sphere radius scaled by
  the node's largest axis), so its kernel is unchanged.
- **The metalness workflow's non-metal F0 is `f0(IOR) x specular colour x specularity
  factor`** (upstream `getSpecularModulate`), packed on the CPU into
  `MaterialUniforms::metalnessSpecular` and read by both surface chunks. It is computed
  in DOUBLE so the default IOR of 1.5 lands on exactly 0.04f and a frame without those
  inputs is bit-identical to a literal 0.04. The colour applies only under
  `setUseMetalnessSpecularColor(true)` (KHR_materials_specular sets it). A frame
  differs from a literal 0.04 wherever a metallic-rough material carries
  `KHR_materials_ior` or a black specular colour: `procedural-sky`'s sand
  (`specularColorFactor [0,0,0]` — no specular at all, as upstream), `refraction`
  (IOR 1.33) and `post-processing`'s amber (1.55).
- **Anisotropy has a DIRECTION** (`StandardMaterial::setAnisotropyRotation`, degrees,
  upstream `material_anisotropyRotation`): `T' = cos r * T + sin r * B` in both
  chunks, packed as `anisotropyParams`. The strength goes up as a magnitude; the
  deprecated negative strength is folded in as rotation + 90, with quarter turns
  written exactly, so a material that only ever used the sign picks the tangent or
  bitangent bit for bit.
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
  one backend can be asked. Two failure shapes this prevents, both silent: a LITERAL
  standing in for a limit (a clamp to 4096 is a quarter of what either backend
  actually allows) and the SAME limit spelled differently per backend (a hard-coded
  16x anisotropy on one, a queried one on the other). The float-renderable pair
  defaults to FALSE and the dimensions to 4096, so a backend that answers nothing
  degrades instead of
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
  `tests/ktx2TargetTests.cpp` drives each site with a device that can create exactly
  one format and checks the texture comes out in it; a hard-coded ASTC fails it.
- **Frame statistics are counted into `GraphicsDevice::frameCounters()`, and
  `Engine::render()` zeroes them as it starts.** The writers are whoever does the work
  — the forward pass (draws, material switches, sort and forward time), the culler
  (cameras, cull time), the three shadow passes and `drawDepthOnly` (shadow draws and
  time, skin and morph time wherever the palette is actually updated), the cluster pool
  — and every one of them already holds the device. Each backend's `draw()` calls
  `recordDraw(primitive, instances)`, which is what `stats.triangles` and
  `otherPrimitives` come from, and `setShader` counts a switch when the shader
  changes. A second reset site (in the fills, say) would let `start()`'s tick followed
  by the examples' manual update/render count two renders into the first frame. A new
  counter needs a writer
  AND a line in `tests/frameStatsTests.cpp`, which renders a known scene through the
  real engine on a stub device — nothing on screen reads these, so nothing else will
  notice a dead one.
- **A normal the OBJ or STL parser DERIVES follows the winding it emits.** With
  `flipWinding`, a normal computed from the file's winding is turned with the
  triangles; a file's own OBJ normals are left as the file says. Smoothing weights each
  face by its CORNER ANGLE, so a quad smooths the same whichever diagonal it was split
  on. And an OBJ corner's dedup key carries the generated normal: without it a flat
  (`s off`) cube with no normals welds to 8 vertices and lights two faces of every
  corner with the third's normal. Nothing in the examples loads OBJ or STL, so
  `tests/objStlRoundTripTests.cpp` is the only thing that sees these.
- **`pixelFormatInfo` is a map the `PixelFormat` enum does not enforce.** An
  enumerator with no entry makes `pixelFormatBytesPerPixel()` return 0, which
  the Vulkan upload path uses to size its staging copy.
  `tests/pixelFormatTests.cpp` lists every enumerator by hand — add a format
  there when you add one to the enum.
- **The forward sort key packs its fields; it must never XOR them.** The layout is
  in `scene/renderer/sortKey.h` — draw bucket, alpha test, material ID, mesh — each
  owning its own bits, and `tests/sortKeyTests.cpp` holds it. XORing overlapping
  ranges makes two materials that differ in one of the colliding fields hash equal and
  interleave, and a caller that shifts the key and discards a half can drop the
  shader variant key — the most expensive state change in a frame — from the order.
  Material
  IDENTITY is what is sorted on, as upstream, because consecutive draws of one
  material skip binding entirely — state similarity cannot deliver that.
  The mesh field is `Mesh::id()`, a creation-order counter (upstream `mesh.id`), never
  the mesh's ADDRESS: that field orders the draws of one material, the last of two
  coplanar surfaces wins the depth test, and an order that follows the heap renders the
  same scene with different pixels from run to run (`orbit`'s statue: up to 190 pixels).
  Anything else that orders draws must likewise use an id or the collection order, not
  a pointer.
- **A draw BUCKET (default 127) is the primary key of BACK2FRONT (higher first) and
  FRONT2BACK (lower first)**, ahead of depth (`distanceSortsBefore` in `sortKey.h`).
- **Blending is NOT a shadow-caster rule.** A blended mesh casts solid depth unless it
  alpha-tests or dithers its shadow; turn `castShadow` off for one that should not (UI
  element visuals do).
- **A layer carries a sort mode per sublayer** (`Layer::opaqueSortMode` /
  `transparentSortMode`, upstream's SORTMODE_*), defaulting to MATERIALMESH and
  BACK2FRONT. The two pull in opposite
  directions on purpose: opaque wants the fewest state changes, transparent has to
  composite back to front. SORTMODE_CUSTOM with a null callback leaves the order
  ALONE rather than falling back to a mode nobody asked for.
- **A layer's depth clear happens INSIDE the forward pass when the layer is not the
  pass's first action.** Render actions of one camera and target share a
  `RenderPassForward`, whose load action clears only for its first action; a later layer
  with `clearDepthBuffer` (the gizmo layer, `layers`' front layer) is cleared mid-pass by
  `clearDepthInPass` (a depth-1 triangle under ALWAYS, colour writes off, current
  viewport), as upstream's in-pass `renderer.clear`. Without it such a layer is silently
  depth-tested against the scene — the transform gizmos' centre handles vanish inside the
  box. Only the LAYER's depth clear is done mid-pass (not colour, stencil or a second
  camera's clears).
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
  halves, and it is the same condition `onEnable` / `onDisable` fire on. Gathered on
  `enabled()` alone, a light on a disabled entity goes on lighting the scene and
  casting its shadow while the mesh instances on that same entity vanish.
  `LightComponent` also
  syncs `active()` into its backing `Light`, because `shadowRenderer`,
  `shadowRendererLocal` and the cookie pass gate on `Light::enabled()` rather than
  on the component. Two sweeps deliberately take EVERY instance and say so in a
  comment: the lightmapper's layer backup, which must restore a light it widened
  even if that light is switched off mid-bake, and the camera's render-data purge,
  where a disabled light is exactly the one holding a stale pointer.

  CAMERAS too: `LayerComposition` builds its render actions from the camera's active
  state and fingerprints it, since built and fingerprinted from `enabled()` a camera on
  a disabled entity keeps rendering and switching the entity does not even trigger a
  rebuild. The fingerprint also carries the camera's clear flags, because
  `setupClears` COPIES them into the actions and a runtime change would otherwise be
  ignored. `tests/componentActiveTests.cpp` holds both.

  SCRIPTS likewise: every phase — initialize,
  postInitialize, fixedUpdate, update, postUpdate — gates on `active()`, and
  `Script::enabled()` folds in its component's active state, so a script on a
  disabled entity stops running rather than merely stopping being drawn.
  `ScriptComponent::onEnable` is what initializes a script created while the
  component was inactive; in a `setEnabled` override, which sees only the
  component's own flag, a script created on an entity that is enabled LATER would
  never initialize at all.

  PARTICLE SYSTEMS too: testing the component's and its own entity's `enabled()`
  keeps an emitter under a disabled PARENT simulating.
- **A particle's clock is upstream's, and `rate` is the seconds between births, so 0 is a
  BURST.** Particle i starts at life `-i * rate`; a life <= 0 is unborn and re-spawned every
  step; reaching the lifetime wraps the life back by `max(lifetime, numParticles * rate)`,
  showing the particle again when the emitter loops and HIDING it (flag in `rotSeedSize.w`)
  when it does not; `stop()` clears the loop and hides the unborn, and `play()` restores it,
  bringing hidden particles back at their next wrap. `rate` 0 does not mean "auto,
  lifetime / numParticles", and `stop()` neither freezes nor clears the pool.
  `scaleGraph` is a half-extent, as upstream's +/-1 quad times scale; read as a full
  extent, every quad is HALF upstream's size. A
  screen-space emitter (`screenSpace`, a child of a screen-space element) takes the node's
  world transform as CLIP space with no view or projection, sizes in viewport heights with
  the quad's x scaled by height / width (upstream #9570), and a screen puts it in the UI draw
  order beside its elements (`ScreenComponent::processDrawOrderSync`).
  A particle's colour is LINEAR and owes the target upstream's output stage (particle_end):
  the colour map AND the colour graph are decoded from gamma space (the graph's rgb on the
  CPU when its LUT is built, clamped to [0, 1]; alpha is never decoded), multiplied, then
  tone-mapped with the scene's exposure and gamma-encoded, or left linear on a camera
  frame's HDR scene (`ParticleEmitter::setOutput`, filled per draw by the renderer like the
  splats' tail). A ramp left undecoded draws every mid-tone of a colour graph BRIGHTER than
  upstream's (a 0.5 grey at about 0.73). The kernel's randomness is an INTEGER hash (PCG of the particle
  index and a step counter), identical in MSL and GLSL; `fract(sin(x) * 43758)` is not
  uniform on the GPU (a spark fountain 10% narrower than upstream) and not the same on
  the two backends. An unset graph is a CurveSet whose curves have NO KEYS — a default
  `CurveSet` still holds one empty curve, and treating that as a zero graph2 halves
  every velocity graph on average. The option defaults are upstream's: scale 1, opaque,
  white, BLEND_NORMAL, rate 1.
- **A script fires upstream's lifecycle events on ITSELF: `enable` / `disable` and `state`
  (bool) when `enabled()` changes, and `destroy` once.** `enabled()` is the script's own flag
  (`setEnabled`) AND its component's active state, so a component or entity switched off fires
  them too. `ScriptComponent` reports the state it is told (`Script::syncState`) rather than
  re-reading it, because `Entity::destroy` disables a component while its entity still reads
  as enabled: a destroyed entity's scripts fire `disable`, then `destroy`. A script created
  while inactive fires `enable` when it becomes active, BEFORE it initializes (upstream's
  order). `destroy` fires from `~ScriptComponent`: the entity is alive, its script component
  is not, so a handler must not reach for `entity()->script()`. And the engine fires
  `prerender` at the top of `Engine::render`, before the UI elements sync: the last point to
  move what this frame draws, after every update. `AnnotationManager` is built on all of
  them; `tests/annotationTests.cpp` holds the events.
- **A script may create a sibling or destroy its own entity from inside its own
  method, and the component's loops are built for it.** `ScriptComponent::forEachScript`
  walks by INDEX, so a script created mid-pass (appended, possibly reallocating the
  vector) runs in the same pass; and a `RunState` outlives the component, so when
  a script's `entity()->destroy()` frees the component mid-loop, the destructor hands the
  scripts to `RunState::retired` instead of freeing the one still executing and lets go
  of the state WITHOUT freeing it, and the outermost loop, which checks `alive` before
  touching the component again, frees it on the way out. The state is owned by hand, not
  by a `shared_ptr` copied per loop: that is two atomic operations per component per
  phase for a case that almost never happens. A loop that range-iterates
  `_scripts` makes both cases undefined behaviour that usually still works.
  `tests/scriptLifetimeTests.cpp` holds both, and under the `sanitize` preset a
  range-iterating loop aborts with a heap use-after-free.
- **A script is visited only in the phases its TYPE overrides.** `Script::phasesOf<T>()`
  decides at compile time which of `update`, `postUpdate` and `fixedUpdate` T (or a base
  between it and `Script`) overrides — `&T::update` names `Script`'s own member exactly
  when nothing did; a member that cannot be named (overloaded, inaccessible) counts as
  overridden, which is always safe. `Script::make<T>()` stamps the result on the instance,
  and `REGISTER_SCRIPT` and `ScriptRegistry::registerType<T>()` build through it. A
  HAND-WRITTEN factory (`registerType(name, [] { return std::make_unique<T>(); })`) gets
  `PHASE_ALL` and is visited in every phase: correct, only slower, so write
  `Script::make<T>()` there too. The script system keeps one execution-ordered component
  list PER PHASE; a component joins a list when it gains its first script implementing
  that phase (`componentGainedPhases`, safe mid-loop) and leaves them all when destroyed,
  so a component none of whose scripts implements a phase is not even read in it. With
  every script visited in all three phases, 50,000 scripts that override nothing cost
  4 ms of update a frame; now nothing. `SortedLoopArray::remove` finds its item by
  bisection on the key. `tests/scriptPhaseTests.cpp` holds the trait, the lists, order,
  disabled scripts and a phase gained mid-loop.
- **An entity built from a container outlives the Asset's `unload()`.** A mesh instance
  co-owns its mesh and material, and every material a `GlbContainerResource` hands out
  keeps the container's texture list alive (`Material::retainResource`), because a
  material holds its textures as RAW pointers. All four parsers (glb, obj, stl, assimp)
  build that container. Without it, unloading the asset would free the textures under a
  live entity's materials. Still borrowed: a `Texture*` from a TEXTURE asset set on a
  material by hand — that asset must outlive the material.
- **Component lifecycle runs in `Component::order()`, not container order.**
  Lowest first on enable, reverse on disable, creation order as the tiebreak;
  `RigidBodyComponent` returns -1 so its body exists before anything can move or
  query it. `onPostStateChange()` then runs over every component, which is where
  one wires itself to a sibling that had to exist first. Never iterate an
  `unordered_map` here: the order would vary per run.
- **Component systems emit `add` / `beforeremove` / `remove`, and
  `removeComponent(Entity*)` is the way to take a component away.** A system that
  needs its own bookkeeping on destruction must not declare an overload named
  `removeComponent` — that hides the virtual (`ScriptComponentSystem` calls its
  one `unregisterComponent` for exactly this reason).
- **`ComponentSystemRegistry::add` REJECTS a duplicate id, or a second system for
  the same component type, and keeps the first.** Overwriting the lookup maps would
  leave the original alive, owned and still subscribed behind an id that no longer
  resolves to it. `remove` erases from the owning vector and both
  maps together — partial erasure is the bug this pairing exists to prevent.
- **Component lists are PER ENGINE** (`Engine::components()`, a `ComponentRegistry`:
  `components().instances<RenderComponent>()`; `instancesOf<T>(registry)` when the registry
  may be null). Two engines in one process never see each other's components. Code reaches
  the registry through what it already holds: a system through `componentRegistry()`, a
  component through `registry()`, the renderer and the composition through the scene
  (`Scene::componentRegistry()`, set by the engine), the shadow passes through
  `ShadowRenderer::componentRegistry()`, which `collectShadowCasters` and
  `JointComponent::bodyWillBeDestroyed` take explicitly. A component joins its engine's
  registry from its constructor (`listInstance(this)`, resolved from its system's engine or
  its entity's hierarchy); one built in no engine's hierarchy — every glTF container entity
  — joins when its entity is inserted under an engine's root
  (`Entity::onInsertedIntoParent`), and moves with it to another engine. A component that
  outlives its engine is let go by the registry's destructor. A new component type calls
  `listInstance(this)` / `unlistInstance()` and includes `componentRegistry.h` in its .cpp;
  a test with no engine gives its components and composition a `ComponentRegistry` of its
  own (`joinRegistry`, `LayerComposition::setComponentRegistry`), or they are in no list
  at all. The batch mesh instances live there too
  (`ComponentRegistry::batchMeshInstances`), filled by the engine's `BatchManager`.
- **A component type's list keeps CREATION order, and a destroyed
  component leaves a NULL in it until the list is next read**
  (`framework/components/componentInstanceList.h`). The order is a process-wide creation
  serial, so a component listed LATE (a container inserted after newer components were
  made) is inserted at its creation position rather than appended;
  removal finds the slot by binary search on that serial and nulls it, and
  `items()` closes the holes in one ordered pass. A `std::erase` on a vector — a scan
  and a shift per removal — would make destroying K of N components cost K x N. Two
  rules follow. Every loop over a component list checks each entry for null — the list
  never hands out a hole, but a component destroyed during the loop becomes one under
  it (where an erase would shift the survivors and the loop skip one). And a destructor
  or teardown hook that
  walks its own type's list uses `forEachLive`, which does not compact, because it may
  run inside someone else's loop over `items()`. A new component type registers the
  same way; `tests/componentInstanceListTests.cpp` holds order, holes, a destroy
  mid-walk, late listing, two engines and a component outliving its engine. `ElementComponent`'s destructor sweeps the elements for `_maskedBy` only
  if it was ever handed out as a mask.
- **A node's `children()` follows the same contract.** Each child knows its slot
  (`_slotInParent`); `removeChild` (and deleting an attached node) leaves a NULL in the
  parent's `_children`, popping it outright when it is the last, and `children()` closes
  the holes in order before returning, so a caller never sees one — except a loop that
  removes a sibling from inside its own body, which then sees a null under it (never a
  shift). Such a loop checks for null and does not call `children()` on that node again
  inside the loop; every loop INSIDE `GraphNode` that can run callbacks (`fireOnHierarchy`,
  `notifyHierarchyStateChanged`) walks by index and skips holes. A `removeChild` that
  finds and erases is quadratic over many siblings. `tests/graphNodeTests.cpp` holds
  order, re-removal, deletion
  while attached and a removal mid-walk. `removeChild` DISABLES the detached subtree
  (DEVIATION: this engine renders from global component lists filtered by `active()`, so
  a detached entity left active would go on rendering), but `addChild` MOVING a node from
  one parent to another notifies only a real change of its enabled-in-hierarchy state:
  reparenting between two enabled parents fires no disable / enable.
- **`EventHandler::off(name, callback)` matches by function IDENTITY**: a function pointer,
  or the same captureless lambda object. A capturing lambda or a `std::function` cannot be
  compared and is a compile error there — keep the handle `on()` returned, or use a scope.
- **`Entity::destroy()` is the teardown path, and it does NOT free the node.**
  Descendants first, disable in order, `destroy` event, then each component
  released THROUGH the system that owns it (so `beforeremove` / `remove` fire for
  a destroyed entity exactly as for an explicit removal), in reverse creation
  order. It is idempotent and the destructor calls it. Ownership stays with the
  parent's `unique_ptr`: freeing inside `destroy()` would leave `this` dangling
  for the rest of the call. Note the `_destroying` guard in
  `removeComponentInstance` — teardown has already disabled everything in order,
  and without it each component would get a second `onDisable`.
- **Every render component of one primitive type shares ONE mesh per device**
  (`sharedPrimitiveMesh` in `renderComponent.cpp`, upstream's `getShapePrimitive`), and
  `setMaterial` swaps the instance's material IN PLACE, as upstream, instead of
  rebuilding the primitive. So nothing may modify a mesh reached through a primitive
  component — it is every such component's mesh; per-instance state (material,
  lightmap, mask, stencil, shadow flags) lives on the MeshInstance and survives a
  material change. A batched source's material change tears its group down
  (`sourcesLeaving`). DEVIATION: the cache holds meshes WEAKLY —
  the components co-own them (`_ownedMeshes`) and the last one to go frees the mesh —
  because the device fires no "destroy" a strong cache could clear on, and a mesh freed
  at static destruction would release its buffers into a device already gone. Boxes
  of one material tie in the sort key's mesh field and keep collection order.
  `tests/primitiveMeshSharingTests.cpp` counts the buffers.
- **`Entity::clone` is TWO passes, and a new component owes both.** The first builds
  the copy — node state and tags, then each component in CREATION order through
  `Component::cloneFrom` — and the second, once the whole subtree exists, calls
  `Component::resolveClonedReferences(source, map)`, which points any reference into
  the source subtree at its copy (`Component::remapCloned`) and leaves one outside it
  alone: joint ends, a button's image, skin bones and root, the legacy animation's
  model, and a script's own references through `Script::resolveClonedReferences`
  (upstream `resolveDuplicatedEntityReferenceProperties`). A component with settings
  overrides `cloneFrom`; one holding an Entity or node pointer overrides the second
  too. Scripts are recreated by NAME and copy nothing unless `Script::cloneFrom` does
  — the stand-in for upstream's attribute copy. A render clone takes
  `MeshInstance::cloneFor` (shared mesh and material ownership, its own morph and skin
  instance, no lightmap or instancing) and CO-OWNS a primitive mesh, and skips
  instances a splat, emitter or wide line attached; those owners rebuild their own.
  A component without its own `cloneFrom` comes back default-constructed.
  `tests/entityCloneTests.cpp` fails when a component comes back default-constructed,
  a reference is not remapped, or a cloned box borrows its mesh from the source.
- **`Tags::add` / `remove` forward through a const reference.** With a string literal
  the variadic template would otherwise re-deduce itself for the vector it builds and
  recurse until the stack runs out.
- **`Engine::start()` must be called AFTER the scene exists.** It fires the
  initialize phase (`start`, then systems `initialize` / `postInitialize`, then
  the app's `initialize` / `postinitialize`) and then ticks. `ExampleApp` starts
  the engine in `run()` once `create()` has returned, for exactly this reason —
  starting it in `initEngine()` initializes an empty world and renders an empty
  first frame. Every script initializes before any script post-initializes.
- **The per-draw uniform rings GROW, and the growth is why an overflow is only
  ever one bad frame.** Both backends size a frame region for a draw count, count
  what the frame actually ASKED for (including what did not fit), and reallocate at
  the next frame boundary. Growth cannot happen mid-frame: every offset already
  handed out is interpreted against the one buffer bound at the start of the render
  pass (Metal) or named by the persistent dynamic-UBO descriptor sets (Vulkan). So
  it happens behind a full drain — Metal waits out the other two in-flight frames
  through the device's `MetalFrameGate` (one semaphore paces every ring), Vulkan calls `vkDeviceWaitIdle` and then REWRITES the two
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
- **A matrix palette is uploaded once a FRAME, named by a version, and Metal's palette
  ring grows like the uniform rings.** `SkinInstance` and `SkinBatchInstance` take a new
  `paletteVersion()` (`GraphicsDevice::nextPaletteVersion()`, process-wide, never 0)
  each time they rewrite their palette, and pass it to `setDynamicBatchPalette`; Metal's
  ring (`MetalPaletteRingBuffer`, its bookkeeping in `PaletteFrameAllocator`) hands
  every draw carrying the same version the frame's one copy, so a skin drawn by the
  forward pass and three shadow passes costs one palette, not four. A palette writer
  that changes the bytes WITHOUT taking a new version makes later draws of the frame
  reuse the old copy; version 0 is never shared. A frame that asks for more than the
  region holds (256 KB to begin with) is counted in full and the ring reallocates at the
  next frame boundary behind a drain, with one message; in that one frame a draw that
  did not fit keeps the palette bound before it. Vulkan copies a palette per draw into
  its uniform ring, which already grows. `tests/paletteSharingTests.cpp` holds the
  sharing, the overflow count and the versions.
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
  The CLUSTER light and cell buffers are the other one: every grid a frame binds
  takes its own pair from a per-frame slot (`MetalGraphicsDevice::setClusterBuffers`),
  a grid bound again reuses its pair, and the pair is bound on the open encoder at
  once — `startRenderPass` binds only what was set before the pass, and a layer
  binds its grid after its pass began. One shared buffer rewritten per layer lit a
  pass whose grid was the first ever written with NO buffer at all (a whole mesh of
  a lightmap bake missed its omni light on Metal alone), and lit every layer of a
  frame with the last grid written.
- **Mesh instances are culled ONCE per (camera, layer) per frame, into a cache both
  sublayers read.** `ForwardRenderer::buildFrameGraph` registers the pairs it will
  render (`Renderer::requestMeshInstanceCull`) and culls them in one batch
  (`executeMeshInstanceCull`), which is where the `precull` and `postcull` events
  fire — once per camera, upstream's contract, which a lazy per-layer cull cannot
  give. `renderForwardLayer` then reads its own bucket; it does not sweep the scene
  or run the frustum test itself.

  **All of a camera's layers are culled in ONE sweep of the scene**
  (`Renderer::cullMeshInstances`): each component's layers are matched against the
  requested ones as a bitmask, and an instance is tested once and pushed into every
  requested layer's bucket it belongs to. Each bucket's ORDER is components in
  creation order, then the layer's own instances; keep it, since equal sort keys keep
  it too. The cache keeps each
  pair's vectors across frames and drops a pair not culled the frame before.
  `dispatchGpuInstanceCulling` returns at once when no mesh instance has GPU culling
  on (`MeshInstance::gpuCulledInstanceCount`) instead of sweeping the scene to find none.

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
  that comparison catches what no screenshot does, such as the aspect-ratio hole
  above.
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
  And `ShadowRenderer::needsShadowRendering` is PURE: were it to consume a
  `SHADOWUPDATE_THISFRAME` request, a culled light would answer "no" and consume the
  request in the same breath, losing the shadow the caller asked for.
  `Renderer::consumeOneShotShadows`
  does that after the frame graph is built.

  The eight-slot main light array is ranked by `Camera::screenSize`, not by component
  order, so the slots go to the lights covering most of the picture. `tests/lightCullingTests.cpp`
  pins the bounds geometry and the cull decision, because NO example in the tree has a
  light off screen — every one of them keeps its lights in view, so a culled light is
  never exercised by a render at all.
- **A morphed mesh's bounds grow by how far its targets REACH.** `Morph::aabb` is the
  union of every target's position-DELTA bounds and the origin (upstream `Morph.aabb`),
  and `MeshInstance::aabb` adds its min to the rest-pose min and its max to the max
  (upstream `_expand`); a skinned, morphed glTF's bone boxes take each vertex's reach
  under all its targets at once, negative deltas summed toward the min and positive
  toward the max (upstream `_initBoneAabbs`, in the GLB parser here). Without either,
  culling, light culling and the shadow fit read rest-pose bounds and a mesh morphed
  outward can be culled on screen. Like upstream this is the one-target-at-a-time case,
  not every
  target stacked at full weight. No render shows it unless a morph carries a mesh across
  a frustum edge (`mesh-morph` is bit-identical); `tests/morphBoundsTests.cpp` holds the
  numbers.
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
- **A splat's footprint takes its focal length PER AXIS and keeps the SIGNS**
  (`viewport.xy * (P[0][0], P[1][1])`, upstream #9486/#9490, both backends). One focal
  from the width for both axes squashes every splat whenever the viewport's pixel
  aspect differs from the projection's (a manual camera aspect, a side-by-side stereo
  target); with square pixels the two agree. Under an ORTHOGRAPHIC camera the spherical
  harmonics are
  evaluated along the camera forward, not from the camera position to the splat
  (`GpuGSplatParams::cameraOrtho`, upstream #9531): ortho rays all run parallel, and a
  direction from the camera position changes a splat's colour as the camera pans while
  the image stays put. Under ortho the footprint's Jacobian is also evaluated at view
  position (0, 0, 1), and splat fog takes the view depth `-view.z`, not `clip.w` (1 for
  every splat under ortho).
  No shipped asset has SH bands, so no render shows it.
- **A splat's clip z is CLAMPED to the depth range, and that only works because
  its screen-space kernel is clamped too.** A gaussian splat is a quad built around
  ONE projected centre, so the whole quad carries that centre's depth: an unclamped
  centre crossing the near plane clips the entire splat away while its footprint
  still covers visible pixels, and the surface nearest the camera pops out whole as
  you walk into a cloud. Upstream's `gsplatCenter.js` clamps, and both backends do
  (to `[0, clip.w]`, after the GL-to-[0,1] remap). The catch is that the same
  near-plane splats are the ones whose perspective Jacobian — it divides by view.z —
  blows their footprint up without bound, and the z clip silently hides that: clamp z
  without `gsplatCorner.js`'s `vmin = min(1024, viewport)` kernel clamp and the
  frustum x/y cull, and ONE splat covers the screen. Both backends have all three. Do
  not port one of these without the others.
  The default gsplat example pose cannot see any of it — nothing there straddles a
  plane, and the frame must come back bit-identical, which is the control that says
  a change here is confined to the splats that actually cross.
- **A splat's colour is GAMMA space, and it owes the target an output stage.**
  Upstream's `gsplatOutput` (`prepareOutputFromGamma`): decode when tone mapping, fog or
  a linear target needs linear, fog at the view depth, tone map with exposure, and
  encode back for a gamma target. The renderer fills the tail of `GpuGSplatParams`
  (fog, exposure, tone-mapping mode, linear-HDR target from `GraphicsDevice::hdrPass()`)
  per draw, and both splat vertex shaders apply it (`gsplatPrepareOutput`). Decoding to
  linear and stopping is right only under a camera frame: on a gamma target the splats
  would be written linear, untonemapped and unfogged beside tonemapped meshes. The
  tone-mapping curves are the forward
  pass's own, not a copy: Metal splices the `common-tonemap` chunk into the splat source
  at creation, and `gsplat.vert` includes `chunks/common-tonemap.glsl` under
  `VT_TONEMAP_OPERATORS_ONLY` and calls `toneMapByMode`. Growing `GpuGSplatParams` means
  the bundle validator's expected size and both backends' staging arrays too.
- **`RenderAction::firstCameraUse` / `lastCameraUse` describe the camera's WHOLE FRAME and
  nothing rewrites them.** The composition sets them; `prerender` / `postrender` fire from
  them and the directional-shadow block split reads them, as upstream. The camera frame's
  clones get them re-marked across its scene, transparent and after passes
  (`RenderPassCameraFrame::updateCameraUseFlags`, upstream's), and
  `RenderPassForward::validateRenderActionOrder` works out block-local spans itself.
  Rewriting them per block (in `addMainRenderPass` or a forward pass) makes a camera
  frame fire both events twice a frame and feeds a grab-pass split the rewritten flag
  the next frame.
- **GPU instance culling is ONE implementation, `ComputeInstanceCuller`, over `Compute`**
  (kernels in `instanceCullShaders.h`); a backend gets it by supporting compute and
  indirect draws. `tests/gpuInstanceCullTests.cpp` (label `gpu`) culls on the real device
  and compares the read-back arguments and instances with a CPU cull; no example
  renders with GPU culling on.
- **GPU instance culling has one output PER CAMERA and runs once a frame, before
  anything draws.** `dispatchGpuInstanceCulling` culls every GPU-culled mesh instance once
  for each camera in the frame's render actions, into that camera's own output
  (`MeshInstance::gpuCullOutputFor`), and a draw binds the output culled for ITS camera in
  this frame (`culledOutput(camera, renderVersion)`); a camera with none — a picker, a
  bake, an appended pass — draws the whole instance buffer, as do the shadow passes. One
  output shared by the cameras made every view draw the last camera's set. An output not
  culled for 120 frames is dropped. `tests/gpuInstanceCullTests.cpp` (label `gpu`) culls
  for two cameras through the renderer and reads each output back.
- **An `ASPECT_AUTO` camera's aspect is resolved BEFORE culling**
  (`Renderer::resolveAutoAspectRatio` at the top of the graph build); the draw-time
  assignment from the actual target stays and the cull cache's frustum compare still
  catches a disagreement. It changes FIRST-FRAME work only, which a one-shot shadow keeps:
  resolved later, `ambient-occlusion`'s directional shadow would be fitted on frame one
  with the default 16:9 aspect instead of the window's.
- **A raw `Entity*` an object keeps must follow the entity's `destroy` event**: subscribe
  when set, clear the pointer in the handler, and `off()` the handle in the owner's
  destructor (a handler that outlives its owner is a use-after-free the sanitizer build
  catches). `DestroyWatch` (`framework/destroyWatch.h`) is that handle: `watch()` on set,
  and it unsubscribes on the next `watch()`, `reset()` and in its destructor. Joint ends, the transform gizmo's target, a button's image entity and the
  outline renderer's records do; `tests/triageContractsTests.cpp` holds the button.
- **`ResourceLoader::shutdown` delivers every undelivered completion as an ERROR, and a
  `load()` after it fails at once** — otherwise an `Asset` stays `_loading` forever and a
  later `loadAsync` coalesces onto a load that will never finish.
- **The lighting block is set once per LAYER, not per draw.** `renderForwardLayer`
  calls `setLightingUniforms` on a layer's first draw and again only when a draw's
  light mask or `receiveShadow` differs from the previous draw's — the only two
  inputs to that block that depend on the draw (the mask also picks which clustered
  lights the draw accepts). Anything genuinely per draw must go
  in the model or material block, never in the lighting block, or every draw after
  the first reads the first one's value.
- **Vulkan reuses per-draw uploads WITHIN A FRAME, keyed on what they came from.** Every
  draw of a material whose pack is unchanged shares one ring slot
  (`_materialUniformSlots`, keyed on the material and `Material::uniformsVersion()` — a
  process-wide counter bumped by `markUniformsDirty`, so a freed material's reused address
  can never match); material-less draws (every opaque shadow and prepass caster) share
  the constant default block; draws with the same cluster buffers share one set 5, and the
  zero cluster sentinels are allocated once a frame. `_frameSerial`, bumped in
  `onFrameStart` where the ring and the pools are rewound, invalidates all of it. Quad and
  custom uniform blocks are never reused — nothing versions them. So a MUTATOR THAT SKIPS
  `markUniformsDirty()` leaves Vulkan drawing the old block for the rest of the frame
  as well as the cache (Debug builds' re-pack comparison reports it).
- **A per-draw deduplication must not HASH the block it deduplicates.** Hashing the
  whole ~2.8 KB lighting block with FNV-1a on every draw is a serial 700-step multiply
  chain, about a microsecond a draw; a `memcmp` against a copy of the last upload is
  vectorised and exact (a hash can collide and reuse the wrong block) but still a large
  share of a 10-20k-draw frame, much of it in shadow passes whose draws never read
  lighting. So on Metal a draw compares a VERSION: every writer of
  `_lightingUniforms` calls `markLightingChanged()`, the three setters `draw()` itself
  calls per draw go through `writeLightingIfChanged` (an unconditional bump there would
  make every draw a lighting change), and the block is compared exactly only when the
  version has moved. A NEW WRITER THAT SKIPS THE BUMP leaves draws on the previous
  lighting block; Debug builds assert on it in `submitPerDrawUniforms`.
  Vulkan's image descriptor sets likewise remember the last set handed out per
  layout, with the exact image infos (`LastImageDescriptorSet`, tied to `_frameSerial`),
  before falling back to the hashed per-frame cache.
- **Metal's `draw()` issues encoder state only when it differs from what the encoder
  holds, and it is the ONLY writer of that state.** The pipeline, the vertex buffers at
  slots 0 and 5, the cull mode, the depth-stencil state, the stencil reference, the
  offsets of buffer slots 3 and 4 and the fragment samplers at slots 0-6 are remembered per
  encoder (`_pipelineState`, `_encoderVertexBuffer0` ..., the binder's
  `_encoderMaterialOffset` and the texture binder's `_boundSamplers`);
  `startRenderPass` forgets them with the new encoder (`resetEncoderStateCache`,
  `resetPassState`) and sets the front-face winding once. Code that sets any of these on
  `_renderPassEncoder` from anywhere else must update or reset the cache, or the next
  draw skips a bind it needed. Slot 1 is deliberately not cached (the scene block goes
  there through `setVertexBytes`). A call site that clears `_pipelineState` per draw
  (passing `first = true, last = true`) makes each draw re-issue all of it and the
  driver re-emit its render state per draw. `MetalRenderPipeline::get` also answers a
  repeat of one of the last FOUR keys from a memo, since a UI alternates between two
  pipelines (image, text, image, text) and a one-entry memo misses every draw of it.
- **Metal encodes into ONE open command buffer, and whatever must run before it, or
  needs its results, flushes it first.** `MetalGraphicsDevice::openCommandBuffer()` is
  the buffer every render pass, blit, mip generation, compute dispatch, particle
  simulation and the HUD encode into, in the order they are called; it is created on
  demand, is null while a render encoder is open (Metal allows one encoder at a time),
  and `flushCommands()` commits it. `onFrameEnd` attaches the present and the ring
  semaphores' completion handlers to whatever buffer is open then and commits it. A
  command buffer per pass and per dispatch cost a third of `post-processing`'s render
  CPU, and 200 particle emitters blocked in `commandBuffer()` once 64 buffers were
  uncommitted or in flight. Four rules follow. (1) A STANDALONE buffer committed on its
  own runs AHEAD of everything still in the open buffer, so code that reads a result
  back or waits (`MetalTexture::read`, the staging blit of a private texture write, the
  waited GPU-cull path) calls `flushCommands()` first, and code that draws over the
  frame (the ImGui overlay) encodes into the open buffer instead of committing its own,
  or the frame is drawn over it. (2) Work encoded OUTSIDE a frame — a compute dispatch
  from `Engine::update`, where the particle systems simulate — stays in the open buffer
  until the next frame end, as Vulkan queues its dispatches; `copyRenderTarget`,
  `generateMipmaps` and an offline scope flush when no frame is open. (3) A pass that
  ends with at least 256 draws or 300,000 vertices in the open buffer commits it
  (`kEarlyCommitDraws` / `kEarlyCommitVertices` in `endRenderPass`), so the GPU starts
  on the shadow passes while the CPU encodes the forward pass; never wait on that
  commit. (4) Nothing may hold the pointer across a call that can flush: ask for it
  again.
- **Vulkan's `draw()` does the same: it binds only what differs from the draw before it,
  and it is the ONLY writer of that state.** `BoundDrawState` (`_bound`) remembers, for
  the command buffer being recorded, the descriptor set and dynamic offset at each of
  sets 0-6, the vertex, instance and index buffers and the stencil reference;
  `bindSetIfChanged` is the one place a set is bound. The pipeline stays bound across
  draws (`_currentPipeline` is not cleared on `last`). On MoltenVK a redundant bind
  costs twice — recorded, then encoded into Metal at submit — and binding seven sets per
  draw was most of the gap to the Metal backend. Two repeats skip more than the bind: a
  draw whose material block has the key and version of the one before it skips the slot
  lookup, and a draw repeating the previous draw's material — same `uniformsVersion()`,
  which every mutator moves, texture setters included — and instance lightmap skips
  rebuilding set 1's image infos (a quad's inputs always rebuild). A material TEXTURE
  setter that skips `markUniformsDirty()` therefore leaves a later draw of the same pass
  on the old texture; `vulkanSmoke` swaps a material's texture between draws of one
  pass and fails if that goes unseen. The cache is forgotten wherever a
  pass begins (`beginPassState`), after the overlay, and at frame start
  (`resetBoundDrawState`). Code that binds a set, a vertex or index buffer, or the
  stencil reference on the graphics bind point from anywhere else must go through it or
  reset it, or the next draw skips a bind it needed. Per-draw work must not allocate:
  the pipeline's colour formats are a fixed array, and `_materialUniformSlots` is stamped
  with `_frameSerial` instead of cleared (clearing frees a node per material per frame).
- **A shader is CREATED before the frame's first pass draws, and on Metal creating it is
  what starts its compile.** `MetalShader` compiles its library asynchronously from its
  constructor and `getLibrary` waits for it. Compiling MSL source is the whole cost of a
  shader the system has not cached (about 0.4 s each; the back-end pipeline step is a
  twentieth of that), so shaders compiled on first use make a frame that needs sixteen
  new ones wait for them in a row — a 7 s first frame on `post-processing` with a cold
  system cache, 1.5 s with them compiling side by side. Three pieces keep the creations
  ahead of the uses: `FrameGraph::render` calls `RenderPass::prepareShaders()` on every
  pass (and its before/after passes) before the first executes; the forward pass
  resolves its render actions' variants there (`Renderer::prepareForwardShaders`, due on
  the first frame and the frame after one that built a variant); and
  `renderForwardLayer` resolves once more right before its draw loop, with the frame
  switches as they then are (one of them, the lighting-mode SSAO texture, is published
  by a pass of the same frame). A NEW PASS creates its shaders in `prepareShaders()`
  (a quad pass: `useCachedShader`; a depth-only pass: `prepareDepthOnlyShaders`) and
  calls it from `execute()` too; one that creates a shader in `execute()` alone still
  renders, and waits alone for its compile the first time. An override must cost nothing
  once its shaders exist. Measure a change here COLD: move
  `$(getconf DARWIN_USER_CACHE_DIR)/com.visutwin.<example>` aside for the run and put
  it back (a warm first frame is ~40 ms whatever the code does).
- **Vulkan keeps its compiled shaders between runs itself** (`ShaderDiskCache`, in
  `~/Library/Caches/visutwin-canvas` or `$XDG_CACHE_HOME/visutwin-canvas`;
  `GraphicsDeviceOptions::persistentShaderCache` / `shaderCacheDirectory`;
  `VISUTWIN_SHADER_CACHE_DIR=<dir>` and `VISUTWIN_SHADER_CACHE=0` override both). Two
  things go there: the SPIR-V `vulkanCompileGlsl` compiles at run time (every quad pass
  and custom shader), keyed on the stage, the defines and the WHOLE source — an entry is
  found by a hash and accepted only when the stored key matches byte for byte — and the
  device's `VkPipelineCache`, which every graphics and compute pipeline is created
  through (`pipelineCache()`, then `notePipelineCreated()`) and which is written back
  120 frames after the last new pipeline and when the device goes away. Without them
  every run reconverts each pipeline's SPIR-V (on MoltenVK most of a pipeline's
  creation) and reruns shaderc: a warm first frame of 300 ms instead of 70. A compile
  setting changed in `vulkanCompileGlsl` must change the tag at the top of its cache
  key, or old entries are served. `vulkanSmoke` points the cache at a directory of its
  own; `tests/shaderDiskCacheTests.cpp` holds the format and every miss.
- **A camera frame's depth prepass RENDERS only where something reads the depth before
  the scene pass, or under MSAA** (`RenderPassCameraFrame::prepassRenders`).
  `prepassEnabled` means "there is a depth consumer" (TAA, SSAO, DOF, fog). Under MSAA
  the prepass is the only sampleable depth, so any consumer renders it; single-sampled
  the scene pass clears and rewrites the shared depth texture, so only lighting-mode
  SSAO, which samples the depth BEFORE the scene pass, needs one. Rendered for every
  consumer at any sample count, it spends draws (a third of `taa`'s) on depth erased
  before anything samples it. The split is upstream's.
  A new consumer that reads scene depth inside or before the scene pass has to be added
  to that predicate; `tests/cameraFrameStopTests.cpp` holds the table. The prepass draws
  with the frame's TAA jitter (`Camera::jitterOffset`, which the forward view uses too),
  because under MSAA TAA reprojects from ITS depth, and only depth-WRITING materials from the
  camera's sublayers before the Depth layer, taken from the frame's cull sets.
- **A UI element on a screen OWNS its entity's transform** (upstream's patched `_sync`,
  here `GraphNodeTransformHook`, which `ElementComponent` installs on its entity):
  `GraphNode::sync`, `setPosition` and `setLocalPosition` go through the hook, the world
  transform is built from the anchors, the parent element's model transform and the
  screen matrix, and setting the position RE-DERIVES THE MARGINS. So an element's
  position is a function of its anchors, pivot, margins and screen: set them with the
  setters (upstream semantics) or once with `setup(ElementDesc)` (upstream's
  `addComponent('element', data)` order, #9525 included — a position set BEFORE setup
  survives it). On a SPLIT axis (two anchors differ) the anchors and margins set the
  size and the authored width or height is ignored; the default margins can make such a
  box INVERTED, which upstream's text alignment handles and ours does too
  (`setVerticalAlign`, default 0.5 as upstream).
  A screen-space element's world transform is CLIP SPACE: its visuals set
  `MeshInstance::setScreenSpace`, which compiles `VT_FEATURE_SCREEN_SPACE` (vertex clip =
  world xy, z 0.5), skips culling, shadows and depth-only passes, and lets ANY camera draw
  it on whatever layer the element names — no separate orthographic UI camera. A
  screen-space screen's resolution is
  `Engine::canvasSize()`, window POINTS (the space mouse events arrive in), polled by the
  screen system each update since nothing fires upstream's `resizecanvas`.
  `tests/elementLayoutTests.cpp` ports upstream's element tests.
  UI INPUT is engine-driven: `Engine::handleInputEvent` passes every SDL event to
  `ElementInput`, which delivers upstream's element events as `ElementInputEvent*` (a
  POINTER, so `stopPropagation` is shared along the bubble — a handler typed on a base or
  a value is skipped silently by `EventHandler`), in canvas POINTS; an example forwards
  nothing itself. An element needs `useInput` AND a camera drawing its layer to be hit.
  ElementInput sees mouse and finger events BEFORE the devices, and a press an element
  handler stops (`stopPropagation`) is WITHHELD from the mouse and touch devices — upstream's
  `stopImmediatePropagation` — so game code reading the devices does not act on a UI click
  (`input-events` demonstrates it). Keys and gamepads always get everything.
  An element on a screen of EITHER kind defaults to LAYERID_UI (manual draw-order sort, as
  upstream); one on no screen to WORLD. On WORLD a world-space screen's coplanar elements
  sort by distance and a panel covers its own buttons.
  Text is laid out per CODE POINT (`decodeUtf8` in `textLayout.h`); the fonts' glyph ids
  are code points, so "…" is one glyph. `markupTags()` is indexed by code point.
  A component's `onEnable` does NOT run when it is added to a live entity here, only on a
  later enable — a component that must subscribe at once does it in
  `initializeComponentData` (ButtonComponent does). `tests/elementInputTests.cpp`.
  Text and image elements are DRAWN by `ElementInput::syncElements`, which
  `Engine::render` calls before the frame (no example calls it). The visual's material is
  upstream's: EMISSIVE-only, colour times the image texture, alpha from the texture, black
  diffuse, and NOT tone mapped (`StandardMaterial::setUseTonemap(false)` compiles
  `VT_FEATURE_NO_TONEMAP`: mode NONE, no exposure, gamma still applied; a feature that changes
  tone mapping has to reach every return path, the unlit one included). Setting the colour as diffuse AND emissive makes the unlit path's
  `base + emissive` draw every glyph at twice its linear colour (white clips and hides
  it; an HDR label such as `post-processing`'s blooms twice as hard). A screen assigns its
  elements' `drawOrder` depth-first (priority in the top 8 bits) on the update after a
  hierarchy change, and the UI layer sorts its transparent sublayer MANUALLY by it, as
  upstream; a new UI visual must copy `drawOrder` onto its mesh instance or it draws in
  collection order. A 9-sliced sprite's grid is built on the CPU
  (`imageElementGeometry.h`), where upstream slices in the vertex shader;
  `tests/imageElementGeometryTests.cpp` holds it against a literal port of that shader.
  Atlas frame rects are measured from the image BOTTOM; with v = 0 at the top row a
  frame's bottom samples v = 1 - y / height (upstream's fragment stage flips its sliced v
  to the same result).
  Every shipped font is MSDF (a glyph `range`), and `StandardMaterial::setMsdfMap` puts
  the atlas page in the BASE COLOUR slot under `VT_FEATURE_MSDF`: the base-colour multiply
  skips it and the unlit path reads it as distances (median of RGB, upstream's
  `applyMsdf`, outline and shadow composited in linear). Using that slot is deliberate —
  Vulkan's fragment stage is at MoltenVK's sampler limit. A font's pages are separate
  textures, so text is one mesh instance and material PER PAGE. A page is kept raw and
  sampled bilinearly; baking the field into alpha at a fixed ramp with nearest filtering
  blurs text up close and aliases it small.
  `tests/msdfTextTests.cpp` holds the pages, the per-page split, kerning, the glyph-bounds
  extent and upstream's outline (x 0.2) and shadow (x 0.005, y by MINUS the page aspect,
  upstream's uniform as is) scaling. Upstream's own thumbnail puts the shadow below the
  text; a sign "derived" from upstream's v-up glyph UVs flips it above. Settle a
  direction on upstream's pixels — count which side of the glyph the rim
  falls on — not on a reading of its UV code, and not on a centroid of the visible rim,
  which the glyph covers.
  Text layout lives in `textLayout.h` (pure: measure, then place) and runs SYNCHRONOUSLY
  in the element's text setters, as upstream's does: `autoWidth` / `autoHeight` are ON by
  default and the element takes the text's size at once, so a caller can stack lines by
  `height()` right after `setText`. A text that wraps must turn autoWidth off, and BEFORE
  its text is set, or the element has already grown to the unwrapped width. Markup
  (`markup.h`, upstream's scanner and parser) resolves to a style per symbol; each (page,
  style) run is its own mesh instance and material (DEVIATION: upstream uses vertex
  attributes and one mesh per page). Text with tags takes the SAME shadow offset as text
  without: upstream's per-vertex path packs -width / height into y as well, so both come to
  `(0.005 x, -aspect x 0.005 y)` per page (a sign flip read off its shader alone, without the
  packing, puts a tagged run's shadow on the wrong side).
  Text is laid out on upstream's METRICS: glyphs scale by fontSize / 32 (the fonts' em),
  lines step by fontSize, and the block is aligned by the glyph `bounds` extent with
  vertical alignment 0.5 by default. Scaling by fontSize over the 64-pixel atlas cell
  draws EVERY text at half its size. Compare text size with upstream's thumbnail as a
  ratio to a
  neighbouring element (a name to its bar), which survives the thumbnail's other aspect.
- **The BACK BUFFER is MULTISAMPLED by default (`GraphicsDeviceOptions::antialias`, 4x), as
  upstream's device.** `GraphicsDevice::samples()` is its count, fixed for the device's life,
  and a pass on the back buffer takes its default ops from it (`RenderPass::init`): the colour
  RESOLVES into the drawable / swapchain image at the end of every pass, and the multisampled
  surface is stored only when a later pass loads it (the frame graph's store marking, as for
  any target). The depth is resolved (sample 0) into a single-sample depth WHENEVER a pass
  stores it — Metal `_backBufferDepthResolve`, Vulkan `_depthImage` — and that is what
  `copyRenderTarget` reads for the back buffer's depth, so a depth grab between two passes sees
  what the first drew (the pass after the grab loads depth, which is what makes the first
  store it). A pipeline drawn into the back buffer has to carry the sample count: Metal's
  back-buffer branch sets `rasterSampleCount`, Vulkan's draw takes `_activeRasterSamples`
  from the open pass (1 for the overlay, which draws single-sampled onto the resolved image,
  as the Metal ImGui overlay does). A scene that antialiases a target of its own (a camera
  frame) or draws gaussian splats turns it off, as upstream's examples do
  (`ExampleOptions::antialias`). Check a change here with `VISUTWIN_ANTIALIAS=0/1` on a
  scene using both grabs (`VISUTWIN_SSR_FLOOR` on `clustered-lighting`: SSR moves ~5,000
  pixels either way on both backends, which agree).
- **The BACK BUFFER's depth attachment carries STENCIL, on both backends, and the
  stencil follows the depth.** UI masks write it (ARCHITECTURE.md, UI masks). Metal's is
  `Depth32Float_Stencil8` (`metal::kBackBufferDepthFormat`, which every back-buffer pipeline
  names as BOTH its depth and stencil format — a pipeline that names one and not the other
  fails validation against the pass); Vulkan's is `D32_SFLOAT_S8_UINT` where the device
  supports it and falls back to the depth-only format. The stencil is cleared whenever the
  pass clears stencil OR depth and stored whenever it stores either, so no pass needs to know
  it exists. On Vulkan a layout barrier on that image must name BOTH aspects
  (`depthImageAspect()`), a SAMPLED view must name depth alone, a render-target attachment
  view both, and a depth copy stays depth-only in its region; `backBufferDepthFormat()`
  reports `PIXELFORMAT_DEPTHSTENCIL` so the depth-grab copy's target matches. Stencil state
  is PER DRAW from the mesh instance (`MeshInstance::setStencil`); the renderer resets it
  after each layer's loop, so a draw that sets none tests nothing. Note also that
  `Material::setAlphaMode` RESETS the blend, the depth state and the transparent flag —
  set it first and the rest after, or they are silently lost.
- **A layout group reflows when its INPUTS differ, not on events** (DEVIATION from upstream's
  dozen reflow-triggering events, several of which this port does not fire —
  `enableelement`, `element:add`, `layoutchild:add`). After every update the layout group
  system compares each active group's inputs — its options and size, and each child's
  serial, enabled state, width, height, pivot, anchors and layout-child settings, as raw bits
  — with those of its last reflow, reflows the ones that differ outermost first (by graph
  depth), and repeats until none does, giving up after upstream's 100 passes. The calculator
  (`layoutCalculator.h`) is a pure function, so the layouts are upstream's;
  `tests/layoutCalculatorTests.cpp` ports all 36 of upstream's cases. Two things the
  comparison gets right: the inputs are RECORDED AFTER the layout is applied and before
  `reflow` fires, so the anchors a reflow resets do not reflow it again (upstream's
  `_isPerformingReflow`) while a size a `reflow` handler sets does (the scroll view's content
  sizes itself that way); and a child is identified by `ElementComponent::serial()`, never its
  address, which a new element can reuse after a freed one and would then never be placed.
  It costs a few floats per child per frame and nothing more while nothing changes: the
  inputs are gathered into a buffer the group keeps (`_currentInputs`, swapped with
  `_lastInputs` when they differ), so the comparison allocates nothing.
- **The performance HUD is ONE draw on the UI layer, made by the FIRST camera that
  renders it** (`MeshInstance::setDrawOncePerFrame`, claimed in `collectDrawEntries` as
  the forward pass collects it). A new path that draws a forward layer's instances must
  claim them the same way, or the HUD draws once per camera. `ExampleApp` adds a
  UI-only camera, enabled while no camera of the example renders the UI layer over the
  whole window. Its quads and history texture follow the frames-in-flight rule (own
  arena, committed only on change; a ring of `maxFramesInFlight()` textures). Detail in
  ARCHITECTURE.md, examples harness.
- **UI visuals SHARE their buffers and their materials; nothing reached through an
  element's mesh instance is that element's alone.** `ElementInput::syncElements` builds
  each element's visual, and three things about it are shared. (1) GEOMETRY lives in
  `UiGeometryArena` (`framework/input/uiGeometryArena.h`): a few large vertex and index
  buffers in which each visual part has a run, indices stored absolute. A part's `Mesh`
  is its own object, but its buffers are the arena's and its primitive's `base` is the
  run's first index — add `part.geometry->firstIndex()` to anything that narrows the
  range (the text draw range does). A resize takes a NEW run and gives the old one back;
  the arena keeps a returned run out of use for `maxFramesInFlight` frames
  (`beginFrame`, once a sync), because on Metal a frame in flight still reads it. A
  buffer pair per element costs a vertex and index rebind and two driver resource-list
  entries per draw, and a resized element a pair of GPU buffers per frame. (2) The
  MATERIAL is found by value: `MaterialKey` (kind, space, texture, colour, opacity, and
  for MSDF the font range, outline and shadow) names everything the material is built
  from, and parts with equal keys draw with one material, as upstream shares its element
  materials. A part whose key changes takes the material another part already has for
  the new key, restyles its own in place when nothing shares it (`use_count() == 1`,
  re-keyed), or builds one. So never mutate a material taken from a UI mesh instance: it
  is every equal element's. A custom `element->material()` is outside all of this.
  (3) `VertexBuffer::writeRange` / `IndexBuffer::writeRange` overwrite a byte range and
  leave the rest: Metal copies into the shared storage, Vulkan stages just the range. The
  per-frame pass is kept cheap by three more things: each element holds its visual's
  record (`ElementComponent::drawRecord`, tagged with the drawer, never cloned), so there
  is no map lookup per element; the layers are compared in place rather than copied; and
  the mask walk (`syncMasks`, every element tree) runs only while some element is a mask,
  plus ONCE after the last one goes, to clear the stencil state and `maskedBy` it left.
  A change of SIZE alone gives the existing parts new geometry — the mesh instances and
  materials stay — while a change of text, image, sprite or mask rebuilds the parts. The
  visual's entity is `setExcludedFromClone(true)`: `Entity::clone` copying it would leave
  the clone drawing the SOURCE's geometry beside the visual its own sync then makes. A
  stale record under a reused element address is discarded when the new element is first
  seen. `tests/uiVisualSharingTests.cpp` holds the allocator, the arena's reuse delay,
  sharing and restyling, the resize path, the mask skip and the clone.
- **An element's corners are only as current as its entity's world transform, which is
  LAZY here.** Upstream syncs the whole hierarchy every frame; this engine computes a world
  transform when something asks for it, and that sync is what marks the corners dirty. So
  `screenCorners` / `canvasCorners` / `worldCorners` sync the entity FIRST; testing the
  dirty flag first hits an element moved since the last render where it was before. No
  example shows it — every example renders between a move and the next press — so the
  drag helper's test is what holds it.
- **A text element's default font size is 32, as upstream** (`text-element.js`); a
  default of 16 draws every text that sets no size at half upstream's (`layout-group`'s
  and `scroll-view`'s titles). A scroll view, scrollbar and button look for their elements and
  scrollbars after each update (`refreshBindings`), since nothing fires `element:add` or
  `scrollbar:add` here; the mouse wheel reaches the scroll view as a browser's pixel deltas,
  100 a notch (`ElementInputEvent::wheelPixelsX/Y`). SDL's wheel deltas already follow the
  system's natural-scrolling setting (FLIPPED only reports it), so the flag must not be
  undone, or a scroll view moves against the user's preference.
- **The back buffer is the canvas in POINTS times `GraphicsDevice::pixelRatio()`** =
  min(`maxPixelRatio`, the window's pixel density), upstream's `maxPixelRatio`.
  `resizeCanvas` takes points, as upstream's takes CSS pixels. The default is uncapped (a
  DEVIATION from upstream's browser default of 1), and `ExampleApp` caps it at 1, the density
  upstream's examples render at in a browser. Metal shrinks the layer's `drawableSize` and Core Animation scales it to the
  window. Vulkan builds its swapchain ITSELF (`initSwapchain`, not vk-bootstrap, which
  always
  takes the surface's current extent) at an extent clamped into
  the surface's [min, max]: MoltenVK allows 1..16384 and scales, and where min = max = current
  (X11, Windows) the clamp falls back to the window. A SUBOPTIMAL present rebuilds the swapchain
  only when the target size changed (`swapchainExtentStale`), since a deliberately small swapchain
  may be reported suboptimal on every present. ImGui's framebuffer scale follows the drawable.
  `vulkanSmoke` caps and uncaps mid-run under validation.
- **Leftover instance bindings follow the next draw.** The backends pick the
  instancing vertex layout by scanning bound slots, so shadow passes must unbind
  slot 5 after an instanced caster.
- **metal-cpp framework extern constants** (e.g. `MTL::CommonCounterSetTimestamp`)
  only link in the `*_PRIVATE_IMPLEMENTATION` TU. Compare string values instead
  inside the engine library.
- **`Matrix4::getElement` takes (col, row)**, not (row, col). Reading an AXIS with it is
  where this bites: `getElement(0,0), getElement(1,0), getElement(2,0)` is ROW 0 — the X
  components of all three axes — not the X axis, and the two agree only for an unrotated
  node. That spelling puts a rotated rect or disk light's LTC quad outside its own plane
  (`makeGpuLight` reads `getColumn(0)`). Use `getColumn`. It is also a full
  16-byte store and reload per call on the SSE and NEON backends (free only on
  Apple's), so a loop of them is the slow way to read a matrix: use `getColumn`,
  `store`, or a whole-matrix operation.
- **A hand-built sphere's triangle winding has to be counter-clockwise seen from
  OUTSIDE**, or its normals face inward. A mirror ball HIDES this — it still
  reflects something — and the same sphere with a diffuse material comes out black.
  `DEBUGPASS_WORLDNORMAL` says it in one frame: a
  correct sphere is blue in the middle, an inverted one is not.
- **A normal is carried by the INVERSE TRANSPOSE of the model matrix, and both
  backends compute it.** Under non-uniform scale the bare 3x3 and the inverse
  transpose differ, and lighting reads the difference directly: a flattened sphere
  shades as if it were still round. Metal uploads the matrix per draw, built by
  `Matrix4::normalMatrix()`: column i is the cross product of the other two model
  columns — the 3x3 cofactor matrix, cheaper than a 4x4 inverse — divided by the
  SIGNED determinant (their triple product); Vulkan computes the cofactor
  matrix per vertex in `shaders/vulkan/normal_matrix.glsl`, which every vertex
  module includes, and applies the determinant's sign explicitly. The two are
  equal by construction — a cofactor matrix is the inverse transpose times the
  determinant, and the shader normalizes, so the magnitude cancels and the sign is
  all that has to be put back. Do not "simplify" either side to `mat3(model)`:
  that defect is invisible in any scene whose scales
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
  lights a mirrored mesh INSIDE OUT; nothing (no `normalSign` uniform) may cancel the
  determinant's sign. Assets really do carry such nodes — one node
  of `leonardo_da_vinci.glb` is mirrored, and it is the only place a shipped
  example shows this at all.
- **A batch is one vertex layout, one primitive type, ONE pair of shadow flags, one light
  mask, one stencil state and one draw bucket** (a blended group also keeps its draw order:
  a candidate is never merged past an overlapping one it skipped), and the batch copies
  mask, stencil, bucket and draw order from its sources. A MIRRORED source is re-wound as
  it is merged (DEVIATION: upstream splits by scale sign and flips the batch's faces), so
  mirrored and plain sources share a batch. `BatchManager` merges by reinterpreting a source vertex buffer as the
  parsers' 56-byte packed vertex, so a mesh instance that is not exactly that
  layout may never enter a batch: a skinned mesh (88 bytes) or a point cloud (28)
  tagged into a batch group would merge as garbage geometry, read past the end
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

  **A source leaving a group tears that group's batches down AT ONCE.** A batch keeps raw
  pointers to its source mesh instances (and a dynamic batch to their nodes, read every
  frame), so a render component that is disabled, destroyed, moved to another group or
  replacing its mesh instances calls `BatchManager::sourcesLeaving(groupId)` while those
  pointers are still good, and the group is rebuilt at the next `updateAll()`. Upstream
  only marks the group dirty; its garbage collector keeps the sources alive until the
  rebuild, which C++ does not. Joining a group (enable, a new mesh instance, a new id)
  only marks it dirty. The rebuild collects sources with `active()`, not `enabled()`, so
  a disabled entity's meshes stay out. `tests/batchLifetimeTests.cpp` builds an engine
  on a CPU-buffer stub device and holds all four cases.

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
  camera frame the standalone grab pass copies the back buffer.
- **The grabbed scene colour is LINEAR HDR under a camera frame and GAMMA-encoded
  otherwise**, so every consumer gates its decode on bit 5 of
  `LightingData::flagsAndPad[0]`, the same bit the sky and the tail check. There are
  four such consumers — refraction and SSR, in each language — and a decode applied
  unconditionally darkens whatever samples it by roughly a stop.
- **Dynamic refraction has three traps on BOTH backends, and each can hide the
  next; all three make the surface read dark and opaque.** (1) The fragment-stage
  `lighting.viewProjection` must not be uploaded TRANSPOSED (`getElement(row, col)`
  in a binder): every refracting fragment then projects to a negative w and its grab
  UV clamps into a corner — one flat colour over the whole surface, which reads as
  "opaque", not as "wrong offset". SSR reads the same matrix. (2) The camera frame's
  scene pass stops at `lastGrabLayerId` (the skybox), and render actions exist only
  for ENABLED layers, so `RenderPassCameraFrame::findActionIndex` places the stop by
  composition position, as upstream's `addCameraLayers` does (held by
  `tests/cameraFrameStopTests.cpp`); searching render actions alone, a disabled Skybox
  layer (usual for an env-atlas-only scene) matches nothing, the pass takes every
  action, the grab runs AFTER the transparent layers, and the surface refracts itself
  from last frame. (3) The diffuse albedo applies ONCE, as upstream (the refraction
  mixes into `dDiffuseLight`, which `combineColor` multiplies by albedo), not as a
  `baseColor^(thickness + 1)` tint. (4) The refracted colour is weighted by
  `1 - getFresnel(NdotV, gloss, specularity)` (gloss-aware, iridescence included) and
  mixed by `transmission` alone, and Beer-Lambert absorbs over the length of the refracted
  vector, which carries the model scale; a Schlick weight on the whole mix turns a glass
  rim opaque. A transposed matrix hides a wrong stop completely.
  Probe it this way — output the grab at a FIXED uv (valid texture?), the flags
  `uv in range` / `w > 0` (valid projection?), and the raw sample + 0.05 (a feedback
  loop runs away to white within 120 frames).

  Compare refraction brightness against upstream only when it is pinned to the same
  pose; an unpinned capture or a differently-posed thumbnail reads as "dimmer than
  upstream" when it is not (pinned, `post-processing`'s amber matched, ours a touch
  brighter, BEFORE the Fresnel weight moved to the refracted colour — re-measure). The refraction offset is scaled by the model's
  per-axis world scale, as upstream's refractionDynamic (x60 on the amber): Metal passes
  it from the vertex stage as a flat `modelScale` varying, Vulkan's fragment stage reads
  the model matrix from the push constants it now shares with the vertex stage. Still
  open: the amber projects LARGER here than upstream at identical camera parameters. Numbers in `ENGINEERING-LOG.md`.
- **Vulkan's clip space is NOT Y-down for this engine.** The backend rasterises
  through a negated-height viewport so Metal projection matrices work unchanged,
  which puts NDC +Y at the TOP row of every target, back buffer and offscreen
  alike. A point projected in a shader therefore maps to a texture coordinate
  exactly as it does on Metal, `* vec2(0.5, -0.5) + 0.5`. A GLSL call site that
  assumes Y-down samples the grab upside down.
- **A grab pass OWNS the texture it publishes, and the device only borrows a raw
  pointer to it.** So a grab pass is persisted across frames rather than rebuilt
  (the next frame's scene pass binds the pointer before the grab re-runs), and its
  destructor clears `setSceneColorMap` / `setSceneDepthGrabMap` when the device
  still points at it — `requestSceneColorMap(false)` and any camera-frame option
  change that rebuilds render targets both destroy one.
- **Every caster sweep goes through `collectShadowCasters`, and it takes the
  CAMERA.** Batch mesh instances belong to no `RenderComponent` — `BatchManager`
  registers them straight with the scene layers — so a hand-written sweep of the
  engine's render components misses them. A directional FIT and PASS with their own
  sweeps disagree about exactly that: the fit sizes the shadow map's depth range to the
  unbatched scene while the pass draws batches into it, so a batch outside that range
  is clipped out of the map and its shadow is simply absent (`dynamic-batching` shows
  it). The camera argument filters components by layer, and a caller that fits or
  draws for one camera must pass it. Two sweeps of the same thing drift; use the
  collector.

  **A directional cascade's pass draws the list its fit prepared**
  (`LightRenderData::visibleCasters`, stamped with the frame's `renderVersion`).
  `ShadowRendererDirectional::cull` collects the scene's casters ONCE per (light,
  camera), applies the camera-independent caster rules once, and per cascade tests
  them against the FITTED frustum. It must be the fitted frustum, not the wide one the
  fit sweeps with: that camera sits a million units back and its side planes are good
  to about a tenth of a unit, which on a large frame disagrees about a few casters on a
  cascade edge (reusing the wide-frustum list is faster and draws casters too few). A
  list stamped with another frame is not used (it holds raw pointers); the pass then
  collects for itself. `ShadowCasterComponentFilter` resolves the camera's component
  once per sweep rather than searching every camera for every render component
  (`shouldRenderShadowRenderComponent`).
- **A depth-only draw sets the caster's cull mode itself** (`drawDepthOnly` →
  `resolveCullMode`, the material's cull with the node's scale flip, upstream's
  `setupCullModeAndFrontFace` in `submitCasters`). Otherwise shadow and prepass draws
  inherit whatever the previous draw left on the device: the default on the very first
  frame, the last full-screen quad's CULLFACE_NONE on every frame after. That is
  invisible with realtime shadows (every frame is the "after" case) and freezes the odd
  frame into every one-shot shadow. The tell for this class
  of bug is "the first frame differs from every later one" — re-arm the one-shot at
  frame 2 (`setShadowUpdateMode(THISFRAME)`) and compare; and read the shadow map
  back (`Texture::read` on the atlas) rather than the lit frame, converting the
  crushed perspective depth to distance before looking at it.
- **Two directional lights per layer can be shadowed, and a light may only be
  shadowed through its OWN slot.** DEVIATION: upstream samples every directional
  caster's map; here `ShadowParams::directional[kMaxDirectionalShadows = 2]` holds
  each shadowed light's map, cascade palette, distances and biases, the renderer
  gives the first two directional casters slots 0 and 1 (`shadowMapIndex`, which
  Vulkan reads from `coneParams.w` and Metal from `typeCastShadows.w`), and clears
  `castShadows` on any further one with a one-time warning. The FILTER is chosen
  per shader variant (`VT_FEATURE_VSM_SHADOWS` / `PCSS_SHADOWS`), so a light whose
  shadow type differs from slot 0's is refused the same way. Slot 0 has the base
  uniforms; slot 1 is an appended block of the same layout (`shadow1*` on Metal,
  `dirShadow1*` on Vulkan) and one shared function per language evaluates either
  (`evaluateDirectionalShadow` in `common-shadow-pcss.metal`,
  `sampleDirectionalShadow(slot, ...)` in `common-shadow-vsm.glsl`). Metal binds
  slot 1's map at texture 35; on Vulkan BOTH maps are separate images (scene set
  bindings 1 and 22) read through the shared samplers at 12 (linear, for EVSM) and
  13 (nearest, for depth), which costs no combined sampler. Check it with
  `VISUTWIN_FILL_LIGHT=35,30,1.5,1` on `ambient-occlusion`: the fill's own shadow term
  (about -2.48 counts mean) agrees between Metal and Vulkan, with the building's shadow
  the same shape on both. Expected noise: PCSS on `shadow-cascades` on Vulkan has about
  fifty isolated pixels of 3.3M off by up to 48 counts — a blocker-search threshold
  flipped by the SPIR-V compiling differently, NOT the sampler (anisotropy and LOD clamp
  are ruled out). A test light needs a valid shadowMapIndex (not -1) and nonzero
  cascade distances, as `vulkanSmoke`'s shadow-catcher step sets.
- **A depth-only Vulkan pipeline honours the bound `DepthState`'s compare function.**
  Forcing `LESS_OR_EQUAL` in `vulkanRenderPipeline.cpp` whenever the pass has no colour
  attachment silently turns the clustered atlas's per-rect clear (`clearDepthRect`: a
  depth-1 triangle under ALWAYS) into a no-op wherever a caster has written before, so
  shadows from every past position of a moving spot light accumulate, while every
  static scene renders identically either way. The golden set includes
  `clustered-spot-shadows` to catch it. When a pipeline ignores a state the engine set,
  the bug is invisible until something depends on the non-default value.
- **Vulkan PCF is Metal's comparison sampler done by hand, and must stay so.** Metal reads
  every depth shadow map through a LINEAR comparison sampler (four texels compared, the
  results bilinearly blended), and upstream's PCF1/3/5 are one, four and nine such taps.
  Vulkan binds no comparison sampler (MoltenVK's sampler limit), so `shadowTap` /
  `directionalShadowTap` GATHER the four texels at the corner they share and blend the
  comparisons by a fraction computed in the shader, and `pcf3x3Taps` / `pcf5x5Taps` are
  upstream's kernels, shared by the directional, local and clustered-atlas paths. Until
  2026-10-06 Vulkan compared 9 or 25 texels with uniform weights: every shadow edge a
  staircase of whole texels, invisible at small texels and plain where a texel covers
  several pixels (`dynamic-batching`'s 150 m shadow distance: 14k pixels off by up to 107
  counts; now none above 6, and every golden case agrees across backends to a few
  counts). Gather at the shared CORNER, not at the tap point: there the hardware's
  sub-texel rounding cannot pick a different 2x2 block from the one the weights assume.
  PCSS keeps its point taps on both backends.
- **A directional VSM bias is `vsmBias / (cascade-0 fitted far / 7)`**, the spot rule over
  the fitted depth span; passed raw, it is a variance floor several times too large.
- **A VSM SPOT stores distance / range, not depth, and an omni light asking for VSM
  gets PCF3** (upstream both). `Light::resolveShadowType` falls back with a one-time
  warning and `Light::setType` re-resolves the kept request. A spot's VSM pass (its own
  map, non-clustered only; the clustered atlas is depth-only) writes EVSM moments of
  `distance(light, p) / range`, which the shadow fragment recovers from the pass's own
  VIEW-PROJECTION (`shadowDistanceRatio`: the light is where clip x, y and w vanish, the
  far plane comes from row 2) — perspective depth at near 0.01 is crushed against 1 and
  the exponential warp has no precision left there. So Metal binds the scene block to
  the FRAGMENT stage too (buffer 1) and Vulkan's push constants are visible to it. The
  receiver compares its own distance / range less 0.0002 with the variance bias
  `vsmBias / (range / 7)` and NO normal offset (`localShadowPcss.w` flags the slot). On
  Metal the moments map cannot sit in a `depth2d` slot: it binds at 37 / 38 and 11 / 12
  stay empty; Vulkan reads it through bindings 2 / 3 with the linear sampler.
- **A shadow pass must not take its variant from a scene-wide switch set by the
  FORWARD pass.** `renderForwardLayer` sets ProgramLibrary's feature switches (VSM,
  PCSS, cookies, local shadows ...) when the forward pass executes, which is AFTER
  that frame's shadow passes. Were `getShadowShader` to read the VSM switch, the first
  VSM shadow any light renders would use the depth-only variant and write no moments; a
  realtime shadow is right one frame later, a ONE-SHOT one stays blank for good —
  zeros on Vulkan, uninitialised private memory on Metal, so the backends disagree. The
  caller passes `vsm` from the light it renders (`DepthOnlyShaders::vsm`). Anything a
  shadow or depth pass compiles must
  come from its own light or pass, never from state the forward pass leaves behind.
  Diagnose this class by reading the map back (`Texture::read`) on frame 1 and after a
  one-shot re-arm at frame 2: a map that is only right the SECOND time is a first-use
  bug, and realtime updates hide it.
- **A light's cascade RECTS come from its cascade COUNT, set in the constructor as well as
  the setter** (`Light::directionalCascadeLayout`). `setNumCascades(1)` returns early on
  an unchanged count, so with the count defaulting to one, a rect member initialised
  with the four-cascade 2x2 grid renders every directional shadow that never sets a
  count into ONE QUADRANT of its map: half upstream's resolution.
  `input-events`' ground acne is the scene (a 200 m caster ground), and a LIVE upstream
  frame at the same size has it too, matching ours number for number (ground mean
  93.63, std 3.45, range 86-99); a halved map breaks that match. Compare against a LIVE
  upstream frame
  at matched size before calling a thumbnail difference a bug: a 320 px thumbnail averages
  stripes a few pixels wide to nothing. `tests/shadowMapInvalidationTests.cpp` holds the
  default rect.
  The general rule: a setter that returns early on an unchanged value NEVER RUNS for the
  default, so anything it derives must also be derived at construction, from the same
  function; a dirty flag that starts false is the same trap.
- **The default is ONE shadow cascade, as upstream.** A one-shot directional shadow
  is unusable with more than one: the receiver picks its cascade by VIEW depth, so
  moving the camera carries the scene into cascades whose maps were fitted once to the
  near slices of the original view — zooming into a multi-cascade `ambient-occlusion`
  loses every directional shadow and zooming out brings them back. A scene that wants
  cascades sets them, and then must not use one-shot
  directional shadows with a moving camera (upstream has the same limit).
- **`shadowDistance` sets the shadow TEXEL, and a smeared or popping character
  shadow is a texel problem before it is a bias problem.** The one default cascade
  spans the camera frustum out to the shadow distance, so the ortho radius is about
  the distance and the texel is `2 * radius / resolution`: 100 m at 2048 is a 10 cm
  texel, and a 1.8 m character's legs are one texel wide and pop as it moves.
  `anim-stategraph` follows upstream's `locomotion` at 16; max(radius * 4, 100) makes
  its shadow a faint smear. Ablate before blaming the engine — that scene's big white
  zigzag survives a receive-only floor, a 16 m distance and a maximal bias, because it
  is the logo in `playcanvas-grey.png` stretched over the floor exactly as upstream
  shows it. Keep the distance a few
  multiples of the camera distance to the subject, or scale it with the camera as
  the fly demo does.
  A second cascade is not a free sharpening: the atlas splits into 2x2 quadrants,
  so every cascade renders at HALF the resolution, and the subject has to fall in
  the near one to gain anything: two cascades at distribution 0.5 split
  `anim-stategraph` at 4 m and put its 5 m character in the far cascade, with a softer
  shadow (3 cm texel vs 1.7). Reach for a cascade when the far one must cover far
  more floor than the subject needs, not to fix the subject.
- **A directional receiver's shadow depth is SATURATED, never range-tested.** The
  shadow camera's near and far are fitted to the CASTERS every frame, so a
  receiver that is not a caster — a ground plane, or any surface further along the
  light than the last caster — projects to z > 1. Rejecting that z and lighting the
  fragment (a) leaves a receiver-only ground with no shadow at all, and (b) where the
  fit ends INSIDE a shadow, cuts it off along a straight line that moves with the
  casters' bounds — a hard edge that flickers as an animated caster moves (the fly
  demo's body shadow). Clamping z to [0, 1]
  is upstream's `getShadowSampleCoord` for an ortho light: 1 compares lit against
  the cleared map and shadowed against any caster in front, and EVSM's cleared
  texels already synthesise lit moments. Keep the UV test. A ground plane may
  be receiver-only, which it should be: a huge caster inflates the fitted range
  into whole-plane acne (PCF) or blown-up penumbras (PCSS). Diagnose a suspected
  recurrence by the shape — a shadow with a STRAIGHT edge that is not any
  caster's silhouette is the far plane.
  `setShadowNormalBias` is in WORLD units: 0.1 on a 0.3 m subject lifts every floor
  receiver 6 cm and starts the leg shadows away from the feet. Size it to about one shadow
  texel of the scene.

## Measuring a backend divergence

Whole-frame mean luminance is a BAD signal: scenes animate, content differs, and
the tonemap compresses whatever you are chasing. A mean over a symmetric REGION is
just as bad in a different way — it is invariant under a mirror, so a horizontally
flipped sky measures 0.9999 against the correct one. Split every region you measure
into halves, and when two
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
5. `VISUTWIN_AMBIENT_SH=r,g,b` switches any example to the SH-probe variant with a
   uniform probe, so the probe path can be measured without a probe-setting scene.
6. `VISUTWIN_SCREENSHOT_COUNT=n` captures n CONSECUTIVE frames of one run
   (`<stem>_<frame><ext>`). A flicker is a difference between two frames of one run;
   two runs differ anyway (see the fly demo's 46k-pixel noise floor).
   `VISUTWIN_SCREENSHOT_TIME=s` arms by seconds since the first frame instead of by
   frame: an animated example's frame index is not a clock — the same frame can land
   1 s into one run's state and 2 s past it in the next run of the same binary.
7. `VISUTWIN_SSR_FLOOR=y,size[,ssr[,pole[,gloss]]]` lays a MIRROR plane under any example with
   screen-space reflections on it (third field 0 for the control) and requests the
   colour and depth grabs on every camera; a fourth field stands a magenta pillar on
   the floor at that x. A pillar on a mirror is the SSR oracle: its reflection must
   start at its base row, run its full height and stay collinear with it
   (`scratchpad`-style check: fit the pillar's centre line, measure the reflection's
   deviation from it). A test floor must be a real mirror — a metal's reflectance is
   its albedo, and a dark metal reflects at a tenth, which can "lose" the pillar. The
   fifth field lowers the floor's gloss to watch the roughness cone blur the
   reflection while the pillar stays sharp.
8. `VISUTWIN_FILL_LIGHT=pitch,yaw,intensity[,shadows]` adds a second, white
   directional light to any example. Aimed like the key light, the frame minus a run
   without it is the fill alone, and it must be as bright inside the key light's
   shadow as outside it. With `shadows` = 1 it takes the second directional shadow
   slot and copies the key light's shadow settings — without the key's distance the
   default 40 units leaves a large scene entirely past it, and unshadowed.
9. `VISUTWIN_FIXED_DT=seconds` replaces the measured frame time, so an animated
   example reaches the same state at the same frame in every run. With it, two runs
   of one binary are bit-identical and two builds CAN be screenshot-diffed.
10. `VISUTWIN_LOCAL_LIGHT=x,y,z,intensity,range` adds a white omni light, which under
    the default clustered lighting goes through the CLUSTER loop. The frame minus a run
    without it is that light alone; compare it across backends.

11. `VISUTWIN_BLOOM_THRESHOLD=t` sets the bloom threshold on every camera. No upstream
    example sets one; t=0 must reproduce the unset frame exactly.
12. `VISUTWIN_SHADOW_TYPE=n` sets `ShadowType` n on every shadow-casting directional
    light (0 PCF3, 2 VSM, 4 PCF5, 5 PCF1, 6 PCSS). `shadow-cascades` otherwise reaches VSM and
    PCSS only through a key press. `VISUTWIN_LOCAL_SHADOW_TYPE=n` does the same for every
    shadowed spot and omni light (`pcss-local` with 2 drives the spot VSM path, which no
    upstream example uses).
13. `VISUTWIN_DEBUG_PASS=n` renders every camera with `DebugShaderPass` n (1 ALBEDO,
    2 WORLDNORMAL, 9 LIGHTING). Step 1 above without editing an example: frames that
    match in ALBEDO and WORLDNORMAL but not in LIGHTING put the divergence in lighting.
    A near-zero `VISUTWIN_FIXED_DT` (1e-7) freezes an animated scene; a gap that
    vanishes frozen is TIMING (something from another frame), not shading, and a large
    step (2.0) magnifies it until the wrong backend is obvious.

14. `VISUTWIN_CPU_STATS=first,last` prints, when frame `last` is reached, the MEDIAN over
    frames [first, last] of the engine's update, its render (less the display wait — the
    time the thread was NOT running inside the calls that can block on the display, so the
    figure equals the thread's own CPU time over `render()`, checked to 0.01 ms on both
    backends from an empty scene to 20k draws, vsync on and off), and
    the renderer's per-phase frame statistics (cull, sort, forward, shadow, skin and morph,
    clusters). `VISUTWIN_NO_VSYNC=1` turns off Metal's display sync for such a run; Vulkan
    always presents FIFO. The HUD costs about 0.01 ms a frame compact and 0.05 ms with
    graphs (`MiniStats::postRender`, timed directly); one HUD-on run against one HUD-off
    run says nothing. Trust the PHASE
    timers over the whole-render
    figure: a GPU-bound frame's render time swings 1.0-3.2 ms between identical runs
    (back-pressure outside the recorded display wait), while forward and shadow hold to a
    few hundredths. A CPU claim needs the before and after binaries run
    ALTERNATELY, three times each, as a GPU claim does, and a profile before a change:
    suspected "hot" items often measure under 0.1 ms, while `sample <pid> 5` finds the
    real costs in one minute. Sort its
    output by inclusive samples per engine function; a function that owns most of the
    main thread but sits in `nextDrawable` (Metal) is waiting on the display, not working.
    On Vulkan look one level deeper: `vkQueueSubmit2` under `onFrameEnd` is where MoltenVK
    ENCODES the frame into Metal (work) as well as where it takes the drawable (a wait).
    Recording the whole submit as display wait leaves the encode out of Vulkan's render
    time (40% of the main thread at 20k draws), so Metal/Vulkan CPU figures taken that
    way flatter Vulkan. `GraphicsDevice::DisplayWaitScope` records a call's wall time
    less the thread's CPU time inside it; any new call that can block on the display
    goes through it.

15. `VISUTWIN_MAX_PIXEL_RATIO=r` caps the back buffer at r pixels per point on any example
    (the examples default to 1, which already matches upstream's default browser density for
    GPU-time comparisons, and on Retina exercises the scaled swapchain on Vulkan; 2 renders
    Retina at full density). An example that sets its own ratio (`screen-scaling`'s toggle)
    overrides it. `VISUTWIN_ANTIALIAS=0/1` turns the back buffer's MSAA off or on whatever
    the example asks (`ExampleOptions::antialias`), for a parity or cost comparison.

Animated examples cannot be screenshot-diffed across shader changes unless they run
under `VISUTWIN_FIXED_DT`.

## Feature notes

Per-subsystem detail lives in `ARCHITECTURE.md`: how each feature works, the call
that turns it on, its deviations from upstream, and — under its own "Live gotchas"
— the traps that bite while working ON that subsystem: MSAA and render targets,
the shadow pass's opacity frontend, shadow-map invalidation and the bias
convention, the SSAO/prepass pairing, camera priority, PCSS, lightmaps and probes,
wide lines, parallax, opacity dither, the scalar maps, normal-map
scaling and the Nishita atmosphere. Read that file before touching any of them.

What stays HERE is only what bites during UNRELATED work.

## Open items

- **The VRAM figure is a LOWER BOUND, and a storage buffer is counted as `sb`, not `vb`.**
  All five buckets (`tex`, `vb`, `ib`, `ub`, `sb`) are live. `ub` is the uniform memory
  a backend
  owns — its per-frame rings, every frame in flight included — which each backend reports at
  frame start (`GraphicsDevice::setBackendBufferVram`), since a ring only grows there. `sb`
  is every `VertexBuffer` once it is bound as STORAGE (`VertexBuffer::markStorageUse`: a
  `Compute` buffer parameter, a particle, splat or storage draw, and the splat buffers at
  creation; its bytes move out of `vb`), plus Metal's cluster light and cell buffers. The
  HUD's compact VRAM is upstream's `vram.totalUsed`, all five summed. The texture
  figure is CONTENT size from `TextureUtils::calcGpuSize`, fixed at construction, so
  it counts no driver padding and under-counts any texture whose mips are generated
  on the GPU afterwards (`setMipmaps` does not move `_numLevels`). That is a stable
  under-count, not a drift — the same figure is added and subtracted — but nothing
  may present this as an exact allocation total. The HUD's compact view labels
  the sum `VRAM`, as upstream labels its `vram.totalUsed`, and it is that same
  lower bound; the detailed sizes split it into textures, geometry (vb + ib) and
  buffers (ub + sb).

  The `texShadow` / `texAsset` / `texLightmap` SPLIT is live too, and the three
  sub-buckets DELIBERATELY DO NOT SUM to `tex`. A texture joins one
  only where its creation site sets `TextureOptions::profilerHint`: the parsers
  (glb/obj/assimp), the four texture-asset paths in `asset.cpp` and the font atlas
  are ASSET; `ShadowMap::create` (depth map and VSM blur temp) and the clustered
  shadow atlas are SHADOWMAP; both lightmap bakes are LIGHTMAP. Everything else —
  render targets, the post-processing chain, env atlases, area-light LUTs,
  reflection probes, scene grab — stays `TEXHINT_NONE` on purpose, because it is
  none of those things. Nothing on screen shows the split today; anything that does
  must show the remainder as "other" rather than let a subtraction that does not
  balance read as a bug. **Add the hint when you add a texture creation site**, or its
  bytes land only in the undifferentiated total.
- **Under MSAA the sampleable scene depth is the PREPASS's texture, not an
  attachment of the scene target, so a resize has to resize it by hand.**
  `RenderPassCameraFrame::frameUpdate` resizes the scene target from the device
  size and must resize `_sceneDepthTexture` with it: left at the original window size
  while the prepass target is rebuilt around it, SSAO samples a small depth at
  full-window coordinates and the whole frame turns dark and streaked once the window
  grows wide enough. A window STARTED at the large size is always right; that
  difference is the test for any resize bug: resize at frame 30 and diff against a run
  started at
  that size, expecting zero differing pixels. Log the sizes every pass sees
  (target, source texture, device) rather than reasoning about which object a
  resize reaches.
- **The SSR march samples the colour grab at LOD 0, point-samples the depth,
  bisects to the crossing and reaches 0.4 of the camera range.** All four in both
  chunks. The colour grab is MIPMAPPED and the fetch sits behind a data-dependent
  loop, so an implicit LOD takes undefined derivatives and reads the coarsest mips:
  every reflection becomes the scene's average, boxes turn into blobs and a thin
  pillar vanishes (the light-cookie trap again). The depth tap is a silhouette test
  and must be nearest (Vulkan's head binds `nearestClampSampler`). A hit accepted at
  the coarse sample sits up to a step past the intersection, so the crossing is
  bisected six times and the thickness judged at the refined point, with a rejected
  silhouette jump continuing the march instead of ending it. A fixed reach in world
  units cuts a tall pillar's reflection short and reflects nothing across a large
  hall; the reach is 0.4 x (far - near) in 48 steps, thickness 1.25 steps. With the
  pillar oracle the reflection runs the pillar's full height on both backends,
  contiguous at the base, mean sideways deviation under a pixel. A ROUGHNESS CONE picks
  the colour mip: the GGX lobe's half-angle is taken
  as roughness^2, its footprint at the hit is tan(cone) x hit distance, converted to
  grab pixels by the focal length (the view-projection's clip-y row length x half the
  grab height, since V's rows are unit) over the hit's depth, and log2 of that is the
  LOD — so a rough floor blurs its reflection instead of fading it, and only
  roughness above 0.7 fades. Reference, the pillar on the mirror floor at gloss
  0.98 / 0.8 / 0.6: floor high-pass energy about 4.4 / 3.4 / 2.4 while the pillar's
  own stays put, Metal and Vulkan within 3%. Still true: objects thinner than a step
  can be skipped.
- **A camera frame owns BOTH grabs.** `CameraFrameOptions::sceneDepthMap` (from
  `CameraComponent::requestSceneDepthMap`) gives the frame a `RenderPassDepthGrab`
  with an explicit source, its offscreen scene target, placed beside the colour
  grab; either request splits the scene pass at the grab layer. Under MSAA the scene
  target's depth is an internal multisampled buffer no copy can read, so the frame
  publishes its PREPASS depth texture as the grab map instead, which needs the
  prepass (TAA, SSAO, DOF or fog) — without one it warns and SSR has no depth.
  Without a depth grab in the frame, SSR under any post-processing does nothing on
  either backend. The standalone `RenderPassDepthGrab` publishes the scene depth in
  `before()` for the depth-layer flow; with a source set it only copies. Check on
  `post-processing` with `VISUTWIN_SSR_FLOOR`: the floor's SSR on/off difference is
  ~10.8k pixels on both backends and both paths, and Metal and Vulkan agree on the
  floor mean to 0.1.
- **UI not ported yet**: right-to-left text, XR select events and grapheme clusters (emoji
  sequences are several symbols). The twenty-two UI examples
  (`ui-text`, `ui-text-markup`, `world-to-screen`, `ui-buttons`, `world-ui`, `input-events`,
  `screen-scaling`, `ui-panel`, `text-justify`, `text-typewriter`, `masking`, `layout-group`,
  `scroll-view`, `common-widgets`, `anchors`, `image-fit`, `drag-and-drop`, `render-to-image`,
  `text-auto-font-size`, `ui-custom-shader` — upstream's user-interface/custom-shader — and
  `ui-particle-system` — user-interface/particle-system — and `text-localization`) port
  upstream's CURRENT versions; `text-emojis` is not ported yet (it draws with upstream's
  `CanvasFont`, a system-font rasteriser this port does not have).
- **Example coverage gaps.** Nothing exercises: SH light probes (drive them with
  `VISUTWIN_AMBIENT_SH`), SSR (drive it with `VISUTWIN_SSR_FLOOR`), gsplat SH bands 1-3, detail
  normals (upstream's `test/detail-map` cannot be ported faithfully — it toggles
  diffuse, normal and AO detail maps and only NORMAL exists here), fog of any
  type, or sheen. The last two mean a change to those paths has to be driven
  deliberately to be seen at all. Iridescence is driven:
  `reflection-planar-blurred`'s lenses (`SunglassesKhronos.glb`) carry it, and match
  upstream's thumbnail.
- **The cluster loop owes every material term the main light loop has.** With clustered
  lighting the default, every spot and omni light is shaded in
  `forward-fragment-clustered.*`, not the main loop. Direct clearcoat uses `ccNormalW`
  and the gloss-mapped `ccAlpha2` on both backends, not the base normal and the
  material's flat coat roughness. Check with `VISUTWIN_LOCAL_LIGHT=6,2,1,3,15` on
  `clearcoat`: the light's contribution matches between backends to 0.00 counts on
  average. Sheen, iridescence and Oren-Nayar
  under a local light are ported line for line from the main loop but no example drives
  them. When a term lands in one light loop, add it to the other (main, cluster) on both
  backends. An area light's LTC terms are helpers in `common-ltc`, which both loops call.
- **Clearcoat composes as upstream's energy-conserving
  `lit * (1 - Fc * cc) + (ccDirect + ccReflection) * cc`, with a clearcoat IBL
  reflection, on both backends**, and the three clearcoat maps are on both. A SEPARATE
  image read through a shared sampler must be filtered EXACTLY as the per-texture
  sampler would filter it — check anisotropy, not just filter and wrap — or every
  oblique surface diverges by backend (a smooth map such as the parallax height map
  hides it; a ribbed coat normal map shows it). Reference: `clearcoat` on Vulkan reads
  a mean absolute difference of 0.002 counts against Metal on the whole 900x700 frame,
  with 12 pixels above 8 counts — isolated specular glints.
- **Queued — none a correctness bug:** the
  binding footprint exceeds WebGPU's defaults (Metal 36 texture slots, Vulkan 7 sets). The
  parsers derive tangents through one `generateTangents` / `tangentFromNormal`
  (`packedVertex.h`), whose sign rule is opposite to `calculateTangents`; glTF animation
  tracks keep file order (`AnimTrackList`). Measured
  and dropped (`VISUTWIN_CPU_STATS` plus `sample`): the per-(camera, layer)
  culling sweep is at most 0.1 ms a frame even at 2,173 draws; `renderForwardLayer`'s own
  per-sublayer work is under 2% of the main thread; the graph build and its allocations
  do not register; Vulkan's set 4 per skinned draw and a variant-sorted caster list
  have nothing to act on in any shipped scene. Revisit them only with a scene that shows
  them in a profile.
- **The ambient diffuse is scaled by `(1 - specularity)` on both backends**, right
  where upstream's `litForwardBackend` does it after `addAmbient`: per channel, F0 in
  either workflow, only when the material renders specular, and only on the ambient
  irradiance (a lightmap that replaces it is not scaled, nor is direct light).

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
- Code comments mention upstream ONLY where the reader needs it: a `DEVIATION:`
  note, an example's file header naming the upstream example it ports, a
  deliberately reproduced upstream quirk that would otherwise look like a bug, and a
  test whose oracle is upstream's own code or cases. No "as upstream", no upstream
  function or file names as attribution, no upstream issue or commit numbers —
  describe what the code does and why. Port history belongs in the engineering log.
- Every header and source file opens with the same block, before any descriptive
  comment: `// SPDX-License-Identifier: Apache-2.0`, `// Copyright 2025-2026 Arnis
  Lektauers`, `//`, `// Created by Arnis Lektauers on DD.MM.YYYY` (the day the file
  is created, no trailing period), `//`.
- Shader features: `VT_FEATURE_*` prefix (not upstream `PC_*`), declared once in
  `platform/graphics/shaderFeatures.h`
