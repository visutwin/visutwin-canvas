# Engineering Log

Narrative record of completed work on VisuTwin Canvas: postmortems, migration
accounting, and verification results. Split out of `CLAUDE.md` on 2026-09-04,
verbatim, so that file could go back to being what an agent must READ BEFORE
CHANGING CODE: rules, contracts, live gotchas and open items.

Nothing here is a standing instruction. If a rule in this file still binds, it
also appears in `CLAUDE.md`, and that copy is the authoritative one. Entries are
newest first within each topic.

## SIMD backend defects: a wrong horizontal sum, an ungated SSE path, a missing determinant guard (2026-09-06)

**Four fixes across the math backends, none of them reachable on Apple silicon
except the last — FIXED.**

`Vector2::dot` and `Vector4::planeNormalize` both ended their SSE horizontal sum
by broadcasting lane 1 (`_MM_SHUFFLE(1,1,1,1)`) instead of bringing lane 2 down
(`_MM_SHUFFLE(1,0,3,2)`, which the correct sibling `Vector4::dot` already used).
`Vector2::dot` therefore returned twice the dot product, making `length()` a
factor of sqrt(2) too large. `planeNormalize` returned `2*(x*x + y*y)` with
`z*z` dropped entirely, and it is the only consumer in `Frustum::create`: a
plane whose normal lies along Z — the near plane of any axis-aligned camera —
measured as zero length, failed the `len > 0` test and collapsed to
`(0,0,0,0)`, a frustum plane that culls nothing.

The SSE path could not have compiled in the first place. It was selected on
`__SSE__`, which every x86-64 target defines, while using SSE4.1 intrinsics
(`_mm_dp_ps`, `_mm_insert_ps`) that clang's x86-64 baseline stops short of, and
no `-msse4.1` appears anywhere in the build. It is now gated on `__SSE4_1__`, so
a stock x86-64 target falls through to the scalar path rather than failing to
build. That also means these two arithmetic fixes are **unverified by execution**:
they are correct by inspection against `Vector4::dot`, and the argument for an
x86 build in CI is now concrete.

`Matrix4::inverse` on the Apple backend returned `simd_inverse` unguarded, where
the scalar branch and upstream (mat4.js) return identity on a zero determinant.
A zero component in a node's local scale is the ordinary way to reach it, and
the resulting NaN flows through `GraphNode` into the view matrix and every
frustum plane derived from it. This one IS live on Apple silicon: removing the
new guard makes `tests/simdMathTests.cpp` fail with infinities, which is how it
was confirmed rather than assumed.

**A correction to the audit.** It also listed `Quaternion::normalized` as
NaN-producing on the Apple backend. That is wrong: `Quaternion::simd` is a
`simd_quatf` there, and `simd_normalize` of a zero quaternion returns identity
already (probed directly — the zero *float4* overload is what returns NaN, and
this type does not use it). The zero guard was still added, because the SSE path
(`_mm_rsqrt_ps` of zero) and the NEON path (`1/sqrt(0)`) genuinely do produce
NaN, and `invert()` routes through it. The SSE path also swapped
`_mm_rsqrt_ps` for an exact reciprocal square root: ~12 bits of mantissa drifts
visibly through repeatedly renormalised rotations.

## KTX2 always transcoded to ASTC, and three pixel formats had no descriptor (2026-09-06)

**Two defects from the 2026-09-06 upstream audit, both invisible on Apple
silicon — FIXED.**

`pixelFormatInfo` (platform/graphics/constants.cpp) is a map the enum does not
enforce, and `R32F`, `DEPTH16` and `BGRA8` had enumerators with no entry, so
`pixelFormatBytesPerPixel()` returned 0 for them. The Vulkan upload path sizes
its staging copy from exactly that value (vulkanTexture.cpp), and R32F is the
format `renderTarget.cpp` blesses for MSAA depth resolve while BGRA8 is the
back-buffer copy format. `tests/pixelFormatTests.cpp` now lists every
enumerator and fails if one has no descriptor; it cannot iterate the enum, so
adding a format means adding a line there too.

The KTX2 transcode target was a hard-coded ASTC 4x4 default with no capability
query anywhere, following upstream's `basis.js chooseTargetFormat` in neither
spirit nor letter. ASTC is native to Apple GPUs and absent from desktop
NVIDIA/AMD, so every KTX2 asset on a desktop Vulkan GPU would have attempted an
image creation in an unsupported format. `GraphicsDevice` gained
`supportsCompressedFormat()` — Metal answers from `supportsFamily(Apple2)` and
`supportsBCTextureCompression()`, Vulkan from `textureCompressionASTC_LDR` /
`textureCompressionBC` plus the sampled-image bit for the mapped `VkFormat` —
and `preferredCompressedRgbaFormat()` picks ASTC, then BC7, then DXT5, then
uncompressed RGBA8. The transcoder gained the uncompressed path (which counts
pixels, not 4x4 blocks, in both the buffer size and the count it is handed).

**There are FOUR KTX2 call sites, not one.** The audit named
`resourceLoader.cpp`, but the path every example actually uses is
`asset.cpp:149`, and the GLB parser has two more for `KHR_texture_basisu`.
Fixing only the one the audit cited would have changed nothing observable. The
format is chosen once on the main thread and passed down, because all three of
the other sites run on a worker that must not touch the device;
`TextureResourceHandler` and `ContainerResourceHandler` take it at construction.

Verified on both backends with `render-to-texture`, which loads a KTX2
checkerboard: each logs the target it picked at startup, both choose ASTC 4x4 on
this Apple silicon machine, and both render with zero errors. The hardware here
cannot exercise the BC branch — that needs a desktop GPU, and the startup log
line exists so the choice is visible when someone runs it there.

## Transparent draws sorted on radial distance (2026-09-06)

**Off-axis transparent surfaces sorted behind centred ones at the same view
depth — FIXED.** The forward pass ranked draws by the squared radial distance
from the camera to the AABB centre. Upstream's `_calculateSortDistances`
(layer.js) uses the signed depth along the camera's forward vector, and the two
disagree by up to `1 / cos(fov / 2)`: at an 80 degree field of view a surface at
the edge of the frame read 30% farther than one at the centre at the same depth,
so two overlapping transparent quads could swap order as the camera panned. The
radial form also cannot tell "behind the camera" from "in front". The renderer
now computes `forwardSortDistance` (scene/renderer/sortDistance.h) once per
draw, the camera forward once per layer, and `MeshInstance` gained upstream's
`calculateSortDistance` hook for instances that know a better answer (a
particle system, a large sheet). The gsplat path already derived the same
forward vector inline and now shares the helper.

Held by `tests/sortDistanceTests.cpp`: an off-axis and a centred point at the
same depth read the same distance (radially they did not), a nearer off-axis
point sorts in front of a farther centred one that was radially a tie, a point
behind the camera reads negative, and a yawed or scaled camera node still
yields a unit forward. Refraction and layers render as before on Metal.

## CAS sharpness was a blur (2026-09-06)

**Raising `RenderingSettings::sharpness` softened the image — FIXED.** Upstream's
compose pass feeds its CAS kernel `lerp(-0.125, -0.2, sharpness)`: the neighbour
weight is NEGATIVE, which is what makes `(w*(a+b+d+e) + c) / (4w + 1)` an
unsharp mask. This port passed the raw positive user value into the identical
kernel, where a positive weight is a convex blend of the pixel with its four
neighbours, and the shaders gated the stage on `sharpness > 0`. Every scene that
asked for sharpening got a 5-tap blur instead. The remap now lives in
`RenderPassCompose::execute`, zero keeps the stage off, and both shader bodies
gate on `< 0`. Both bodies also gained upstream's `max(max_g, 1e-4)` guard on the
contrast estimate (Metal divided by `max_g` raw, GLSL used a different epsilon)
and the same guard on the HDR re-expansion.

Measured on the post-processing scene with gradient energy over a static crop of
the statue (the orbs animate, so whole-frame diffs are meaningless): at
sharpness 0.4 the old pass read 6.96 against 7.44 with sharpening off — below
the reference, a blur — and the new pass reads 8.54. Vulkan shows the same
ordering. A "settings in, uniform block out" table test is the right guard for
this class of defect and is on the test list from the 2026-09-06 audit.

## Ambient occlusion never reached the diffuse light (2026-09-06)

**An AO map had no effect on diffuse lighting on Metal — FIXED.** The
2026-09-06 upstream audit found that `forward-fragment-ambient.metal` multiplied
only `directDiffuse` by the occlusion term, and only under `occludeDirect`;
`indirectDiffuse` was never touched. Upstream's `litForwardBackend.js` does the
opposite split: `occludeDiffuse` runs on the AMBIENT term unconditionally
(before `addLightMap` and before the light loop), and runs a second time over
everything only under `occludeDirect`. So with the default material a baked AO
map, or lighting-mode SSAO, only ever darkened the specular. Both backends now
follow upstream: the ambient diffuse is occluded always, the direct diffuse and
any lightmap only under `occludeDirect`. The Vulkan chunk gained the
`occludeDirect` gate and the `occludeSpecular` mode and intensity switch it
had been ignoring; it needed a `directDiffuse` accumulator and a split of
`indirect` into diffuse and specular halves, and because `color` is accumulated
in place there, an occluded share is taken back out as `x * (f - 1)`.

Two things made this invisible. The ambient-occlusion example "disables the
baked AO map" with `setAoMap(nullptr)`, but the GLB parser fills the base
`Material::occlusionTexture`, a separate property on the same texture slot, and
the shader feature was `aoMap() || occlusionTexture()` — so the example rendered
with its AO all along and the toggle did nothing. `StandardMaterial::setAoMap`
now writes through to the base property. And the effect is subtle in that
scene: 8.5% of pixels get darker by up to 85 counts, none brighter, in the
crevices of the ORM texture's red channel.

Verified on Metal with the laboratory scene, same binary, chunks hot-reloaded:
map on, old vs new chunk, 8.46% of pixels darker and 0.002% brighter, mean
113.43 to 112.97; map genuinely off (after the setter fix), old vs new chunk,
mean absolute difference 0.001, i.e. the change is a no-op when `ao == 1` as it
must be. Run-to-run noise on the same binary measured 0.003.

## Vulkan lighting parity (2026-09-03 / 04)

### Local-light shadow casters inherited the skybox vertex stage
**Vulkan spot and omni shadows cast nothing — FIXED (2026-09-04).** The local
shadow passes were valid, submitted the same caster population as Metal (64
spot draws and 47/46/7 draws for the populated omni faces in `pcss-local`), and
produced no validation errors, but a direct D32 readback stayed at the clear
value. The missing state reset was one frame earlier: the forward pass normally
ends on the skybox, and `RenderPassShadowLocalNonClustered` bypassed materials
without clearing that binding. `VulkanGraphicsDevice::draw` selected its special
depth-pinned skybox vertex module from the stale material, even though the
active shader was the shadow program, so every caster landed at the far plane.

The local shadow pass now explicitly clears material state, matching the
directional pass. Vulkan also derives skybox pipeline/module selection from the
resolved shader feature set instead of mutable material state, which prevents
any material-less draw from repeating the failure. Verified in `pcss-local` for
both modes: PCF produces the two crisp spot/omni floor shadows and PCSS produces
the expected contact-hardening blur, with Vulkan validation clean.

### Material colours were never sRGB-decoded
**Vulkan material colours were never sRGB-decoded — FIXED (2026-09-03).** This was
the bulk of the "camera-frame" brightness gap, and it was never camera-frame
specific. Material colours are authored in GAMMA space here (`setDiffuse` stores
its colour raw — unlike `setEmissive`, which `StandardMaterial::updateUniforms`
pre-linearises), so the shader owes them a decode. The Metal chunk decodes the
base-colour FACTOR, the base-colour TEXTURE and the VERTEX colour (the last in the
vertex stage, once per vertex). The Vulkan chunk decoded none of the three and
multiplied the raw values straight together, which left every surface brighter and
washed out. Fixed in `forward-fragment-surface.glsl` and in
`forward_color.vert` / `forward_point.vert`.

Why it read as a camera-frame bug: outside CameraFrame the forward pass tonemaps
and gamma-encodes immediately, which compressed the error to a few percent; under
CameraFrame the linear HDR carries it through to compose intact.

The measurement that settled it is worth reusing: `DEBUGPASS_ALBEDO` and
`DEBUGPASS_LIGHTING` (`Camera::setDebugShaderPass`, wired on BOTH backends) split
the frame into its material frontend and its lighting. Albedo was off by 11-40 per
channel and now matches Metal to 0.2. Whole-frame mean luminance is a BAD signal by
comparison — scenes animate, content differs, and the tonemap compresses whatever
you are chasing.

### Direct diffuse was 1/PI * kD too dark
**Vulkan direct diffuse was 1/PI * kD too dark — FIXED (2026-09-03).** The second
half of the same gap, and it was DIRECT light, not the environment. Metal and
upstream both compute direct diffuse as `albedo * radiance * NdotL`: upstream's
`lightDiffuseLambert` is a bare NdotL and its combine multiplies by albedo, and the
Metal chunk matches. The Vulkan chunks divided by PI and multiplied by
`kD = (1 - F)(1 - metallic)`, which made every direct light about a third of
Metal's — and kD applied `(1 - metallic)` a SECOND time, since `diffuseAlbedo`
already carries it. Fixed in `forward-fragment-lights.glsl` (punctual and
directional) and `forward-fragment-clustered.glsl` (local lights), including the
lightmap-bake accumulators so a bake still matches the lit result.

How it was isolated, which is the reusable part: strip the scene one term at a
time with `DEBUGPASS_LIGHTING` and compare. Light off, no ambient, no env atlas →
both backends agreed to 0.1, proving everything except the light matched. Light on
→ Metal (165.9, 160.0, 148.7) against Vulkan (113.2, 111.1, 106.9), a constant
0.356 ratio in all three channels, which is a single scalar and not a colour or
texture problem. After the fix, (165.9, 160.0, 148.7) against (165.6, 159.8, 148.5).
Whole-scene parity: 8 of 11 examples now within 10% of Metal where several were 40%
or more out.

DO NOT chase the environment specular first, as an earlier note here suggested.
The ambient `kD` and Fresnel divergences in `forward-fragment-ambient.glsl` are
real and still unfixed, but they are worth only a fraction of a percent on
`shadow-cascades` — I tried them, they made `post-processing` much worse, and they
were reverted. The env atlas contributed almost nothing to this gap.

### There were no directional shadows at all
**Vulkan had NO directional shadows at all — FIXED (2026-09-03).** Not a shading
divergence: the shadow map was never written. `ProgramLibrary::getShadowShader`
opened with `if (!_device || !hasProgram("shadow")) return nullptr;`, and
`registerGlslPrograms` deliberately registers no "shadow" chunk program, because
the Vulkan shadow shader comes from prebuilt bundle modules selected by the
definition NAME rather than by chunk composition. So on Vulkan that call always
returned null, every shadow pass returned at its `if (!shadowShader)` guard before
drawing a single caster, the map stayed at its cleared 1.0, and every fragment read
as lit. The guard now applies only to MSL; `buildForwardShaderVariant` already falls
back to the bundle for a program with no chunked GLSL form.

This was a REGRESSION from the ShaderChunks-on-Vulkan work: before the registration
was split per language, "shadow" was registered for both.

Verified: the pass now draws 68 casters per face on Vulkan, the same as Metal, the
tree shadows are there, and `shadow-cascades` moved 162.0 -> 158.8 against Metal's
158.5. Both shadow passes now log once when they have no shader, because a total,
silent loss of shadows reads as a shading bug and cost a long search here.

## Vulkan camera-frame gamma (2026-09-02 / 03)
**Vulkan camera-frame washout — FIXED (2026-09-02).** Every camera-frame scene
rendered pale and low-contrast on Vulkan (`shadow-cascades`, `depth-of-field`),
which is what blocked pixel-validating the volumetric fog port. Cause: under
CameraFrame the forward pass must output LINEAR HDR and leave exposure, tonemap
and gamma to compose. Metal has always done that, gated on bit 5 of
`LightingData::flagsAndPad[0]`. On Vulkan the field existed and was documented as
mirroring Metal 1:1, but **nothing ever set the bit and the shader never read
it** — so the forward pass tonemapped and gamma-encoded, then compose did both
AGAIN. A double gamma is exactly "brighter, washed out". Fixed in two places:
`vulkanGraphicsDeviceDrawBinding` now keeps bit 5 in step with `hdrPass()`, and
`forward-fragment-tail.glsl` returns linear HDR early like the Metal chunk.
Scene means moved 222 -> 185 and 206 -> 157 (Metal: 160 / 94); non-camera-frame
scenes are byte-identical (clearcoat unchanged), since the bit is never set there.

**Sky under CameraFrame — FIXED (2026-09-03).** The same double-gamma, one layer
up: the Vulkan sky block in `forward-fragment-surface.glsl` applied exposure,
tonemap and gamma unconditionally and returned, so it never saw bit 5. Under
CameraFrame it wrote gamma-encoded sRGB into the linear HDR target and compose
encoded it again. Metal checks the bit in all three of its sky paths
(atmosphere / cubemap / env atlas); Vulkan now checks it too.

## Backend-agnostic effect passes: the QuadRender migration (2026-09-01 to 03)
The six migrations that emptied `PostPassKind` and deleted the Vulkan
post-process framework, in the order they were done.
**Migrated: `RenderPassVsmBlur`, `RenderPassVolumetricFog` (+ its combine).** It is now one implementation
(`scene/renderer/renderPassVsmBlur.cpp`) carrying MSL and GLSL source selected by
`GraphicsDevice::shaderLanguage()`, replacing `MetalVsmBlurPass` (347 lines),
`VulkanGraphicsDevice::executeVsmBlurPass` + its pipeline/descriptor helpers (243
lines), the `executeVsmBlurPass` virtual, `VsmBlurPassParams`, and the build-time
`vsm_blur.{vert,frag}` modules — net -450 lines, verified pixel-identical on both
backends. Blur direction became a uniform instead of two compiled variants.

**Volumetric fog (2026-09-02)** was the second migration and the first to need
more than the seam had: its uniform block is 512 bytes, larger than the
400-byte `MaterialUniforms` the quad block was riding. `kPerDrawUniformCapacity`
(512) now sizes that slot, the Vulkan material descriptor's range, and the padded
allocation behind it. The effect deleted `MetalVolumetricFogPass` (549 lines) and
both fog virtuals; shaders live in `scene/graphics/volumetricFogShaders.h`.
**This gave the Vulkan backend volumetric fog, which it never had** — it is
enabled and visibly affects the frame, but is NOT pixel-validated: the
`shadow-cascades` scene renders paler on Vulkan than Metal *with fog disabled*,
a pre-existing backend difference that swamps the comparison. Metal output is
unchanged. DEVIATION: the GLSL shadow tap is a manual depth compare (the Vulkan
backend samples shadow maps through a non-comparison sampler everywhere), where
MSL gets hardware PCF from `sample_compare`.

**CoC, DOF blur and depth-aware blur (2026-09-02)** went next — three simple
quad effects, deleting `MetalCoCPass`/`MetalDofBlurPass`/`MetalDepthAwareBlurPass`,
three device virtuals, their Vulkan `executePostPass` arms, three params structs
and the `post_{coc,dof_blur,depth_blur}.frag` bundle modules. CoC and DOF blur are
pixel-identical on Metal; the depth-aware blur differs on ~2.8% of pixels
(visually identical, confined to AO contact edges). That last one is NOT a shader
change — the MSL is semantically identical — it is the geometry: the old pass drew
a 3-vertex fullscreen triangle, `QuadRender` draws a 4-vertex tri-strip, so UV
interpolation differs in the low bits and the BILATERAL weight amplifies it at
depth discontinuities. CoC and DOF don't expose it (CoC samples at texel centres,
DOF clamps its taps).

TWO TRAPS met here, both worth remembering. (1) The CoC effect was **divergent
between backends**: Metal ramped from the focus distance with (far, near) channel
order, Vulkan used a +/- range/2 dead zone ramped over the HALF range with the
channels SWAPPED, and each backend's DOF blur read its own convention. Upstream's
`coc.js` has the dead zone AND (far, near) — so each backend had half of it right.
Both now follow Metal's behaviour; aligning the ramp with upstream is a one-line
change in one place (noted in `renderPassCoC.cpp`). (2) Quad passes bind the SCENE
sampler (repeat + mip + aniso), not `_postSampler` (clamp, no mip) which every
dedicated post pass used. Switching the quad path to `_postSampler` looks correct
and is probably what these effects want, but it also moves bloom downsample and
DOF output — so it was left alone rather than smuggled into a migration commit.

**Compose (2026-09-02)** was the sixth and largest: the whole post chain, six
input textures and a 44-field uniform block, now one implementation in
`scene/graphics/composeShaders.h`. A block of scalars plus one `vec2` packs
identically under MSL and std140, so both shaders declare the SAME field list —
the Vulkan shader's packed `p0..p10` vec4 accessors are gone. Metal output is
unchanged within run-to-run noise. Two dead bindings went with it: the Metal
shader declared `cocTexture`/`blurTexture` for a multi-pass DOF branch that has
been commented out for a long time (only `applyDofSinglePass` runs).
`MetalComposePass` SURVIVES as the shared fullscreen-quad geometry provider that
env-convolve/reproject, TAA and SSAO still borrow; it lost its shader and draw.

THREE things this uncovered, all fixed here. (1) Set 1 had no binding for slot 2,
so any quad effect using three or more textures was broken on Vulkan — which
silently included the DOF blur migrated earlier that day. (2) The set-1 slot list
was duplicated in THREE places (layout creation, binding loop, descriptor writes)
and the write path detected "is this the material set?" by matching its SIZE, so
adding a slot in two of the three wrote every binding to the wrong index. It is
now one `kMaterialTextureBindings` in `vulkanUniformLayouts.h`. (3) Quad passes
bound the SCENE sampler (repeat + mip + aniso) where every dedicated post pass
bound `_postSampler` (linear, clamp, no mip); with compose's CAS tapping outside
[0,1] this wrapped to the opposite edge and put wrong pixels along the frame
border. Quad passes now get `_postSampler`, which also corrects bloom's
downsample chain at its borders (its targets have no mips, so this is purely an
address-mode fix) and shifts bloom output slightly across the frame.

KNOWN, PRE-EXISTING: the Vulkan camera-frame/compose path renders washed out
compared to Metal — visible in `depth-of-field` and `shadow-cascades`, and
present before this migration (it is why the volumetric fog port could not be
pixel-validated). The GLSL was ported faithfully, so the bug came along with it;
it is now a one-shader fix instead of two.

**Debts cleared alongside (2026-09-02).** The CoC ramp now matches upstream's
`coc.js` — a dead zone of +/- focusRange/2 then a ramp over the FULL focusRange —
which also makes it agree with `applyDofSinglePass` in composeShaders.h, the one
place that already followed upstream. `vignetteColor` is exposed end to end
(`RenderingSettings` -> `CameraFrameOptions` -> `RenderPassCompose`); it had been
plumbed as far as the shader with no way to set it. `QuadRender` draws an
oversized fullscreen TRIANGLE rather than a two-triangle quad, matching what the
dedicated post passes always used (no diagonal seam, 3 verts).

### SSAO and TAA, which finished the category
**SSAO and TAA (2026-09-02)** finished the fullscreen-quad category. Both follow
the compose recipe — one implementation with MSL + GLSL selected by
`shaderLanguage()`, in `scene/graphics/ssaoShaders.h` and `taaShaders.h`. SSAO's
block keeps the explicit pads that make each `vec2` 8-byte aligned (MSL and
std140 agree there); TAA adopted the Vulkan packing of size-and-flags into one
`vec4`, so every member is a mat4 or vec4 and there is no padding to get wrong.

That emptied `PostPassKind`, so the entire Vulkan post-process framework
(`vulkanPostProcess.cpp`, 400 lines: the shared descriptor layout, the pipeline
cache, `executePostPass`) is deleted along with the last four `post_*.frag`
bundle modules. Metal lost `MetalSsaoPass` and `MetalTaaPass`. Combined with the
earlier migrations this pass removed ~1600 lines.

Verification: depth-of-field is pixel-identical; ambient-occlusion moves 147
pixels of 630k (visually identical, AO contact edges — the same small unexplained
delta the depth-aware blur showed, and NOT the triangle-vs-quad geometry, which
was tested and ruled out). The `taa` example cannot be pixel-verified at all: it
is temporally accumulative and moves 15% of pixels between two runs of the SAME
binary, so the check there is that it renders cleanly with no ghosting.

### Grabs and mips generalised off the vtable
**Grabs and mips generalised (2026-09-03).** `grabSceneColor`, `grabSceneDepth`
and `generateCubemapMips` were three effect-specific virtuals that each baked the
effect's policy into the backend: which texture to cache, how to resize it,
whether to mip it, which device slot to publish it to — written twice per backend
and near-identical within each. They are now two GENERIC operations, which is the
shape upstream's device has: `copyRenderTarget(source, colorDest, depthDest)` and
`generateMipmaps(texture)`. The policy moved up into `RenderPassColorGrab` and
`RenderPassDepthGrab`, which own their destination texture and share one
allocation helper (`scene/graphics/sceneGrab.h`). Metal lost its raw-MTLTexture +
external-wrapper machinery for the two grabs entirely.

GOTCHA that this uncovered: a blit needs the source and destination pixel formats
to MATCH, and copying from the back buffer had no Texture to read a format off —
the drawable is BGRA8 while the engine's only 32-bit unorm format was RGBA8. So
`PIXELFORMAT_BGRA8` now exists, both backends map it, and
`backBufferColorFormat()` / `backBufferDepthFormat()` report what the back buffer
actually is. The old code dodged this by allocating from the raw Metal format,
which a backend-agnostic pass cannot see.

## Gaussian splat sort direction under non-uniform scale (2026-09-03)
**Sort direction under non-uniform scale (fixed 2026-09-03).** The sorter's key is
`dot(localCentre, direction)`, so `direction` has to weight each local axis the way
the model matrix does. It was built by transforming the view direction by the
INVERSE model matrix and normalising, which weights axis i by 1/s_i where the true
depth weights it by s_i — the two cancel only under a UNIFORM scale.
`GSplatInstance::sortDirection` now weights each axis by the model matrix's own
basis vector projected on the view direction, exact for any affine transform
(upstream #9268). The error was a function of the VIEW DIRECTION, so a
non-uniformly scaled splat drew over splats in front of it at some camera angles
and not others, with distance making no difference — a splat flattened onto a
ground plane (scale 4, 0.2, 4) was off by metres. Covered by
`tests/gsplatSortTests.cpp`, which fits key against true depth over random points
and sweeps the camera; the old form scores 0.8 normalised error there and the new
one 1e-7.

## Frame graph store propagation (2026-08-21)
clearing it, marks the earlier pass on that target as having to STORE its results. Two bugs
lived there until 2026-08-21:

- **The back buffer was excluded.** The guard read `if (renderTarget != nullptr)` directly
  under a comment saying "or null which represents the default back-buffer" — so no
  back-buffer pass was ever told to store. Harmless while a frame had a single back-buffer
  pass, which is the normal case; the moment the scene-color grab split the frame into two,
  the second pass loaded a depth buffer the first had discarded. The skybox then passed the
  depth test everywhere and repainted over every opaque draw, so **all opaque geometry
  vanished** and only post-grab (transparent) draws survived. That silently broke
  `refraction-example` — its columns were missing from the frame and visible only through
  the refracting sphere — and blocked `requestSceneColorMap` for everyone else.
- **`_renderTargetMap` was never cleared**, so the first pass of each frame propagated
  stores onto the previous frame's pass (and kept it alive). It is per-frame state now.

Grab passes carry no color ops of their own and must not displace the real draw pass in that
map, or the pass that actually reads the surface would look at the grab instead.

## Omni shadow bias is relative, not absolute (2026-08-17)
**Omni shadow bias is RELATIVE** (fixed 2026-08-17): a fraction of the receiver
distance applied BEFORE the perspective projection, not an offset after it.
Cubemap shadow depth is crushed against 1.0 — with near 0.01 and far 30, half a
world unit of separation is 8e-5 of stored depth, so the old fixed 0.001
post-projection offset was more than ten times the gap it was meant to preserve
and erased omni shadows entirely at ordinary light ranges. Both backends now bias
`d` by 0.2% instead (`omniShadowParams[2]`).

## Vulkan feature mask reached the fragment stage only (2026-08-16)
**Vulkan specialization gotcha (fixed 2026-08-16):** the feature mask used to be
specialized into the FRAGMENT stage only, so every `vtFeatureEnabled(...)` in a
vertex shader read false — silently disabling Vulkan vertex displacement, and any
future vertex-stage feature. Both stages now get it in `vulkanRenderPipeline`.

## Single-sourcing the material uniform block (2026-08)
That was four hand-maintained copies plus two size checks that had to agree, where
a mismatch shifted every following field — silent corruption, not a compile error.
Adding a field is now one line. Verified by temporarily appending a probe field:
it appeared in the C++ struct, the generated GLSL and the size validator, and the
build stayed green; then reverted. Rendering is unchanged (clearcoat pixel-identical
on both backends; the generated GLSL declaration is textually identical to the
hand-written one it replaced).

The feature-index list got the same treatment. Indices are assigned from
declaration order rather than a hand-written bit column, which was verified by
temporarily growing the list to 66 entries: both sides moved to 3 words with no
hand edits and the Vulkan smoke test still passed.

## Lightmappers (2026-08-16 GPU, 2026-07-14 CPU)
Full original notes for both bakers, kept because the option lists and the
three easy-to-miss bake requirements are not written down anywhere else.
**GPU lightmapper** (`framework/lightmapper/gpuLightmapper.h/.cpp`, 2026-08-16): upstream's own mechanism — each target mesh is rendered **in UV space** (`VT_FEATURE_LIGHTMAP_BAKE`: the vertex stage writes clip position from UV1, the fragment stage outputs the diffuse LIGHT with no albedo), so occlusion comes from the existing shadow maps instead of rays. ~40 ms for the house scene versus ~12 s for the CPU baker. The bake rides the normal frame graph like `ReflectionProbe`: one camera per target with `Camera::setLightmapBakePass(true)`, its own render target, and a private layer holding just that mesh; `bake()` then `update()` after `Engine::render()`. THREE things it must do that are easy to miss: the bake pass forces `CULLFACE_NONE` (UV winding follows the unwrap, so half the charts would be culled), every scene light gets the bake layer ids appended for the duration (lights are filtered per layer, else only ambient bakes), and the mesh wears `MASK_BAKE` during the bake and `MASK_AFFECT_LIGHTMAPPED` after (upstream's scheme — bake lights carry `MASK_BAKE` so they cannot light the mesh again at runtime). DEVIATIONS: no AO virtual lights, no bounces, no BAKE_COLORDIR, no GPU dilate/denoise, and **no cast shadows yet** — the bake captures direct light and ambient only (the CPU baker's shadows are ray-traced and still work). The CPU `Lightmapper` stays as the quality reference (ray-traced AO + soft shadows).

**Lightmapper baker** (`framework/lightmapper/lightmapper.h/.cpp`, 2026-07-14): a **CPU** baker (upstream is a GPU UV-space renderer — DEVIATION). `addLight()` (directional/point/spot) + `addOccluder(mesh, worldTransform)` (world triangles for ray casting) + `bake(targetMesh, worldTransform, Options)` → RGBA8 texture (or `bakeAndApply(material, ...)`). Options mirror upstream's scene-level bake knobs: `sizeMultiplier`/`maxResolution` derive a per-mesh resolution from world bounds (upstream `calculateLightmapSize`), `ambientBake` + `ambientBakeNumSamples`/`SpherePart`/`OcclusionContrast`/`OcclusionBrightness` replace the flat AO term with rays distributed over the top part of the sphere shaped by upstream's `bakeLmEnd` curve, `filterEnabled`/`filterRange`/`filterSmoothness` run a bilateral denoise, and per-light `bakeNumSamples`/`bakeArea` give directional lights soft shadows (upstream spreads N virtual lights over the cone; the ray tracer jitters the shadow ray instead). Per target mesh it reads CPU vertex/index storage (`VertexBuffer::storage()` as 56-byte `PackedVertex`, uv1 at offset 48), rasterizes triangles in **UV1 space** (barycentric per texel → world pos+normal), and shades: direct lighting (Lambert × attenuation + spot cone) with **hard shadow rays**, cosine-weighted-hemisphere **ambient occlusion**, ambient+sky terms AO-modulated; then dilates seams and sRGB-encodes (the shader pow(2.2)-decodes). Ray any-hit uses a **median-split BVH** over occluder triangles; the expensive shading phase is multi-threaded (`std::thread::hardware_concurrency`). ~0.9 s for a 512² map with 24 AO samples over 4.6k triangles. DEVIATIONS: LDR RGBA8 only, single bounce (no GI), no color+dir directional lightmaps, no auto lightmap-size/UV-unwrap (uses the mesh's existing UV1 — box faces overlap, so bake receiver-only planes). Mask a lightmapped mesh out of realtime lights with `MeshInstance::setMask(MASK_AFFECT_LIGHTMAPPED)`. Example: `lightmap-bake-example.cpp` (floor baked with soft shadows + AO from occluder boxes/sphere; toggles the lightmap on/off). Test asset: `assets/textures/lightmap-pools.tga` (render-to-texture example ground).

## Dynamic reflection probe scene capture (2026-07-14)
**Dynamic scene-capture bake** (`framework/extras/reflectionProbe.h/.cpp`, 2026-07-14): `ReflectionProbe` renders the live scene into the probe cubemap at runtime instead of using a supplied/authored cube. It owns a mipmapped RGBA8 color cubemap + 6 face `RenderTarget`s + 6 `CameraComponent`s pointed along ±X/±Y/±Z (reusing `LightCamera::pointLightRotations`, 90° FOV, aspect 1). The six face cameras render as ordinary cameras in the normal frame graph (each `RenderTarget` targets one cube face via `RenderTargetOptions.face`), so **construct the probe BEFORE the main camera** (layer composition renders cameras in construction order → faces captured before the main camera samples the probe). Per frame `update()` (called AFTER `engine->render()`) runs `GraphicsDevice::generateCubemapMips` (blit `generateMipmaps` on its own command buffer) to rebuild the roughness mips from the freshly-rendered level-0 faces, and installs the cube via `setReflectionProbe` on the first call. Modes: `setDynamic(true)` re-captures every frame (reflections track the scene); `false` = one-shot then disable the face cameras. Two enabling engine changes: (1) `MetalGraphicsDevice::startRenderPass` now sets the **color** attachment slice from `activeTarget->face()` for cube textures (previously only the depth attachment did, for omni shadows); (2) `GraphicsDevice::generateCubemapMips`. Captured faces hold the normal tonemapped/gamma-encoded forward output, which the probe shader sRGB-decodes — matching the static path. DEVIATIONS: hardware-mip roughness (no GGX cube prefilter); the reflective object must sit on a layer excluded from the probe's capture layers or it self-captures (probe camera is at the probe center); probe faces miss directional-shadow cascades (fit only for the presentation camera). Example: `reflection-probe-dynamic-example.cpp` (chrome sphere on a probe-excluded layer reflects a ring of orbiting emissive boxes captured live — no env atlas/skybox, so the colored reflections come purely from the runtime capture).

## Vulkan compose-side gap closed (2026-09-04)

`depth-of-field` rendered 1.6x Metal and `post-processing` 1.65x, long after the
other scenes had come into line. Neither was a compose bug at all — measuring the
scene texture before compose showed the forward pass already 2.4x too bright, so
compose was faithfully carrying an error handed to it.

Two forward-pass divergences, found by probing one term at a time against the Metal
chunk and comparing at MATCHING points in each shader:

- **The emissive TEXTURE was never sRGB-decoded on Vulkan.** Metal does
  `emissive *= srgbToLinear(sample)`; Vulkan multiplied the raw sample in. The
  factor correctly stays undecoded on both, since `updateUniforms` pre-linearises
  `setEmissive`. This was the dominant term: the probe read (44.8, 46.2, 45.4) on
  Metal against (73.8, 76.0, 75.4) on Vulkan, and fixing it took `depth-of-field`
  from 1.57 to 1.00. It hid for so long because only that scene has large emissive
  surfaces.
- **The environment Fresnel was a hand-rolled Schlick-roughness variant** returning
  up to `(1 - roughness)` at grazing angles where Metal's gloss-aware `getFresnel`
  returns about F0. The identical helper already existed in `common-brdf.glsl` as
  `ssrFresnel`, used only for SSR. Swapping it brought the environment specular from
  (16.8, 19.6, 20.6) to (11.3, 13.0, 12.9) against Metal's (10.9, 12.8, 12.7).

Three earlier suspects were measured and CLEARED, so do not re-chase them: the
diffuse irradiance lookup matches to 0.1, the prefiltered environment sample matches
to 0.2, and ambient occlusion is not a factor in these scenes.

Method notes worth keeping. Probe both backends at equivalent points — my first
specular comparison read Metal post-occlusion against Vulkan pre-occlusion and was
meaningless. And whole-frame means mislead twice over here: the Metal-only MiniStats
overlay drags Metal's mean down (crop it out, which turns a false 1.11 on
`shadow-cascades` into a true 1.00), and a term that is right can be masked by a
larger term that is wrong.

Result: `depth-of-field`, `shadow-cascades` and `glb-loader` all at 1.00.
`post-processing` sits at 0.93, now slightly dark, and is the last outlier.

## Environment family: half off the vtable (2026-09-04)

Four device virtuals; two are now backend-agnostic code over QuadRender and two are
not. Net 593 lines deleted against 178 added, and the Metal pass classes for
equirect-to-cube are gone entirely.

**The seam.** Metal turned out to need nothing structural: its `startRenderPass`
already builds and commits a command buffer per pass, so it always worked outside
the frame loop and its old begin/endEnvBatch was only batching. Vulkan is the one
that cannot, because its command buffer, uniform ring and descriptor pools are all
frame-scoped. So `beginOfflineWork` / `endOfflineWork` replaced the Metal-only
batch pair: Vulkan opens a one-shot command buffer that the normal render path
records into via a new `currentCommandBuffer()`, then submits and waits. Waiting is
what makes reusing the frame-scoped resources safe, and the bakes run at load time
or between frames so the stall costs nothing.

**Migrated and verified:** equirect-to-cubemap and reproject. Both render correctly
on both backends in `reflection-probe-dynamic`, with zero validation errors. The
equirect migration also FIXED a Vulkan bug: that example's atlas preview panel was
black before and shows content now.

**Two traps found.** `beginOfflineWork` has to flush pending uploads first — a
texture created without host data records its SHADER_READ_ONLY transition through
the deferred upload queue while marking its tracker immediately, so submitting
offline work ahead of the flush put descriptors in flight against images still in
UNDEFINED. And a quad bake must set blend, depth and cull state itself; outside the
frame graph nothing else has, and the Metal pipeline cache asserts on a null blend
state.

**Not migrated: convolve and atlas.** The sample table (up to 1024 float4s, far
past the 512-byte per-draw block) does fit as an RGBA32F data texture on a quad
slot, and that worked. The atlas did not: after migrating it the backends disagreed
about the layout, Metal moving to the staircase that `EnvLighting::generateAtlas`
actually declares while Vulkan kept a full-width second row. Metal's new output
looked more correct than its old one, which is precisely why landing it on a hunch
was wrong — so it was reverted rather than left in, and the code removed rather
than left dead.

Also worth recording: `tools/generate-env-atlas` is not a usable baseline for this
work, contrary to an earlier note. It is deterministic but emits the same scattered
noise before and after any change. The `reflection-probe-dynamic` atlas panel,
cropped and magnified, is what shows the layout.

## Environment family fully off the vtable (2026-09-05)

The remaining two virtuals, `generateEnvConvolve` and `generateEnvAtlas`, are gone.
All four env bakes are now one implementation each over QuadRender inside a
`beginOfflineWork` scope. Both backends' pass classes and the entire
`vulkanEnvironment.cpp` are deleted: 1465 lines removed against 552 added.

The convolve sample table (up to 1024 float4s, far past the 512-byte per-draw
block) rides the existing quad texture seam as a 1-row RGBA32F data texture,
uploaded before the offline scope opens because that scope flushes uploads on entry.

**The previous attempt was reverted for the wrong reason.** I had concluded the
backends disagreed about the atlas layout, with Vulkan keeping a "full-width second
row". That reading was simply wrong: the GGX convolve rects occupy the left column
starting at y = size/2 while the reproject mip chain steps diagonally from
x = size/2, so a full-width second row is exactly right and Vulkan was correct.
Metal was the broken one. Two lessons: read the layout the caller declares before
judging which backend is wrong, and a hypothesis that survives only because it was
never checked against the source is not evidence.

**Two real bugs found on the way, one of them latent since the QuadRender seam
was built.**

- Metal's `submitPerDrawUniforms` reuses the previous ring offset when the material
  pointer is unchanged. A quad pass has no material, and nothing clears the bound
  material for an offline bake, so every quad draw after the first in a pass shared
  ONE uniform block. Invisible while a pass drew a single quad — which every
  migrated effect did — but the atlas draws a rect list in one pass, so the convolve
  draws read the reproject block and produced nothing. `draw()` now passes a null
  material for the uniform key whenever the quad block is in use.
- The MSL convolve shader sampled its data table through the quad path's linear
  sampler. Apple GPUs cannot linearly filter a 32-bit float format, so that returns
  nothing; it now uses an `access::read` texel fetch, matching the GLSL `texelFetch`.

**How the false lead was cleared.** A three-rect staircase drawn through the
already-landed `bakeReproject` — the atlas's own layout, in one pass — landed
identically on both backends. That took multi-rect viewports off the table in one
experiment and pointed straight at the convolve draws. It also showed Vulkan
leaves unwritten target regions as uninitialised garbage where Metal reads black,
which is why the atlas pass clears first.

Verified: the `reflection-probe-dynamic` atlas panel matches structurally on both
backends, all five reprojection panels render, no validation errors, both suites
green, and `depth-of-field` and `shadow-cascades` are unchanged at 1.00 against
Metal.

The offline seam's two backends stay deliberately asymmetric: Metal commits without
waiting, Vulkan waits. Vulkan waits because it must, to reuse frame-scoped
resources; Metal has no such need and that example re-bakes every frame, where a
stall would serialise CPU and GPU. An experiment adding the wait to Metal confirmed
it does not fix `tools/generate-env-atlas`, whose readback is broken for an
unrelated reason: it calls `getBytes` on a private-storage render target.

## Vulkan reflection probes fixed (2026-09-05)

Both probe examples rendered wrongly on Vulkan: the chrome ball lost its mirror and
every metallic surface washed out to pale pastel. The 2026-08 note recorded this as
dynamic-only, with capture and sampling each "verified OK in isolation" and the
combination as the suspect. Capturing both examples first showed that was wrong on
two counts — the STATIC probe example was equally broken, and the fault was five
plain divergences in the Vulkan probe block, all visible by reading it beside the
Metal one:

- no sRGB decode of the captured cube (it is gamma-encoded, like every other
  texture the shader reads) — this is the one that caused the pale wash;
- no cube-convention X flip, which every sky path here does;
- a raw `F0` multiply instead of a gloss-aware Fresnel;
- box projection re-aimed from the probe's POSITION, unnormalised, where it must be
  re-aimed from the box CENTRE;
- the probe ADDED to the accumulated colour while also overwriting
  `indirectSpecular`, so the environment specular was counted twice — it has to add
  only the difference, exactly as the SSR block below it already did.

Static probe, Metal against Vulkan: 149,760 of 630,000 pixels differed by more than
8 before, 23,615 after, and mean luma went from 23.2 vs 24.3 to 23.2 vs 23.2. The
residual is confined to pattern edges and grid lines.

Worth noting how cheap this was compared to the earlier attempt recorded in memory,
which "made it worse and was reverted": capture both examples, then read the two
shader blocks side by side. The earlier session had ruled out capture and sampling
by experiment and then guessed at the combination, without ever diffing the two
blocks — where all five differences were sitting in plain sight.

## GPU particle simulation off the vtable; effect migration complete (2026-09-05)

`simulateParticles` was the last effect on the `GraphicsDevice` vtable. It is now a
shared kernel in `scene/particles/particleSimShaders.h` dispatched through the
generic `Compute` seam, deleting the Metal kernel, the Vulkan `.comp`, both
backends' dispatch code and pipeline objects, and the bundle generator's entry for
it: 379 lines removed against 107 added.

The migration needed no binding changes at all. Both kernels already declared the
storage buffer at 0 and the parameter block at 1, which is exactly what Compute's
name-order contract produces for one buffer plus a uniform block, and the old
dispatch on both backends already used 256 threads per group with
`ceil(count / 256)` groups.

The one gap was that Compute could only build its uniform block from named scalars,
packed in NAME order. Expressing `GpuParticleSimParams` — a mat4 and seven vec4s —
as 44 named floats would have been unreadable and fragile, so `Compute` gained
`setUniformBlock`, which supplies the block verbatim at the same binding. The two
paths are mutually exclusive and a scalar setter clears the raw block.

**What was left alone, deliberately.** `setParticleState`, `setGSplatState` and
`setMorphState` are still virtual and should stay that way: they bind per-draw
resources at fixed slots, which is the device's job, and folding three calls that
differ in arity and slot ownership into one tagged call would read worse. That is
now stated at the declarations rather than left to be rediscovered.

**A finding that made verification awkward.** Wiring a `ParticleSystemComponent`
into a scene as a probe rendered nothing and dispatched nothing. I first wrote that
down as if the system were unconstructable, which was wrong: component systems come
from `AppOptions::componentSystems`, so `ParticleSystemComponentSystem` was inert
only because no application had ever asked for it. The correct statement is that
this was a coverage gap, and the fix is one line in an example. The Vulkan smoke
test now also drives `ParticleEmitter::update` directly and reports 512 particles
simulated with validation clean.

Metal was verified by construction rather than by execution at the time of the
migration: the kernel, bindings, threadgroup size and dispatch count are unchanged,
and Compute/computeDispatch is already the Metal path the compute-particles example
uses. The example below closed that gap the next day.

## An example for the particle system component (2026-09-05)

`particle-system-example.cpp` is the component's first consumer since
`particles-example` was rewritten as upstream's `compute/particles` in August. It
runs two emitters: an additive box fountain that plays the 4x4 numbered sprite sheet
once per particle life, and an alpha-blended sphere puff damped to 0.8 with a
warm-to-cool colour graph. Between them they cover both emitter shapes, both blend
modes, the sprite-sheet animation, and all three curve LUTs.

The one thing the example must do that no other example does is register the
component system:

```cpp
void configure(AppOptions& options) override
{
    options.registerComponentSystem<ParticleSystemComponentSystem>();
}
```

Both backends render it. Captured at the same frame the two disagreed, which was
purely warm-up: Vulkan reaches its first frame later, so its particles were younger,
smaller and still on the warm end of the colour graph. Captured deeper into the run
the two agree. Any future comparison of this example has to be made at a settled
frame, not a fixed one — the scene is a continuous simulation and never repeats.

## The last brightness outlier was compose, not lighting (2026-09-05)

`post-processing` had been sitting at 0.92-0.94x Metal since the environment-Fresnel
alignment, recorded as a lighting divergence to be chased term by term against the
Metal shading chunks. It was not lighting. Turning the whole compose chain off made
the two backends agree to 1.004 — the sky, which never touches the lighting code,
matched to 0.9999 — so the forward pass was never in question.

**Method.** The scene animates and runs TAA, which is why it had resisted a clean
measurement. I temporarily gave the example a probe switch that pins its clock to a
fixed time and strips any named term, then bisected the compose chain. The first
pass was misleading: enabling grading, colour enhance, fringing or sharpness ALONE
changed nothing at all, because none of them turns the camera frame on by itself.
With vignette held on as an anchor the answer came out immediately — vignette plus
colour enhance measured 0.948, against 1.004 for vignette alone.

**Cause.** The GLSL half of the compose shader was never brought in line with the
MSL half. Three stages were older, cruder implementations that happen to share the
uniform block but not the maths:

- **Colour enhance** computed saturation as `max - min` rather than
  `(max - min) / max`, and mixed toward `vec3(luma)` rather than toward a grey
  normalised against the largest channel. Both are SDR forms. In the HDR values
  compose actually operates on, saturation exceeds 1 and the vibrance factor goes
  negative, so bright pixels were pulled toward black. That alone was the 7%.
- **Colour grading** applied its terms in a different order with different luma
  weights and no HDR normalisation of the grey.
- **The 3D LUT** used the linear colour as the lookup coordinate with no sRGB
  encode and no decode of the sample, and chained the second LUT onto the first
  LUT's output instead of blending both against the same input.

All three are now translations of the Metal functions, term for term.

**Verification.** `post-processing` moves from 0.922/0.926/0.938 to
1.004/1.008/1.014, which is exactly the residual the scene shows with compose
disabled entirely. Its sky matches to 0.9999 and the colour-enhance isolate lands
back on the no-enhance baseline. `depth-of-field`, which uses none of the three
stages, is unchanged at 1.006/1.003/0.999. Both suites green.

**What this unmasked.** `ambient-occlusion-davinci` is the only other scene that
uses these stages, and it now reads 1.21 where it read 1.15 — the wrong colour
enhance had been darkening Vulkan and cancelling part of a larger divergence, the
same shape as the Fresnel alignment before it. That scene's gap is in the SKY
(1.44 in the upper right against 0.96-0.97 on lit geometry), and the one thing
separating it from every scene whose sky matches is that it asks for skybox mip 2.
Recorded as its own open item.

**The lesson worth keeping.** The old note told the next session to probe lighting
terms against the Metal chunks, and that would have been days of work in the wrong
file. Two shader bodies in one header are shared by convention only; unifying a
uniform block does not unify the code. Read both bodies before designing an
experiment — the same mistake the reflection-probe entry above records.

## The Vulkan sky was mirrored (2026-09-05)

The `ambient-occlusion-davinci` scene, which the compose fix above had just
unmasked at 1.21x Metal, was not a mip problem and not a compose problem. The
Vulkan env-atlas sky was an exact left-right FLIP of Metal's.

The Vulkan sky path sampled the atlas with the raw view direction. Every other
env-atlas lookup in that backend already negates X — the diffuse and specular IBL
directions in `forward-fragment-ambient`, and the cubemap branch two lines above
the atlas branch in the same function — because that is the engine's atlas
handedness. Only the sky was missing it. One line.

**Why it survived this long.** A mean over a horizontally symmetric region is
invariant under a mirror. `post-processing`'s sky measured 0.9999 across the full
width and was written off as agreeing exactly; split into halves it reads 1.111 on
the left and 0.879 on the right. The davinci scene only stood out because its sky
is strongly asymmetric, with the sun's warm lobe near one edge.

**How it was pinned down.** The row profiles gave it away — Metal at x=128 read
144 where Vulkan at x=896 read 144, and so on inward — and a direct check
confirmed it: mean absolute difference of the sky region is 22.3 compared
straight, and 0.00 compared mirrored. Zero, not small.

Two false starts are worth recording. The first mirror test, on the davinci
capture, FAILED, because the mirror of a sky box there lands on walls and objects;
I moved on instead of retrying it on a scene that is sky from edge to edge. And
the mip hypothesis in the open item was disproved in one run by forcing the
example to mip 1, which left the divergence untouched.

**Verification.** The davinci sky matches Metal to 1.0000 in both corners and the
frame goes from 1.21 to 0.978. `post-processing`'s sky halves land at 0.998 and
1.001. `clearcoat` measures 1.0000 and 0.9995 on its sky halves and 0.983 overall.
`depth-of-field` improves from 1.006/1.003/0.999 to 1.001/1.001/1.000. Both suites
green.

Every Vulkan scene with an env-atlas sky was affected, and so was every dynamic
reflection probe, since a probe captures the sky through this same path.

## Re-measuring the probe residual, and a double (1 - metallic) (2026-09-05)

With the mirrored sky fixed, the standing claim that "dynamic reflection probes
light wrongly on Vulkan" needed re-testing. It does not hold.

**Freezing the scene first.** `reflection-probe-dynamic` orbits the camera AND
spins its objects with an incremental `rotate`, so capturing both backends at the
same frame number compares two different scenes — the earlier 0.98/0.93/0.97 was
measuring the animation, not the backends. Pinning the clock and skipping the
incremental spin is what made the numbers mean anything.

**What the probe does now.** The captured cube's sky matches Metal at 1.0000, and
so does the main view's background. The panels that reproject the captured cube
sit at 0.97, and splitting one of them shows why: its sky band is 0.999 and its
ground band is 0.82. So the capture is faithful and simply carries whatever the
forward pass does — the probe path is not the problem. `DEBUGPASS_ALBEDO` matches
at exactly 1.0000, `DEBUGPASS_LIGHTING` at 0.97/0.97/0.95, which places the whole
residual in lighting.

**The defect that found.** The Vulkan env-atlas branch scaled its irradiance by
`kD = (1 - Fr) * (1 - metallic)` before multiplying by `diffuseAlbedo`, which
already carries `(1 - metallic)`. That is the same double count fixed on the
direct-light path earlier, still present on the indirect one, and it is a 3.3x
deficit at metalness 0.7. Every other branch in the same function — light probes,
flat ambient, lightmap — multiplies the irradiance by `diffuseAlbedo` alone, and
so does the Metal chunk, which applies no `(1 - Fr)` to indirect diffuse at all.
The env-atlas branch was the lone outlier.

**Verification, including where it is imperfect.** The dynamic-probe ground moves
from 0.949/0.927/0.956 to 1.034/1.054/1.057 and the whole frame from
0.978/0.973/0.974 to 1.006/1.012/1.012. `clearcoat` improves marginally,
0.983 to 0.984. `reflection-probe` is unmoved, since its ball reads a probe rather
than the atlas. `depth-of-field` goes the wrong way by 0.6%, from 1.0006 to
1.0061.

That last number is worth stating plainly rather than burying: the change is
justified by inspection, not by every measurement improving. `diffuseAlbedo`
demonstrably contains `(1 - metallic)` and `kD` multiplied by it again — there is
no reading under which that is correct — so the sign flip on the dynamic-probe
ground (7% dark to 5% bright) means a SECOND, blue-weighted term is off by about
10% and the old bug was cancelling part of it. That is now the open item, and it
is the same shape as the two divergences before it: an error that measured well
because another error was pulling the other way.

## Ground planes read their irradiance from the wrong atlas rect (2026-09-05)

The blue-weighted term the previous fix exposed was not a Vulkan bug. It was a
Metal one, and it had been mis-lighting every unrotated ground plane in the engine.

**The bisect.** The scene's directional light is pure YELLOW, so blue is a clean
readout of indirect light alone. With the light off the ground read 1.05/1.10/1.06
on Vulkan; at metalness 1, where the diffuse term drops out, the two backends
matched at 1.0006; at metalness 0 the gap opened to 1.12/1.19/1.12. That put the
whole divergence in the diffuse irradiance and nowhere else.

**Making the shader show its work.** Both chunks were temporarily made to return
the raw irradiance instead of shading. The curved objects came back byte-identical
and the ground plane came back dark navy on Metal against sky blue on Vulkan — a
plane facing straight up cannot receive a dark navy irradiance, so this was a
correctness bug, not a parity one. Returning the lookup UV instead named it: on
the ground Metal produced u = 0.00 where Vulkan produced 0.314, and the ambient
rect only spans 0.252 to 0.373.

**Cause.** `atan2(0, 0)` is undefined, and a direction of exactly +/-Y hits it.
That is every fragment of an unrotated ground plane. Metal returned an
out-of-range azimuth, `mapUv` mapped outside the ambient rect, and the sample
landed in the ROUGHNESS column further down the atlas — which is why the wrong
value still looked like a plausible blurry environment colour rather than obvious
garbage. GLSL's `atan` happens to return 0 for the same input, so Vulkan was
right by luck. Both now pick azimuth 0 at the pole explicitly.

**Verification.** The frozen `reflection-probe-dynamic` ground goes from
1.034/1.054/1.057 to 0.998/0.998/1.000 and the whole frame to 0.999/0.999/1.000;
its diffuse-IBL-only isolate lands at 1.000. `reflection-probe` improves to
0.9997/1.0013/1.0006. `clearcoat` and `depth-of-field` do not move at all on
Metal, which is the expected signature — neither has a surface whose normal is
exactly axis-aligned. Both suites green.

**What it unmasked, again.** Metal's floor in `ambient-occlusion-davinci`
brightened 2-5% and Vulkan's stayed put, so that scene's floor now reads 0.95.
That is the fourth divergence in this chain and the third time this particular
scene has been the one holding the next one. The pattern is now well established
enough to state as a rule: a scene that measures well is as likely to hold two
errors that cancel as none.

## The davinci floor is SSAO, and one hypothesis that did not survive (2026-09-05)

The floor gap turned out to be ambient occlusion, not albedo and not lighting.
Nothing was landed this round: the characterisation is the result, and the one
fix attempted was reverted.

**Where it is.** Stripping the scene one term at a time on a pure floor patch:

    SSAO on,  blur on     0.808
    SSAO on,  blur off    0.926
    SSAO off              0.976

So Vulkan occludes about 5% more than Metal, and the bilateral blur multiplies
that difference by roughly 2.5 rather than smoothing it away — which is what a
bilateral filter does with a disagreement at a depth discontinuity, and the same
amplification the depth-aware blur showed during its migration. The residual
0.976 is a normal-mapped surface reading the `normalScale` difference the two
backends keep on purpose.

**Where it is not.** The SSAO and depth-aware-blur shader bodies are textually
equivalent between MSL and GLSL, helper for helper: same noise function, same
face normal, same view-space reconstruction, same weights. Unlike compose, there
is no second implementation hiding in the GLSL half. That points the next attempt
at the pass INPUTS rather than its maths.

**The hypothesis that failed, and why it is recorded.** Depth reconstruction is
sensitive to filtering: a bilinear tap straddling a silhouette returns a depth
belonging to neither surface, which the kernel then treats as an occluder. Vulkan
decides per format whether linear filtering is available and falls back to
nearest, so I took Vulkan for nearest and Metal for bilinear, and made the Metal
SSAO and blur point-sample depth. The floor moved from 0.808 to 1.115 and the
frame from 0.961 to 1.011 — both closer to 1 in magnitude, and both with the sign
flipped. Then checking what Vulkan actually creates showed `VK_FILTER_LINEAR` on
the samplers those passes use, so the premise was wrong and the improvement was
coincidental. Reverted.

That is worth stating because the numbers alone would have justified keeping it.
The previous round landed a change that also overshot, on the strength of a proof
by inspection — a double `(1 - metallic)` that no reading makes correct. Here
there was no such proof, only a number that got smaller. The difference between
those two cases is the whole standard.

**Next step for whoever picks this up:** establish what filter each backend
actually applies to the SSAO depth tap, at runtime rather than by reading the
sampler-creation code, since the quad path and the per-texture path do not
necessarily use the same sampler.

## Parallax occlusion mapping, the 2.22 additions (2026-09-05)

The port already marched a height field; what 2.22 added was a reference plane, a
self-shadow pass and a new default. All three landed on both backends.

**`heightMapBase`, and the mistake worth keeping.** The base is the texel value
that reads as the original surface, so a non-zero base lifts part of the field
above the polygon. My first implementation offset the sampled depths by the base
and started the march at `-base` — which is a pure shift of both the ray and the
field, and produced images that were bit-identical to base 0. The controlled A/B
caught it: the quad under test matched its own no-base render exactly, where a
working parameter should have moved it. The fix is that the ray's ENTRY UV has to
move too, by the lateral distance the ray covers while descending from the base
plane to the polygon. After that the same A/B reports a mean absolute difference
of 31.2 levels on the quad under test and 0.00 on its neighbour.

**Self-shadowing.** A second march from the displaced point toward the light,
folded into the directional light's attenuation. Only that light pays for it;
giving every local light its own march is not worth the cost, and is recorded as a
deviation rather than left to be discovered. The shadow march samples at an
explicit LOD because it sits inside the light loop, behind fragment-varying
control flow, where derivatives are undefined; the view march stays on implicit
LOD so the height map keeps its mips. Verified the same way: 6.8 levels mean
absolute difference on the quad it applies to, 0.00 on its neighbour.

My first scaling for it multiplied the accumulated occlusion by the step count,
which drove the quad to 0.59 of its unshadowed brightness — the occlusion term is
already a fraction of the marched range, so the extra factor of 16 saturated it.
A factor of 2 gives 0.94, which reads as a shadow rather than as a stain.

**The breaking default.** `heightMapFactor` went from 0.05 to 0.1, matching
upstream's change from 0.025. Nothing in the repository relied on it: no example
set a height map before this one, and the glTF parser does not bind one at all.

**Verification.** The new `parallax-example` renders 0.9995/0.9996/0.9997 Vulkan
against Metal, which is the run-to-run floor for this harness. `clearcoat` is
byte-identical on Metal before and after, confirming the extra locals the surface
chunk now declares cost nothing when no height map is bound. Both suites green.

## The SSAO depth tap: measuring the filter instead of guessing it (2026-09-05)

The previous round guessed which backend point-sampled depth, guessed wrong, and
reverted. This round made the shader report it.

**The probe.** Sample the raw depth at a texel centre, one texel across, and
exactly halfway between. Under linear filtering the halfway tap is the average of
the two ends; under nearest it equals one of them. The SSAO pass was temporarily
made to output that verdict as its occlusion value — one shade for "no depth step
here to tell from", one for linear, one for nearest — with the bilateral blur
bypassed so the answer survived to the frame. Classifying the result against a
normal render of the same scene:

    Metal    of stepped pixels:  100% linear,   0% nearest
    Vulkan   of stepped pixels:   64% linear,  36% nearest

So the two backends genuinely disagree, and it is VULKAN that point-samples part
of the time — the opposite of last round's assumption, which is why making Metal
nearest overshot: it aligned Metal to a behaviour Vulkan only has a third of the
time.

**The fix.** Make both deterministic rather than trying to make both linear, since
whether hardware filters a depth format at all is a per-format capability and not
something a sampler flag settles. Point sampling is also the defensible choice on
its own terms: a bilinear tap across a silhouette is a position in mid-air. Vulkan
now binds the existing shadow sampler (nearest, clamp-to-edge, mip-less) for any
depth texture in the quad path, and the MSL SSAO and depth-aware-blur passes
declare their own point sampler.

**Verification.**

    davinci frame   0.961 / 0.970 / 0.972  ->  0.986 / 0.992 / 0.995
    davinci floor   0.808 / 0.853 / 0.895  ->  0.918 / 0.959 / 0.985
    davinci sky     1.000 (unchanged)

Both moved toward 1 with no sign flip, which is what distinguishes this from the
reverted attempt. `depth-of-field` and `clearcoat` are byte-identical on Metal and
their parity numbers are unchanged to four decimals — neither runs SSAO, and the
grabbed depth copy the compose DOF reads is not a depth-format texture, so the
Vulkan branch does not touch it. Both suites green.

**What is left.** The floor still reads 0.92 in red. Some of that is the
per-backend `normalScale` difference on a normal-mapped surface, which the project
keeps on purpose; the rest is unattributed. Worth remembering that the bilateral
blur multiplies any SSAO input disagreement by about 2.5, so 3% of input error
presents as 8% on screen.

## Physics: a seam, and Jolt behind it (2026-09-05)

`RigidBodyComponent` was a raycast-only stub: it held a type, answered which
collision component it sat next to, and nothing moved. It now drives a real
simulation, and the engine still owns none of it.

**The shape of it.** `framework/physics/physicsWorld.h` declares `PhysicsWorld`,
`PhysicsBody` and a body description; an application supplies an implementation
through `AppOptions::physicsWorld`, exactly the way it supplies component systems.
`createJoltPhysicsWorld()` provides the Jolt-backed one, compiled in behind
`VISUTWIN_PHYSICS_JOLT` and the `jolt` vcpkg feature. Nothing in the engine
constructs a world.

With no world supplied the old behaviour survives intact: the component holds
settings and the raycasts fall back to the CPU sweep over collision bounds. The
`raycast` example still runs on that path and measures 1.0007 across backends,
unchanged.

**The bug that cost the most time, and what it taught.** The first working build
rendered a pyramid that never moved — two captures 190 frames apart were
byte-identical. `RigidBodyComponentSystem` read `engine->physicsWorld()` in its
constructor, and `Engine::init` stored the world AFTER building component systems,
so the world was null forever. The ordering is now correct, and the system also
resolves the world lazily on its first update so the ordering is no longer
load-bearing. A seam that silently does nothing is worse than one that fails: a
frozen scene looks like a physics bug, not a plumbing one.

**Verification.** A new `physics-world` test drives the backend directly and
checks the properties a stub silently fails: half a second of free fall matches
0.5*g*t^2 within 0.2 units, a sphere comes to rest exactly at its radius above the
floor rather than through it, a raycast reports the top of that sphere with an
upward normal, `raycastAll` returns nearest-first and finds both the sphere and
the ground, an impulse lifts a sleeping body, restitution rebounds, and a
kinematic body ignores gravity. It also creates and destroys eight worlds in a row,
since the backend spins up a job system per world and a teardown that races its
own workers would show up as a crash on exit rather than a test failure. Seven of
seven tests pass, and the Vulkan smoke test is clean.

The `physics` example runs on both backends: a pyramid of boxes on a static floor,
with an auto-demo that drops a heavy sphere every two seconds and rebuilds the
stack once it has been knocked apart.

**One thing found and NOT fixed.** Every Vulkan example, physics or not, throws
`mutex lock failed` when killed with SIGTERM; `clearcoat` and `parallax` do it
too, so it predates this work and is a shutdown-under-kill path rather than
anything physics touched. Recorded here rather than chased.

**Not ported: joints.** Upstream's `PhysicsJoint` and `JointComponent` are the
obvious follow-up and have no equivalent yet.

## Joints, and a correction about examples (2026-09-05)

The physics seam gained constraints, and the examples around it were rebuilt as
ports rather than inventions.

**The instruction that changed the design mid-task.** The user asked that examples
be ports of upstream's, not scenes invented here. Reading upstream's
`physics/joints.example.mjs` immediately invalidated the joint API I had started
building. I had put the joint ON one of the bodies with an anchor offset. Upstream
puts it on its OWN entity, whose transform IS the joint frame, with the local X
axis as the primary one, and names the two ends as `entityA` / `entityB`. That is
a better model — a joint frame is a pose, not an offset — and it is now what the
seam and the component expose. The five types follow upstream too: fixed, ball,
hinge, slider, six-degree-of-freedom.

**Three bugs the unit test caught that a screenshot would not have.**

1. *A hinge test that could not have passed.* My first arm was centred ON the
   rotation axis, where no rotation moves it. The test was wrong, not the engine —
   worth recording because the failure looked exactly like a broken hinge.
2. *Constraints were built in the wrong body order.* A slider told to run at
   +1.5 m/s travelled at -1.5. The backend measures angle and travel from body 1
   toward body 2, so the ANCHOR has to be body 1 for a positive motor speed to
   move end A the way the caller means. Every constraint is now created as (B, A).
3. *Two body locks cannot be taken separately.* Jolt asserts on the deadlock risk,
   and with no assert handler installed that arrived as a bare SIGTRAP with
   nothing on stderr. A handler is now installed, and the locks go through
   `BodyLockMultiWrite`.

**Verification.** The physics test now also covers joints: a fixed joint holds its
two bodies 0.7 apart under gravity, a world-pinned hinge keeps its arm at a
constant radius from the pivot while letting it swing down, a motorised slider
drives its platform along the axis and reverses when the motor does, a weld with a
1 N s threshold under a 500 kg load breaks and drops its body, and destroying a
body takes its joints with it rather than leaving a dangling constraint for the
solver to walk. Seven of seven tests pass; the Vulkan smoke test is clean.

**Examples.** `physics-joints` is a port of upstream's scene — hinged door,
motorised windmill, ball-joint chain, patrolling slider, sprung crate, breakable
tower — at upstream's poses and parameters. The pyramid scene I had invented for
the previous commit is gone, replaced by a port of `physics/falling-shapes`. Both
run on Metal and Vulkan.

DEVIATIONS recorded in the file headers: falling-shapes drops four primitives
rather than five, because upstream's torus needs a mesh collision volume this port
does not have; and the joints example fires its ball along the camera's forward
axis, because there is no `screenToWorld` here.

**Debt this leaves.** Two examples written earlier today — `parallax` and
`particle-system` — were invented rather than ported, and upstream has
counterparts for the second (`graphics/particles-spark`, `particles-snow`,
`particles-anim-index`). They should be revisited.

## Repaying the examples debt (2026-09-05)

Two examples written earlier the same day were invented rather than ported. Both
now have upstream counterparts behind them, and porting them found two real errors
in the parallax work they were supposed to demonstrate.

**`materials/parallax-mapping`.** A closed brick room and a brick sphere under a
warm spot and a cool omni. Porting it corrected the parallax API twice:

1. *The base convention was inverted.* Upstream's own comment states it plainly —
   the base is "the height map value that sits at the level of the geometry", so 1
   treats the map as pure depth below the surface and the engine default pivots
   around mid-grey. Mine had 0 meaning pure depth. The shader now computes
   `base - height` and the default is 0.5.
2. *The height unit was wrong by ten.* Upstream's factor is in TENTHS of a uv
   tile, so its tuned value of 0.4 asks for a relief 0.04 uv deep. Used raw it
   smeared the brickwork into spikes — a picture that is unmistakable once you see
   it, and that no amount of reading the source would have produced.

Neither error was visible in the scene I had invented, because I chose the
parameters to suit whatever the code happened to do. That is the argument for
porting rather than inventing, and it is now a rule in `AGENTS.md`.

**`graphics/particles-anim-index`.** Four emitters sharing one 4x4 sprite sheet,
one `animIndex` each. The port needed `animIndex` itself, which the emitter did
not have: each animation is `animNumFrames` tiles long and they run in reading
order, so the index simply offsets the frame. One option, one line in each of the
two particle shaders, and the four corners come out in four different colours.

The brick texture set is upstream's Bricks076A, transcoded from WebP to PNG since
this port's standalone texture loader is stb and has no WebP path.

DEVIATIONS recorded in the file headers: the parallax port drops upstream's
roughness map for a constant gloss, because this port's gloss map is a Metal-only
scalar map with no tiling of its own and both backends have to render the same
scene; its sphere is a primitive rather than a 128-band generated mesh; and the
particle port leaves out upstream's screen-space reference panel.

Vulkan renders the parallax room at 0.93-0.97 of Metal. That scene is heavily
normal-mapped and the two backends interpret `normalScale` differently on purpose,
so this is expected rather than new.

## A wide line renderer (2026-09-05)

The audit called this the best value-to-effort item among the feature gaps, and a
scientific-visualization engine without per-point line colour and width is missing
a primitive rather than a nicety: a hardware line is one pixel wide and takes
neither.

**What it is.** `WideLine` holds a polyline as packed position, colour and width
arrays. `WideLineRenderer` draws every line it owns in ONE instanced draw, one
instance per segment, expanded to a quad plus caps and joins in the vertex shader.
Because everything that varies rides in the per-segment record, lines of different
widths, colours, caps, joins and dash patterns stay in the same batch.

**What made it cheap.** The storage-draw seam added for `compute/particles` in
August is exactly the shape this needs: an app-owned buffer, one instance per
record, and a params block, with a ShaderMaterial carrying both languages. No new
device API, no custom instancing vertex format — which was the thing I expected to
block it, since this engine's instancing layouts are fixed 64- and 80-byte strides.

The template geometry comes from the VERTEX ID rather than a vertex buffer: a quad
for the body, two 16-triangle discs for round caps and joins, and two bevel
triangles. Every instance draws the same vertex count, and a piece the current
style does not want collapses to zero size rather than being skipped, which is
upstream's own trick and is what keeps it to a single draw.

**Verification.** The example is a port of upstream `graphics/wide-line`: 96 points
on a sine wave, width ramping 4 to 18 pixels, colour cyan to pink, round caps and
joins. Metal and Vulkan render it BYTE-IDENTICAL, which is worth noting — a
screen-space expansion driven entirely by one shared shader has nothing left to
diverge on. Both suites green.

One small engine addition fell out of it: `ExampleApp` now exposes its window size,
because a screen-space width has to be told the render target's pixel dimensions
before the first frame.

**Not ported.** Upstream's controls panel exposes every setting live; the example
cycles caps and joins and toggles dashes from the keyboard instead.

## Morph coverage restored, and a sphere that was inside out (2026-09-05)

`graphics/mesh-morph` ported: three spheres, each carrying three morph targets built
by extruding the part of the sphere nearest an axis plane along its own normals,
with the weights driven by three sine curves. It builds the targets on the CPU
rather than loading them, so it covers `MorphTarget` as well as the blend, and it
restores coverage that was deleted in August.

**What it found.** The spheres rendered black. The morph path was not to blame —
disabling the morph instance changed nothing, and a built-in primitive sphere added
to the same scene with the same material lit correctly. `DEBUGPASS_WORLDNORMAL`
settled it in one frame: the control sphere is blue in the middle, where a surface
faces the camera, and mine was not. The triangle winding in the sphere generator
was inverted, so its normals faced inward.

That generator was copied from `reflection-probe-dynamic-example.cpp`, which has
carried the same inverted winding since it was written. A mirror ball hides it —
an inside-out normal still reflects SOMETHING — which is exactly why it survived.
Both are fixed.

This is the third time in two days that porting an upstream example has surfaced a
defect that the invented scene it replaced could not: the parallax port found an
inverted base convention and a height unit wrong by ten, and this one found a mesh
that has been inside out for months.

**Verification.** Metal and Vulkan render the example identically (1.0000 on all
three channels). `reflection-probe-dynamic` still renders after the winding fix.
Both suites green.

**Not done.** Two of the three coverage gaps remain: gaussian splat spherical
harmonic bands and clustered atlas shadows. Upstream has
`graphics/clustered-spot-shadows` and `clustered-omni-shadows` for the second.

## Clustered spot shadows: the example lands, the feature does not (2026-09-05)

`graphics/clustered-spot-shadows` ported: ten cookie-projecting spot lights circling
a field of tumbled cubes, every one of them a shadow caster, on the clustered path.
Nothing had ever driven the local shadow atlas before, and the example found two
problems rather than demonstrating a working feature.

**What works.** Clustered lighting itself: ten coloured spots light the scene, their
pools land where the lights point, and the cluster grid handles them. The scene-level
settings upstream writes to `scene.lighting` are now reachable here too — the cell
grid, the per-cell light budget, and the atlas's per-slice resolution and capacity —
which the port needed and which did not exist before.

**What does not.** No shadows appear, which is the entire point of the example. And
with ten shadow-casting spots the scene INTERMITTENTLY produces no frames at all:
the first run captured a screenshot at frame 120, later runs of the same binary hang
after startup with nothing rendered. Reverting my renderer and atlas changes does not
help, so it is neither, and it is a race rather than a configuration.

I did not root-cause either, and I want to be plain about the state rather than leave
a green-looking commit: the example is correct and the feature under it is not. The
next step is `LightTextureAtlas::allocate` and whether `Light::atlasSlice()` ever
comes back >= 0 for these lights, since the renderer's clustered branch drops the
shadow entirely when it does not.

**One thing I tried and backed out.** Making `configure` apply live — dropping the
array texture and rebuilding it when the requested size changes — hangs the renderer
outright, presumably because the ShadowMaps already wrapped around the old slices
survive the reset. The setting therefore only applies before the atlas is first
created, and says so at the declaration.

That this port found a broken subsystem is the fourth time in two days that porting
an upstream example rather than inventing one has surfaced something real.

## Clustered spot shadows: half fixed, and one claim retracted (2026-09-05)

**The retraction first.** I reported this scene as intermittently hanging. It does
not. A sample of the "hung" process showed the main thread parked in
`nextDrawable`, which is ordinary back-pressure, and the scene in fact renders 240
frames in 5 seconds. The slow runs were a leftover example process of my own
competing for the GPU. There was no hang and there is no race; I should have taken
the sample before writing it down.

**The real defect, found and fixed.** Only two of the ten spot lights ever had a
chance of casting a shadow. Local shadow-casting lights are allocated a slot in the
bounded main shadow array, and when that runs out — `kMaxLocalShadows` is two — the
allocation clears `castShadows` so the shader will not sample a map that is not
there. Correct for the non-clustered path. Wrong for a clustered spot, whose shadow
comes from the atlas instead, and which therefore neither needs one of those two
slots nor should lose its shadow when they are gone.

The probe made it unambiguous: before the fix exactly two lights reached the
renderer's clustered-shadow branch, with slices 0 and 1; after it, all ten do, with
slices 0 through 9 and a shadow matrix each. Whatever the atlas capacity said, the
feature was capped at two.

**What is still wrong.** No shadow appears, even now that every light arrives with a
valid slice. The CPU half is correct and the GPU half is not: the remaining
candidates are whether the shadow passes actually render depth into the atlas
slices, whether `clusterShadowAtlas` is bound for the draw, and whether the shader's
in-range test on the projected coordinate passes. That is where the next session
should start, and it now has a scene that reproduces it in five seconds.

`clustered-lighting` still renders, both suites are green, and the Vulkan smoke test
is clean.

## Clustered spot shadows: four suspects eliminated, one left (2026-09-05)

No fix this round. What there is instead is a much smaller search space, arrived at
by making the shader and the renderer report rather than by reading them.

**Eliminated, in order.**

1. *The shader's in-range test.* A probe multiplying the light by 0.5 where the
   shadow branch runs and again by 0.5 where the projected coordinate is in range
   darkened the whole scene twice over. Both fire.
2. *Passes not being built.* A counter in the local shadow pass builder reports ten
   of ten built, every frame, each with a valid atlas slice.
3. *Passes drawing nothing.* The same probe inside the pass reports 9 to 14 casters
   drawn per slice out of 99 candidates, which is what the light cones should see.
4. *The atlas not being bound.* `setClusterShadowAtlas` is called with the array
   texture each frame, and `bindCached` puts it at Metal slot 26.

**What is left.** A raw `sample()` of the atlas — the stored depth, not the
comparison — reads at or above 0.999 everywhere. That is the clear value, from a
texture that was demonstrably written. So the texture the shader reads is not the
texture the slice render targets write into, or the render target's array LAYER is
not honoured for a depth array. The next session should check both at the Metal
level rather than in the engine's own abstractions, which is where I stopped.

**A note on method.** Judging the first raw-depth probe by eye was a mistake: a
stored depth of 0.99 and one of 1.0 produce images I cannot tell apart, and I
initially read "looks the same" as "reads 1.0" without earning it. Replacing the
multiply with a `step(0.999, stored)` made the answer black-and-white, literally.
When a probe's output is a continuous value, threshold it.

## Clustered spot shadows: the bug was a bias, not the GPU (2026-09-05)

Fixed. The clustered spot shadow chunk subtracted a depth bias that the projection
cannot afford, so every comparison passed and nothing was ever in shadow.

**The arithmetic.** The example's spot has range 150 and the shadow camera a near
clip of 0.01. Standard [0,1] depth is `f(d-n) / (d(f-n))`, which puts the entire
useful distance range inside about 0.001 of stored depth. The shader subtracted
`-shadowBias * 20`, the non-clustered path's spot convention: at the example's
authoring value of 0.4 that is 0.08, eighty times the depth the whole scene
occupies. Every receiver compared as nearer than every caster. Fully lit.

**What upstream does**, in a one-line comment in its clustered shadow chunk: the
depth bias is applied on render, not in the shader. The atlas pass already sets
hardware polygon offset, so the shader needs nothing. What upstream does apply is a
receiver NORMAL offset, and that is what `shadowData.y` now carries on both
backends.

**Why two sessions of probing missed it.** I had been testing the hypothesis "the
atlas holds no depth", and the probe I used was `step(0.999, stored)`. A correctly
written depth in this projection is about 0.9996, which that threshold cannot tell
from the clear value of 1.0 — so every probe agreed with the wrong hypothesis while
the texture was fine all along. Thresholding a continuous probe is the right
technique, and I had picked the threshold from what an unwritten texture looks like
rather than from what a written one does. The lesson is narrower than "measure
don't guess": choose the threshold from the range the SIGNAL occupies, and if a
probe cannot separate the two hypotheses, it is not evidence for either.

Everything eliminated across the two sessions — pass construction, the array layer,
the store action, the texture identity, the binder cache, the matrix convention —
was eliminated correctly. None of it was the bug, and the bug was four lines away in
the arithmetic.

**Verification.** Metal renders hard spot shadows in all ten light pools. Vulkan
agrees: with the light orbit frozen to a fixed timestep so the two frames are
comparable, the fraction of the floor darker than half the region mean is 0.120 on
Metal and 0.127 on Vulkan, and the per-pixel difference map is empty over the
shadows and the floor. The residual (mean absolute difference 19/255) sits on
normal-mapped cube faces and light-pool rims, which is the known per-backend
`normalScale` deviation and not this feature.

## normalScale: the backends agree, and the note about it was backwards (2026-09-05)

Aligned. Vulkan moved onto Metal's form, which is upstream's; no Metal scene
changed.

**What the two were doing.** Upstream's `material_bumpiness` blends the sampled
tangent-space normal toward flat, `mix(vec3(0,0,1), normalMap, s)`. Metal did that.
Vulkan multiplied xy by the scale and left z alone, which steepens the normal's
slope precisely where the mix flattens it. The two expressions agree only at 0 and
at 1, and 1 is the default that every asset in the tree carries, which is why this
survived unnoticed and why the standing note had the two backends the wrong way
round. That note also predicted alignment would move every normal-mapped Metal
scene. It moves none of them.

Vulkan also re-orthonormalized the interpolated tangent against the normal
(Gram-Schmidt) and left the bitangent unnormalized, where Metal and upstream
normalize both and do nothing else. Removed for the same reason.

**Verification.** The two scenes named in the open items both leave `normalScale`
at 1, so neither could show anything: on `ambient-occlusion-davinci` the before and
after frames are identical to four decimal places in every region. Isolating the
term instead, with the scale forced to 0.5 and the camera in
`DEBUGPASS_WORLDNORMAL` so only the shading normal reaches the pixel:

| Vulkan form | shading-normal difference vs Metal, MAD | p99 |
|---|---|---|
| xy scale (old) | 0.992 | 10.0 |
| mix toward flat | 0.139 | 2.0 |

A seventh of the disagreement, and the whole improvement is the mix: keeping the
Gram-Schmidt alongside the mix gives 0.139 and a p99 of 2.0 as well, so removing it
measured as an exact no-op on this mesh. It is removed for convergence, not for a
result. If a mesh ever shows tangent skew, the fix is to add it to both backends.

**Two open items lost their explanation.** The davinci floor's red channel and the
clustered scene's cube faces were both attributed in part to this. Neither is: both
scenes sit at the default scale where the old and new code are the same expression,
and the shading normals now agree to 0.139/255 regardless. `parallax-mapping`
renders at 0.93 of Metal for a reason that is still unfound; that is a new open
item, and the parallax march is the place to look, not the normal.

## The detail-normal blend, the other half of the same defect (2026-09-05)

The base-map fix in the previous entry left the detail map beneath it wrong in the
same two ways, on both backends. Fixed together with upstream's chunk as the
reference.

**What was wrong.** `detailNormalScale` multiplied the detail map's xy instead of
blending it toward flat, exactly the base map's bug one block lower. And the two
normals were combined by adding their xy, which treats the detail's slope as though
the base were flat, so the combined slope is wrong wherever the base is not.
Upstream reorients instead, rotating the detail into the base normal's own frame:

    n1 = base + (0,0,1)
    n2 = detail * (-1,-1,1)
    out = n1 * dot(n1, n2) / n1.z - n2

left unnormalized, because the TBN product is normalized afterwards. It degrades
correctly at both ends: a flat detail map returns the base exactly, and a flat base
returns the detail exactly.

**This is not an alignment fix.** Both backends carried the identical defect and
both move by the same amount, which is itself the cleanest evidence the port is
symmetric. Measured on a normal-mapped scene given a second copy of its normal map
as a detail map at four times the tiling, with the camera in
`DEBUGPASS_WORLDNORMAL`:

| | shading-normal change, MAD | p99 |
|---|---|---|
| Metal | 1.84 | 15 |
| Vulkan | 1.86 | 16 |

Backend agreement is unchanged either way, 0.588 before and 0.645 after, both far
below the change itself.

**No example covers this.** Nothing in the tree sets a detail normal map, which is
why two wrong lines survived since the feature landed in 2026-07, and why the probe
above had to invent a material. Upstream's own coverage is `test/detail-map`, which
it flags HIDDEN, and it cannot be ported faithfully yet because it toggles diffuse,
normal and AO detail maps where only the normal one exists here.

## Compose order: DOF before SSAO (2026-09-05)

Swapped, matching upstream `compose.js`. The gap-audit entry that named this was
right and the guidelines file was wrong: it claimed the whole chain mirrored
upstream while this one pair was reversed. Both claims are now corrected.

Depth of field runs first, so ambient occlusion multiplies the already-defocused
colour and is not itself blurred. Occlusion keeps full strength in out-of-focus
parts of the frame; the old order let the defocus wash it out along with
everything else.

**Verification.** No shipped example enables both effects, which is why the order
never showed. Probed by turning SSAO on in the `depth-of-field` scene:

| | change from the swap, MAD | p99 | max |
|---|---|---|---|
| Metal | 11.55 | 95 | 165 |
| Vulkan | 11.59 | 95 | — |

Two things make that convincing beyond its size. The backends move by the same
amount, so the swap is symmetric. And the bottom third of the frame, the part in
focus, comes back at MAD 0.03 — where DOF is not blurring, the order cannot matter,
and it does not. The top and middle thirds darken (mean 91.6 to 78.0 and 112.7 to
91.7) because the occlusion there is no longer being blurred away.

Backend agreement is unaffected at this scale, 1.09 before and 1.68 after, both far
under the change itself.

## The parallax-mapping gap was the spot cone, and a worse bug beside it (2026-09-05)

`parallax-mapping` rendered at 0.93 of Metal on Vulkan. It is now 0.997, and the
spot light that caused it matches to 1.0000. A second divergence turned up in the
same function, fifteen times larger, in a branch the scene does not use.

**The bisect**, in the order it ran. Albedo agreed to 1.0002, which cleared the
whole material frontend AND the parallax march itself, since the debug pass samples
at the marched UV. The ambient-occlusion map, gloss and the shading normal all
agreed. Lighting did not. Dropping the environment left the gap; dropping the cool
omni left it and turned the ratio flat across the channels, which said one scalar
on one light; dropping the warm spot removed it. Shadows were not involved: turning
them off changed nothing on either backend, byte for byte.

Then two switches settled it. Widening the cone to 89 degrees made the backends
agree to 0.9999. Switching the falloff to inverse-squared drove Vulkan to 0.148 of
Metal.

**Bug one, the cone.** Upstream's `spot.js` is a `smoothstep` between the outer and
inner cone cosines, and the Metal chunk matches it. Vulkan ramped linearly and
squared the result. Since `smoothstep(t) = t^2(3 - 2t)` and `3 - 2t` is at least 1
over the interval, the squared ramp is never brighter and is worst in the middle of
the penumbra, where it gives half the light. The two agree only at the ends, which
is why the cone core and the near floor matched while the walls did not.

**Bug two, the falloff.** Upstream's `getFalloffInvSquared` is `16 / (d^2 + 1)`
times a squared window, and Metal matches. Vulkan had `1 / d^2` with the same
window. At four units that is 1/16 against 16/17, about a fifteenth. Two shipped
examples select this mode, `procedural-sky` and `clustered-lighting`, so it was
live rather than theoretical. It never surfaced because both scenes are animated
and cannot be screenshot-diffed.

Vulkan also carried three spellings of the cone falloff — non-clustered, clustered,
and no shared function — so the clustered one was a plain linear ramp, a third
curve again. `getSpotEffect` now sits beside `distanceAttenuation` and both call
sites use it.

| measurement (Vulkan / Metal) | before | after |
|---|---|---|
| spot light alone, lighting pass | 0.905 | 1.0000 |
| spot light alone, inverse-squared falloff | 0.148 | 1.0003 |
| full scene | 0.937 | 0.997 |

**What is left** is the indirect term: with every light off, the environment-only
frame reads 0.85 of Metal. That is a 20-to-30-count frame through a non-linear
tonemap, so the number wants re-measuring against something brighter before anyone
treats it as a 15% error.

## One-pass omni shadow caster classification (2026-09-05)

Ported from upstream. An omni light's six shadow faces used to cull independently,
each sweeping every RenderComponent in the scene and testing each caster against a
freshly built frustum. Now one sweep classifies every caster into the faces it
touches, and each face pass draws a prepared list.

The saving is more than the five extra sweeps. The six frusta share their near
plane, far plane and side-plane slope, and their axes are the world axes, so a
caster's box is tested against all six with a handful of comparisons on light-space
coordinates instead of six times twenty-four plane evaluations. A cheap rejection
against the cube bounding all six frusta comes first. That cube is deliberately
larger than the light's range: each face's far plane is flat and perpendicular to
its axis, so the frustum corners stick out past the range sphere, and rejecting
against the sphere would drop casters that are genuinely lit.

**Cost, measured with a runtime switch so both paths ran in one build, one process:**

| shadow casters in scene | one pass | six passes |
|---|---|---|
| 8 | 15 us/frame | 59 us/frame |
| 408 | 144 us/frame | 1100 us/frame |

Just under a millisecond of CPU per shadow-casting omni light per frame at 400
casters, and the gap widens with caster count. Frame time itself says nothing here:
the examples are vsync-locked at 8.3 ms, which is why the measurement times the
culling work specifically, in both modes, rather than the frame.

**Correctness.** `parallax-mapping` is byte-identical to the six-pass build and
`ambient-occlusion-davinci` differs by one count in one pixel.

**The assumption, and how it is held.** The classification only works because the
six face cameras look down +X, -X, +Y, -Y, +Z, -Z in that order.
`LightCamera::pointLightRotations` puts them there, but it is six Euler triples and
says nothing about the order; reordering them or flipping a sign would still render
six shadow maps, just with casters assigned to the wrong faces — a partial, silent
loss of shadows that no example would obviously show. So the order is pinned by
`tests/omniFaceAxisTests.cpp`, which also checks the field of view is at least 90
degrees, since a narrower one would leave wedges between the faces that a caster
could fall into and be dropped from all six lists.
