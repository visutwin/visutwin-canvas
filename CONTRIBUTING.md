# Contributing

Notes for working on the engine itself. For building and using it, see the
[main README](README.md).

## Layout

```
visutwin-canvas/
├── engine/
│   ├── src/
│   │   ├── core/                  # Math, events, tags, shapes, curves, utilities
│   │   ├── platform/              # Graphics abstraction + Metal and Vulkan backends, input
│   │   ├── scene/                 # Scene graph, renderer, materials, lighting, shader-lib
│   │   ├── framework/             # Engine, Entity, components, assets, anim, gizmos, XR
│   │   ├── viz/overlay/           # ImGui overlay for digital-twin HUD
│   │   └── util/                  # General utilities
│   ├── lib/                       # Vendored: metal-cpp, stb
│   └── shaders/                   # Metal chunks + embedded programs, Vulkan GLSL
├── examples/                      # 46 example applications (upstream ports + one original scene)
├── tests/                         # Unit tests + Vulkan validation smoke test
├── tools/                         # Build and utility tools
└── assets/                        # Example assets (some procedural, some user-provided)
```

The engine is 498 C++ source/header files.

## Conventions

- `shared_ptr` for ownership, raw pointers for non-owning references
- `_camelCase` private members, `camelCase()` getters, `setCamelCase()` setters
- Shader features use the `VT_FEATURE_*` prefix
- PlayCanvas is referred to as **upstream** in comments; mark intentional
  divergence with a `DEVIATION:` comment explaining why

## Tests

Unit tests live in `tests/` and register with CTest. They are built by the
`default` preset:

```bash
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```

| Test | Covers |
|------|--------|
| `graph-node-invariants` | Scene-graph hierarchy and transform invariants |
| `stencil-parameters` | Stencil state packing |
| `blend-state` | Blend state packing (including the dual-source `BLENDMODE_SRC1_*` factors) |
| `camera-projection` | Camera projection matrices |
| `vulkan-validation-smoke` | Vulkan backend brought up under the validation layers |

## Vulkan validation

Debug builds enable the Vulkan validation layers by default. The `vulkan` preset
registers `vulkan-validation-smoke` with CTest; the test brings the backend up,
renders, and fails on any validation-layer error:

```bash
cmake --preset vulkan
cmake --build build-vulkan --target visutwin-vulkan-smoke
ctest --test-dir build-vulkan -R vulkan-validation-smoke --output-on-failure
```

The test is only registered when `VISUTWIN_BACKEND_VULKAN` is on, and carries a
60-second timeout with the `vulkan;smoke` labels.

## Running an example for inspection

`tools/run_example.py <binary> <logfile> <seconds>` runs an example under a pty
for a fixed time. spdlog block-buffers when stdout is a pipe, so a run killed by
a signal otherwise loses its tail — exactly the part that matters. Combined with
`VISUTWIN_SCREENSHOT` (see the
[runtime environment variables](README.md#runtime-environment-variables))
this gives a reproducible way to capture what a change actually renders:

```bash
VISUTWIN_SCREENSHOT=shot.png python3 tools/run_example.py \
    build-examples/examples/visutwin-anisotropy.app/Contents/MacOS/visutwin-anisotropy run.log 20
```

The script prints `DIED(<code>)` if the example exited on its own, or `ALIVE` if
it was still running at the deadline and had to be killed.

## Shaders

Shaders are written once, in Slang, under `engine/shaders/slang/`: one file per program
in `programs/`, shared modules in `modules/`, the forward program's chunks in `forward/`.
The build compiles every program into `slang_shader_bundle.h` (MSL and a metallib for
Metal, SPIR-V for Vulkan), so an edit needs a rebuild; `VISUTWIN_SLANG_RUNTIME=1` compiles
from the source tree at launch instead, which picks up an edit without one. Variants are
the feature words of `engine/src/platform/graphics/shaderFeatures.h`, bound as Metal
function constants and Vulkan specialization constants.

The hand-written forward and shadow programs (`engine/shaders/metal/chunks/`,
`engine/shaders/vulkan/`) remain until the migration's last phase, selected with
`VISUTWIN_LEGACY_FORWARD=1` for a comparison in one binary.
