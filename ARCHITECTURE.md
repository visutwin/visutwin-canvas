# Architecture Reference

Per-subsystem reference for VisuTwin Canvas: how each feature works, how to turn
it on, and where it deviates from upstream.

Read `CLAUDE.md` first. It holds the rules and the live gotchas, and where the two
files overlap, `CLAUDE.md` is authoritative. Completed-work narrative lives in
`ENGINEERING-LOG.md`, which is a local file and not part of the repository.

## Contents

Rendering and materials: instancing, shadows, tangent frames, opacity dither,
vertex-color routing, spec-gloss and friends, scalar material maps, the material
system. Lighting: ambient SH probes, reflection probes, LTC area lights, light
cookies, lightmaps. Geometry and effects: skinning and morphs, the anim state
graph, refraction, screen-space reflections, Gaussian splatting, particles,
app-facing compute, extras, transform gizmos, the GPU profiler.

### Hardware Instancing
Two per-instance strides, both column-major `float4x4` model matrix first,
chosen by the `VertexFormat` the app builds and passed to
`MeshInstance::setInstancing`:
- **64 B matrix-only** — `VertexFormat::defaultInstancingFormat()` (upstream's
  `getDefaultInstancingFormat`). Base color comes from the material, as in any
  other draw.
- **80 B matrix + `float4` sRGB color** — `VertexFormat::colorInstancingFormat()`.
  The per-instance color REPLACES the material's base color. Selects
  `VT_FEATURE_INSTANCING_COLOR`, which is what declares `instanceColor
  [[attribute(10)]]` and the matching 5th entry in the Metal vertex descriptor.
  GPU instance culling (`enableGpuInstanceCulling`) requires this stride — its
  cull kernel compacts fixed 80-byte records — and refuses the 64-byte one
  rather than misreading it.
Instanced casters go through the shadow passes too (`RenderPassShadowDirectional`
and `RenderPassShadowLocalNonClustered` bind the instance buffer at slot 5, fetch
`getShadowShader(..., instancing, instancingColor)` and draw instanced, then
unbind slot 5 — the backends pick the instancing vertex layout by scanning bound
slots, so a leftover binding would follow the next caster into its pipeline).
`MeshInstance::setInstancing` also derives a world-space AABB over the whole
instance cloud (`updateInstancingAabb`), so frustum culling and the directional
cascade fit see every instance rather than the base mesh at the node transform;
an app-supplied `setCustomAabb` wins, and a buffer with no CPU-side copy is
skipped. DEVIATION: upstream leaves that AABB to the app.

The instanced shader variant follows **the draw, not the material**: the renderer
derives `instancing`/`instancingColor` from the mesh instance's instancing data
and its buffer format, and passes them to `ProgramLibrary::bindMaterial`. The
legacy `Material::setShaderVariantKey(1<<33)` opt-in is still honoured (and
implies the 80-byte layout, the only one that existed when it was the sole way
in). DEVIATION: Vulkan's pipeline draws an instanced mesh with `forwardInstancedVertex`,
which reads the matrix only (`forwardInstancedColorVertex` is Metal's), so 80-byte buffers
render with the material color there.

### Directional shadows
- **Shadow bias convention**: `LightComponent::setShadowBias` takes upstream's **0..1 authoring value** (default 0.05) and remaps it to the internal `Light::shadowBias` as `-0.01 * clamp(v,0,1)` — negative on purpose, because the shadow passes apply `shadowBias * -1000` as the hardware polygon offset and that product must be POSITIVE to push casters AWAY from the light. Passing the raw component value straight through inverts it, so a larger bias produces MORE shadow (a self-casting ground goes fully black at 0.05). Hardware bias is skipped for PCSS (biases in-shader) and for omni lights on both paths (their faces store perspective depth and take a relative bias in-shader), as upstream skips it for its distance-storing omni maps. The directional PCF shader uses a fixed 0.0001 receiver bias, NOT the light's; only local/clustered lights consume the light bias in-shader (negated + upstream's ×20 spot scale, since our shader subtracts it from the receiver depth). A receiver's shadow-space depth is saturated to [0, 1] on both backends (upstream's ortho `getShadowSampleCoord`), so a receiver beyond the caster-fitted far plane — a receiver-only ground — is shadowed by the casters in front of it rather than skipped.
- **PCF3_32F** (default): hardware-compared depth2d, 4-tap bilinear PCF reconstructing a 3×3 kernel. Vulkan has no comparison sampler bound and does each bilinear comparison by hand (`shadowTap`: four texels gathered and compared, blended by the bilinear fraction), with the same kernels for PCF1/3/5 on every path.
- **PCSS_32F** (`SHADOW_PCSS_32F`): contact-hardening soft shadows (upstream `shadowSoft.js` PCSSDirectional). Also supported on **spot/omni local lights** (upstream `shadowPCSS.js`): a runtime uniform branch (no extra variant) driven by `LightingData::localShadowPcss0/1` = {searchArea UV (0=off), near, far}; spot = Vogel-disk blocker+filter with per-tap depth linearization, omni = Vogel-sphere direction perturbation on the depth cube; `searchArea = penumbraSize/shadowResolution (*fovRatio for spot)` — local-light `penumbraSize` is in shadow-map PIXELS (~10-40), NOT the directional world-space scale. Example: `pcss-local-example.cpp`. The spot shadow camera applies upstream's `rotateLocal(-90,0,0)` (camera looks -Z, light emits -Y), and `MetalUniformBinder` uploads `localShadowMatrix` untransposed (`Matrix4::getElement` takes **(col,row)**, not (row,col)); without either, spot 2D shadow maps do not work. Vogel-disk blocker search + filter (in-shader sample generation, `fractSinRand` seed), world-space penumbra: `penumbra = shape * penumbraSize * depthRange` with `shape = 1-(1-t)^penumbraFalloff`. Per-cascade ortho radii + caster depth ranges flow via `LightingUniforms::pcssCascadeRadii/pcssCascadeDepthRanges`; `pcssParams` = {filterSamples 16, blockerSamples 16, penumbraSize, penumbraFalloff}. DEVIATION: reuses the standard PCF hardware depth map sampled RAW (non-comparison `shadowRawSampler`) instead of upstream's dedicated R32F color map — the `pcf=true` flag in its `shadowTypeInfo` entry selects the depth attachment. Configure: `setShadowType(SHADOW_PCSS_32F)` + `setPenumbraSize(0.02-0.05 — upstream example scale; ~tan of light angular size)` + `setPenumbraFalloff(>=1)`. `VT_FEATURE_PCSS_SHADOWS` set per frame like VSM. GOTCHA: a huge ground plane left as shadow CASTER inflates the fitted caster depth range → whole-plane acne (PCF) and blown-up penumbras (PCSS) — set `render->setCastShadows(false)` on large receiver-only ground.
- **VSM_16F**: Exponential VSM with `c = 5.54`, RGBA16F moments storage. Render path writes `(exp(c·z), exp(c·z)², 1, 1)`; sample path uses Chebyshev's inequality with `reduceLightBleeding(0.1)`. Separable Gaussian blur (default 11-tap, configurable via `LightComponent::setVsmBlurSize`) runs after shadow render. Depth tightening uses **caster-AABB projected onto shadow-cam Z** (rotation-invariant for static scenes — eliminates per-frame depth jitter that would otherwise show as variance flicker on thin geometry). Configurable per-light via `LightComponent::setShadowType(SHADOW_VSM_16F)` + `setVsmBias(0.0025f)` + `setVsmBlurSize(11)`. Mirrors upstream `SHADOW_VSM_16F` (`shadowEVSM.js` + `blurVSM.js`).

### Tangent frames — there is no derivative TBN

Normal mapping uses VERTEX TANGENTS only. Upstream has a derivative-based fallback
(`TBN.js`) for meshes without them, and had to fold the backend into its `tbnBasis`
sign because screen-space dpdy is Y-down on WebGPU and Y-up on WebGL (upstream
#9099). That whole class of bug cannot arise here: nothing samples a screen-space
derivative to build a tangent frame. The fallback instead is CPU tangent generation
in the parsers (`generateTangents`, run for triangle primitives when the file
carries no TANGENT attribute).

A mesh CAN still reach the shader with a zero tangent — a non-triangle primitive
with no tangent stream. `normalize()` of that is NaN, which poisons the shading
normal and the pixel. Both backends guard it (`length_squared(T) >= 1e-6`,
falling back to the geometric normal).

`normalScale` blends the sampled normal toward flat on both backends
(`mix((0,0,1), sample, normalScale)`, upstream's `material_bumpiness`); see the
`normalScale` gotcha below.

### Opacity Dither
`VT_FEATURE_OPACITY_DITHER` (upstream `opacity-dither.js`, BAYER8 variant): `StandardMaterial::setOpacityDither(true)` renders partial opacity in the OPAQUE pass by discarding fragments against a screen-space 8×8 Bayer threshold (pow-2.2-linearized) — no sorting artifacts, correct depth writes. Keep the material non-transparent; alpha comes from `setOpacity`/texture alpha. DEVIATIONS: no blue-noise/IGN variants, no per-frame jitter (static pattern). Bayer helpers live in the `common-dither` chunk (`ditherThreshold` / `ditherDiscards`, shared by the forward and shadow chunks); the forward discard block sits after the alpha-test block.

**Decoupled strength — `alphaDither`** (upstream `StandardMaterial.alphaDither`): opacity normally drives BOTH the alpha blend and the dither density. `setAlphaDither(v)` splits them so opacity drives only the blend and `v` only the dither; `clearAlphaDither()` restores the coupled default. It rides in `MaterialUniforms::dispersionParams.y`, where NEGATIVE means unset. That sign also gates the legacy `alpha = 1.0` write: coupled use is an opaque-pass technique so alpha is forced, while decoupled use is upstream's blend-AND-dither case where alpha must survive to drive the blend.

**Shadow dither — `opacityShadowDither`** (flags bits 29-31, independent of the forward mode in bits 25-27): a partially-opaque caster discards the same Bayer pattern in the shadow pass and so throws a THINNED shadow. Two things make it fire, and both are easy to miss: the shadow pass otherwise **bypasses materials entirely** (the device hands the shader a default-constructed `MaterialUniforms`), so `renderPassShadowDirectional` binds the caster's real material — but ONLY for casters that opted in, leaving every other caster's draw untouched; and `shadowCasterFiltering` lets blended materials cast (it never consults blending: a blended caster with neither alpha test nor shadow dither writes SOLID depth, and one that should cast nothing turns `castShadow` off), or the very casters this feature targets would never reach the pass. It runs in `forward/shadow-fragment.slang`, shared by `shadowOpacityFragment` (the PCF caster stage; on Vulkan attached only to an alpha-tested or dithered caster) and `shadowVsmFragment`, so both shadow types dither on both backends.

Example: `pcss-dither-example.cpp` (port of upstream `graphics/dithered-transparency`).

### Clustered lighting and the shadow atlas
Clustered lighting is ON by default, as upstream (`Scene::setClusteredLightingEnabled`):
local lights are bucketed into a world-space cell grid (`WorldClusters`, one grid per
distinct light set) and the forward shader walks the cell's list instead of a bounded
main array. Every shadow-casting spot AND omni light then renders into ONE packed
2D depth texture, the `LightTextureAtlas` (`LightingParams::shadowAtlasResolution`,
upstream's 2048 = 16 MB whatever the light count), and none of them enters the
main array — which is why the two `kMaxLocalShadows` slots do not cap anything
here. Off, every caster owns its own map (a cubemap for an omni light) and at most
two of them shadow; that path remains for PCSS local shadows and cookies, which the
clustered shader does not sample yet (`pcss-local` and `lights` opt out).

- **Slots** (`LightTextureAtlas::update`, upstream `light-texture-atlas.js`): the
  atlas is split into as many equal squares as there are shadow-casting lights
  VISIBLE this frame (`Light::visibleThisFrame`, a union over cameras; a culled light
  holds no slot and its allocation is cleared with the frame's visibility)
  (`ceil(sqrt(n))` on a side, or `LightingParams::atlasSplit`), sorted largest
  first; lights are ranked by `Light::maxScreenSize` and take slots in that order.
  A light keeps its slot while the size still matches, so a one-shot shadow
  survives; a light handed a different slot is flagged `atlasSlotUpdated` and its
  `SHADOWUPDATE_NONE` re-armed to `THISFRAME`. Lights beyond the slot count cast
  no shadow that frame. A spot renders into its slot inset by 4 px; an omni's six
  faces render into a 3x2 grid of tiles a third of the slot wide, each face 3 px
  WIDER than 90 degrees so a filter kernel at the tile edge stays inside it, and
  the shader insets its UV by the same 3 px (`kShadowEdgePixels`).
- **Rects are top-left origin**, the texture origin this engine has everywhere, on
  both backends: a face renders through a viewport in that space and the shader
  samples in it, so no flip sits between them. The spot's shadow VP folds its
  viewport in (`LightCamera::viewportProjectionBias`, the cascade viewport matrix);
  an omni carries only its rect and depth range in the same 64 bytes, and the
  shader picks the face and UV from the light-to-fragment direction exactly as a
  hardware cubemap lookup would (`getCubemapFaceCoordinates` in
  `forward/common-parallax.slang`, mirrored in
  `LightTextureAtlas::cubemapFaceCoordinates` and held against the six face
  cameras' real projection by `tests/lightTextureAtlasTests.cpp`). Omni faces
  store perspective depth over the light's range and take the cubemap path's
  RELATIVE bias, so the two paths agree pixel for pixel.
- **One pass, rect-wise clears** (`RenderPassShadowLocalClustered`): every face
  of every atlased light draws in one render pass that LOADS the atlas, because
  one-shot shadows in other slots must survive; each face clears only its own
  rect first, by drawing a fullscreen triangle at depth 1 under its viewport and
  scissor with the depth test at ALWAYS (`clearDepthRect`) — a load action can
  only clear a whole attachment, and this is how upstream's WebGPU backend clears
  a viewport too. The face draw itself is shared with the per-face non-clustered
  pass (`drawLocalShadowFace`), so the two cannot drift.

### Ambient SH Light Probes
`VT_FEATURE_LIGHT_PROBES`: 9-coefficient spherical-harmonics ambient replacing the flat ambient (upstream AMBIENTSH basis: `sh[0] + sh[1]x + sh[2]y + sh[3]z + sh[4]xz + sh[5]zy + sh[6]yx + sh[7](3z²-1) + sh[8](x²-y²)`). Enable via `Scene::setAmbientSH(std::array<Vector3,9>)` / `clearAmbientSH()`; the renderer sets `ProgramLibrary::setLightProbesEnabled` per frame and uploads the coefficients in `LightingUniforms::ambientSH[9]`. Coefficients are premultiplied (Ramamoorthi irradiance convolution + 1/π baked in, so a uniform environment of radiance A gives flat ambient A); `sh::projectEquirect` (`scene/graphics/sphericalHarmonics.h`) projects float or 8-bit-sRGB equirect radiance maps, `sh::evaluate` is the CPU mirror. When probes are active they replace both the flat ambient AND the env-atlas Lambert diffuse (specular IBL stays). NOTE: `ProgramLibrary::setEnvAtlasEnabled` (set per frame from `scene->envAtlas()`) gates VT_FEATURE_ENV_ATLAS — without it, unbound-atlas sampling returns nonzero `get_width()` on Apple GPUs and silently overwrites flat ambient with black. Example: `light-probes-example.cpp` (gradient sky projected to SH9, auto-cycles flat vs SH).

### Anim State Graph (modern `anim` component)
Upstream `framework/anim/controller` + `components/anim` port at `engine/src/framework/anim/controller/`, `anim/state-graph/`, `components/anim/`:
- **AnimController**: state machine with transitions (conditions on typed parameters, exit times, transition offsets, interruption sources, priorities), crossfades via clip blend weights, previous-state stack for interrupted transitions.
- **Blend trees**: 1D, 2D cartesian, 2D directional, direct — all four upstream variants in one file (`animBlendTree.h/.cpp`), built from typed `AnimBlendTreeDesc` (DEVIATION: no JSON graph format; `AnimStateGraph` is a builder-style C++ data model).
- **AnimComponent** (`anim` system id, registered like other systems via `registerComponentSystem<AnimComponentSystem>()`): layers (each own AnimController+AnimEvaluator), parameters (float/int/bool/trigger — single float storage), `assignAnimation("State.Leaf", track)` paths. Layers are composed per node by weight and blend type (OVERWRITE lerps toward the layer, ADDITIVE adds its offset from the rest pose), with per-layer node masks and optional `normalizeWeights`, as upstream's AnimTargetValue: each layer's evaluator hands its pose to the component through `AnimEvaluator::setPoseSink` and the component writes each node once. Animation events on a track fire on the component as the clip's cursor passes them; the legacy `AnimationComponent` fires none.
- **AnimEvaluator** composites N clips sequentially (first contributor sets, later clips lerp by their blendWeight — mirrors upstream anim-evaluator.js); legacy AnimationComponent crossfade unchanged.
- Example: `examples/src/anim-stategraph-example.cpp` (fox: Survey ⇄ 1D Walk/Run blend tree driven by a "speed" parameter; auto-demo cycles it).

- **Without a state graph, `assignAnimation("Name", track)` makes upstream's default one**: a
  `Base` layer that plays that state from the start; further names become states of it. A
  layer's `transition(to, time, transitionOffset)` blends from the active state to `to` over
  `time` through an ad-hoc transition, whatever the graph's transitions say (upstream
  `AnimComponentLayer.transition`).
  `render-to-image` drives its knight this way.
### GPU Skinning + Morph Targets
- **Skinning** (`VT_FEATURE_SKINNING`): 4-bone weighted blend. GLB parser reads JOINTS_0/WEIGHTS_0 into an 88-byte skinned vertex layout (PackedVertex + weights float4 @56 + joint indices float4 @72, attributes 11/12). `SkinInstance` builds a node-relative float4x4 bone palette per frame (deduped via `SkinInstance::beginFrame()` counter) uploaded through the slot-6 palette ring (shared with dynamic batching — mutually exclusive), once a frame per `paletteVersion()` however many passes draw the skin; the ring grows with what a frame asks for. Skins resolve bones by glTF node index at `instantiateRenderEntity()` (DEVIATION: upstream resolves by name). **Skinned culling**: the GLB parser computes per-bone bind-space AABBs (weight > 1e-4 influences, reading POSITION+JOINTS/WEIGHTS in parseSkins) stored on `Skin`; `MeshInstance::aabb()` unions each used bone's AABB transformed by `bone.worldTransform * inverseBind`, so skinned instances are frustum-culled (`cull` stays false only for skins without bone AABBs, e.g. non-GLB paths).
- **Morphs** (`VT_FEATURE_MORPHS`): DEVIATION from upstream's render-to-texture accumulation — all target deltas live in one static buffer (per target/vertex: float4 posDelta + float4 nrmDelta, vertex slot 9); the vertex shader sums the top-8 active targets driven by an 80-byte `MorphParams` uniform (slot 10, `MorphInstance::gpuParams()`). Applied in bind space before skinning. **Morph weight animation**: glTF "weights" channels parse into `AnimCurve{propertyPath="weights"}` (N components = target count), `AnimTransform.weights` blends through AnimEvaluator like transforms, and `AnimBinder::resolveMorphInstances` (DefaultAnimBinder: mesh-node entity → RenderComponent morph instances) applies them — works through both legacy AnimationComponent and the anim state graph. Test asset: `assets/models/morph_wave.glb` (generated). Example: `mesh-morph-example.cpp`, a port of upstream `graphics/mesh-morph` — three spheres carrying three procedurally built targets each, weights driven by sine curves. It builds the targets on the CPU rather than loading them, so it covers `MorphTarget` as well as the blend. NOTE: `morph_wave.glb` has no consumer.
- Both compose with shadow passes (directional + local non-clustered fetch skinned/morphed shadow shader variants lazily). Draco-compressed primitives skip skin/morph attributes.

`VT_FEATURE_VSM_SHADOWS` is set per-frame by the renderer based on the active directional light's `shadowType`. When set:
- the shadow program's `shadowVsmFragment` writes EVSM moments to RGBA16F instead of relying on hardware depth;
- the forward program reads each directional slot's moments (Metal slots 39 / 40, the same textures bound again as colour) instead of its depth map (6 / 35, `depth2d`), and evaluates Chebyshev instead of PCF.

### Reflection Probes (box-projected cubemap)
`VT_FEATURE_REFLECTION_PROBE` (upstream `cubeMapProject.js` BOX + `reflectionEnv.js`): a local, parallax-corrected cubemap reflection replacing the global env-atlas specular IBL. Scene-level (like `setSkybox`/`setEnvAtlas`): `Scene::setReflectionProbe(cubemap, position, boxMin, boxMax, boxProjection=true, intensity=1)` / `clearReflectionProbe()`. The prefiltered cubemap binds at **fragment slot 24** (`texturecube<float> reflectionProbeCube`; `kMaxTextureSlots` 24→25); `LightingData::reflectionProbeBoxMin/Max/Params` (params = {boxProjection flag, intensity, maxLod}) carry the box + settings. In `forward/forward-fragment-ambient.slang` the reflection dir `reflect(-V,N)` is box-projected (intersect the box, re-aim from box center — parallax) when `boxProjection`, X-flipped for the engine cube convention, sampled at a roughness→mip LOD, sRGB-decoded, Fresnel-weighted, and OVERWRITES `indirectSpecular`. Runtime feature: `ProgramLibrary::setReflectionProbeEnabled` per frame from `scene->reflectionProbe()`; `MetalUniformBinder::setReflectionProbeUniforms` fills the uniforms + stores the cube. DEVIATIONS: single scene-level probe (no per-mesh assignment/blending); roughness uses hardware trilinear cube mips, NOT upstream's GGX-prefiltered-per-level cube; no GGX cube→cube prefilter path yet. Example: `reflection-probe-example.cpp` (chrome sphere + polished floor in a colored-room cube; auto-toggles box projection ON — floor reflects the walls parallax-correctly — vs OFF — floor reflection collapses to a near-uniform direction-only color).

**Dynamic scene-capture bake** (`framework/extras/reflectionProbe.h/.cpp`): `ReflectionProbe` renders the live scene into the probe cubemap at runtime instead of using a supplied/authored cube. It owns a mipmapped RGBA8 color cubemap + 6 face `RenderTarget`s + 6 `CameraComponent`s pointed along ±X/±Y/±Z (reusing `LightCamera::pointLightRotations`, 90° FOV, aspect 1). The six face cameras render as ordinary cameras in the normal frame graph (each `RenderTarget` targets one cube face via `RenderTargetOptions.face`), so the face cameras must render BEFORE the main camera: cameras render in priority order and equal priorities (the default 0) keep construction order, so **construct the probe first**, or give the main camera a higher priority (upstream gives the probe -1). Per frame `update()` (called AFTER `engine->render()`) runs `GraphicsDevice::generateCubemapMips` (blit `generateMipmaps` on its own command buffer) to rebuild the roughness mips from the freshly-rendered level-0 faces, and installs the cube via `setReflectionProbe` on the first call. Modes: `setDynamic(true)` re-captures every frame (reflections track the scene); `false` = one-shot then disable the face cameras. It relies on two engine facilities: (1) `MetalGraphicsDevice::startRenderPass` sets the **color** attachment slice from `activeTarget->face()` for cube textures (as well as the depth attachment, for omni shadows); (2) `GraphicsDevice::generateCubemapMips`. Captured faces hold the normal tonemapped/gamma-encoded forward output, which the probe shader sRGB-decodes — matching the static path. DEVIATIONS: hardware-mip roughness (no GGX cube prefilter); the reflective object must sit on a layer excluded from the probe's capture layers or it self-captures (probe camera is at the probe center); probe faces miss directional-shadow cascades (fit only for the presentation camera). Example: `reflection-probe-dynamic-example.cpp` (chrome sphere on a probe-excluded layer reflects a ring of orbiting emissive boxes captured live — no env atlas/skybox, so the colored reflections come purely from the runtime capture).

### LTC Area Lights
Upstream's light SHAPE: `LightComponent::setShape(LightShape::LIGHTSHAPE_RECT / DISK / SPHERE)` on a spot, omni or directional light (`LIGHTSHAPE_PUNCTUAL`, the default, is an ordinary light). The source is the size of the light's ENTITY: the world matrix's X column times -0.5 is the half width and its Z column times 0.5 the half height (upstream `_setLtcPositional`), packed as `GpuLightData::areaHalfWidth / areaHalfHeight` with the shape, and carried per light in `GpuLight::areaHalfWidth / areaHalfHeight` (Metal) / `areaRightHalfWidth / areaUpHalfHeight` (Vulkan), the shape in the width's w. A shaped light keeps its type's cone, cookie and shadow (spot and omni shadows, VSM and PCSS included, and the directional cascades); its distance falloff is the range window alone (`getFalloffWindow`), the physical falloff coming from the LTC form factor. Shading is `VT_FEATURE_AREA_LIGHTS`, upstream `ltc.js`: the helpers at the end of `common-ltc.{metal,glsl}` (`ltcAreaLight` corners with the sphere billboarded to the reflection vector, `ltcAreaDiffuse` ×16 for a local light — a directional source keeps plain Lambert, as upstream — `ltcSpecularFresnel` from LUT 2, `ltcAreaSpecular` with LUT 1's inverse matrix, disk = `ltcEvaluateDisk`, NaN-guarded) are called by BOTH the main light loop and the cluster loop, so the two cannot drift; diffuse takes `(1 - specFres)`, clearcoat its own LTC term. While area lights are in the variant a PUNCTUAL light's diffuse is scaled by `(1 - specularity)`, upstream's AREA_LIGHTS quirk. Under clustered lighting a shaped spot or omni light is an area light only while `scene->lighting().areaLightsEnabled` (off by default, upstream's CLUSTER_AREALIGHTS); `ClusterLightData::shape` and `GpuClusteredLight::areaHalfWidth / areaHalfHeight` (176 bytes a light) carry it, and otherwise it shades as punctual. The two 64×64 RGBA16F LUTs are **embedded** in the engine (`scene/graphics/areaLightLuts.h` + generated `areaLightLutsData.inc`; DEVIATION: upstream ships them as an app-loaded JSON) — the renderer creates them lazily on the first area light and binds fragment slots 20/21 through `GraphicsDevice::setAreaLightLuts`. Examples: `area-light-example.cpp` (upstream graphics/area-lights: a shadowed rect spot, a sphere omni and a shadowed disk directional held 5000 units out, clustered area lights on) and `pcss-local-example.cpp` (a PCSS-shadowed rect spot, non-clustered).

### Dynamic Grab-Pass Refraction
`VT_FEATURE_DYNAMIC_REFRACTION` (upstream `refractionDynamic.js`): transmission samples a mid-frame **scene color grab** instead of the env atlas. `StandardMaterial::setUseDynamicRefraction(true)` (+ transmission/thickness/IOR + `setTransparent(true)` so the mesh draws after the grab) and `CameraComponent::requestSceneColorMap(true)` (the depth-layer `RenderPassColorGrab` blits the scene color into a persistent full-mip texture via `GraphicsDevice::grabSceneColor`, works for both offscreen RTs and the drawable since `framebufferOnly=false`). Bound at fragment slot 22 (`kMaxTextureSlots` now 23); rough/high-IOR surfaces read blurrier mips (upstream `iorToRoughness`); the shader projects the refracted exit point with `LightingData::viewProjection` (also in `LightingUniforms`). **KHR_materials_volume + dispersion**: `setAttenuationColor`/`setAttenuationDistance` enable Beer-law transmittance `exp(-(-log(attColor)/attDist)*thickness)` (applies to BOTH dynamic and env-atlas paths; the dynamic path absorbs over the refracted vector's length, which carries the model scale); `setDispersion` (dynamic path only) samples R/G/B at spread etas (`halfSpread = (ior-1)*0.025*dispersion`). The GLB parser reads KHR_materials_transmission/_ior/_volume/_dispersion (`applyVolumeExtensions`). **Linear-grab**: the pow(2.2) grab decode is gated on flagsAndPad bit 5 — under the HDR CameraFrame path the mid-frame grab is linear and is NOT decoded. The refraction offset is scaled by the model's per-axis world scale (Metal passes it from the vertex stage, Vulkan reads the model matrix from the push constants). Example: `refraction-example.cpp` (glass sphere over colorful columns, auto-cycles dynamic vs env-atlas).

### Screen-Space Reflections
`VT_FEATURE_SSR` (upstream `reflectionSSR.js` in spirit — per-fragment world-space march, not upstream's post-process view-space HiZ): a glossy surface reflects the on-screen opaque scene by ray-marching the reflection vector against a **scene depth grab** and sampling the **scene color grab** at the hit. `StandardMaterial::setUseScreenSpaceReflection(true)` + `setTransparent(true)` (so the surface draws AFTER the mid-frame grabs, in the depth layer) and BOTH `CameraComponent::requestSceneColorMap(true)` + `requestSceneDepthMap(true)`. Reuses the refraction color grab (slot 22); adds a **depth copy** — `GraphicsDevice::grabSceneDepth(RenderTarget*)` mirrors `grabSceneColor`, blitting the pass depth into a persistent `Depth32Float` private texture bound at **fragment slot 25** (`depth2d<float> ssrSceneDepthTexture`; `kMaxTextureSlots` 25→26). A depth COPY is required because the live depth buffer is still attached during the transparent draw (can't sample the target you're writing). The march (in `forward/forward-fragment-ambient.slang`, after the reflection-probe block) projects each world-space step through `LightingData::viewProjection` to screen UV, compares the step's clip-space `w` (view distance) against the linearized scene depth (`sceneZ = near*far/(far - rawDepth*(far-near))`, near/far via `LightingData::cameraNearFar` fed by `GraphicsDevice::setCameraClipPlanes` per frame from the camera). On a hit within `ssrThickness` it decodes the grab (pow(2.2) unless the HDR camera-frame path, flagsAndPad bit 5), applies an edge fade + `getFresnel`; the colour grab is sampled at an explicit LOD picked by a ROUGHNESS CONE (GGX half-angle roughness², its footprint at the hit converted to grab pixels), so a rough surface gets a blurred reflection, and only roughness above 0.7 fades it out, and `mix`es OVER the probe/env-atlas `indirectSpecular`. Variant key bit 47; runtime gate `options.ssr = stdMat->useScreenSpaceReflection()`. DEVIATIONS: forward-pass per-fragment march (no HiZ acceleration; 48 steps over 0.4 × (far − near), the crossing bisected 6 times, thickness 1.25 steps), no temporal accumulation or hit fade-by-thickness, off-screen rays fall back to env/probe (no screen-edge stretch). Example: `ssr-example.cpp` (three colored opaque objects on a dark mirror floor; auto-toggles SSR every 3 s — ON, each floor region reflects the object above it; OFF, the floor collapses to flat ambient).

### Extras: OutlineRenderer + ViewCube
`engine/src/framework/extras/` (ports of upstream `extras/`):
- **OutlineRenderer** (`outline-renderer.js`): colored selection outlines. A dedicated "Outline" layer (id 100, excluded from the main camera's default layer list) + offscreen camera render flat **unlit clone** mesh instances (sharing mesh+node with the source — DEVIATION: upstream re-renders originals through a shader-pass override) into an RGBA8 target; H/V extend quad passes (5-tap dilate + edge alpha, offsets/srcMultiplier baked into two shader variants since quad passes carry no uniforms) then an alpha-blend quad composite over the back buffer. The three post passes register via **`Renderer::addAppendPass`** — a new engine mechanism appending app passes to the END of the frame graph. GOTCHA that motivated it: rendering to the back buffer AFTER `Engine::render()` crashes (frameEnd presents the drawable; a stale `_frameDrawable` reuse is a pointer-auth SIGSEGV — the older edge-detect example has this latent bug). Per frame: `outline->frameUpdate(cameraEntity)` before render.
- **ViewCube** (`view-cube.js`): world-axis orientation gizmo. DEVIATION: upstream is DOM/SVG; this port renders unlit sphere handles + axis rods on the IMMEDIATE layer with depth test off, anchored each frame to the camera's top-right corner (`update(cameraEntity)`). `onClick(x, y, w, h, cameraEntity)` unprojects a ray (standard-Z, near plane at clip z=0), ray-sphere picks the six handles, and fires `ViewCube::EVENT_CAMERAALIGN` with the world axis (EventHandler payload).
- An entity whose render component is not `active()` (its own flag, or a disabled entity or ancestor) draws no outline, as upstream 7c1e90f34.
- Example: `outline-viewcube-example.cpp` (auto-cycling outline over three objects + view cube; includes an onClick scan self-test).

### Transform Gizmos
`framework/gizmo/` is upstream's `extras/gizmo` (view-cube.js excepted, a DOM widget;
the ViewCube above stands in for it): `Gizmo` (gizmo.js), `TransformGizmo`
(transform-gizmo.js), `TranslateGizmo`, `RotateGizmo`, `ScaleGizmo`, the shapes
(`shape/shape.h`, `shape/shapes.h`: arrow, arc, box, box-line, plane, sphere), `TriData`,
`MeshLine` and `GizmoMaterial` (shaders.js). Usage is upstream's:

    auto layer = Gizmo::createLayer(engine);            // clears depth, sorts nothing
    TranslateGizmo gizmo(cameraComponent, layer);       // adds the layer to the camera
    gizmo.on(Gizmo::EVENT_POINTERDOWN, [](float x, float y, MeshInstance* mi) { ... });
    gizmo.attach(entity);

- The gizmo needs no per-frame call and no event forwarding: it subscribes to the
  engine's `update`, `prerender` and `destroy` events and to the engine MOUSE device's
  `mousedown` / `mousemove` / `mouseup` (canvas points; SDL's touch-synthesised mouse
  events make touch work too). `pointerDown` / `pointerMove` / `pointerUp` are public
  for an application with its own input, and for `tests/gizmoTests.cpp`. Upstream's
  `document.pointerLockElement` test is the mouse's relative mode, and pointer capture
  is `SDL_CaptureMouse` for the length of a press.
- Size: `gizmoViewportScale` is upstream's `_updateScale` (perspective `tan(fov / 2) x`
  forward distance `x 0.3`, ortho `orthoHeight x 0.32`, times `size`), so the gizmo keeps
  its size on screen; upstream's examples set `size = 1024 / canvas height`.
- Picking is upstream's: the pointer ray against each shape's TriData (unit
  primitives placed by a transform inside the shape's entity), nearest hit first except
  that two hits that both carry a priority order by priority (the centre sphere/box at 2
  beats an arrow's line at 1). Upstream's sort comparator is not a strict weak order, so
  the first element is found by a scan, which is what a sort would put first.
- Shapes draw through `GizmoMaterial`, a Material with one Slang program (`gizmo`) shared per
  device: upstream's unlit gizmo shader — flat colour, alpha blended, discard below 1/255,
  clip z clamped, and the fragment depth forced to `depth` when that is not negative (the
  plane handles at 1, behind every other shape; the angle guide lines at 0, in front).
  The colour block rides the material slot through `customUniformData`.
- The gizmo layer's depth clear needs the IN-PASS clear (`clearDepthInPass`, see Layers
  and depth state): the layer's render actions sit in the same forward pass as the
  scene's, and a load action can only clear at a pass's start.
- Span guide lines (hovering or dragging an axis): DEVIATION — upstream's `app.drawLine`
  becomes two `WideLineRenderer`s of one-pixel lines, the base colour depth-tested on the
  Immediate layer and the occluded colour (alpha `1 - guideOcclusion`) untested and
  blended on the gizmo layer, each clipped to the camera's near plane on the CPU (a wide
  line has no near clip of its own). The rotate gizmo's angle guides are upstream's
  MeshLines.
- C++ lifetime, where upstream relies on its garbage collector: the gizmo follows each
  attached Entity's `destroy` event and drops it (detaching once none is left), follows
  its camera's, and tears itself down on the engine's `destroy`. Each class's
  destructor runs `destroy()` while all of its members still exist, because `nodes:detach`
  handlers of every level run inside it. Events whose arguments upstream leaves
  undefined (`pointer:up` from a detach) carry `(0, 0, nullptr)`: a typed handler here is
  skipped when arguments are missing.
- Other DEVIATIONS, marked at the code: theme colours and axes are C++ types
  (`GizmoTheme`, `GizmoThemePartial`, `GizmoAxis`) instead of string keys, with the
  shape keyed by its handle (YZ) and reporting its plane's normal axis (X) as upstream's
  `plane:x`; `rotation:update` carries Euler angles (`gizmoEulerAngles`, upstream's
  `getEulerAngles`); the arc shape rebuilds its two mesh instances on a radius change
  (no `MeshInstance` mesh setter); the sphere and plane are the engine's 48-band and
  one-quad primitives; the material writes the colour as given, right for the gamma back
  buffer but not decoded for a camera frame's linear HDR target. The deprecated
  `xAxisColor` / `colorAlpha` / `flipShapes` / `orbitRotation` accessors are not ported.
- Examples: `transform-translate`, `transform-rotate`, `transform-scale` (upstream
  gizmos/*), with upstream's layer, events, size rule and defaults; no controls panel.

### Vertex Color Routing + Unlit Emissive
`StandardMaterial::setDiffuseVertexColor` / `setEmissiveVertexColor` (upstream's
two flags of the same name). A mesh's vertex colors modulate the
DIFFUSE lane by default, and material flag bit 28 turns that off while bit 23 routes them to
EMISSIVE instead. `emissiveVertexColor` also implies the vertex-color variant
(`options.vertexColors`), which is otherwise an explicit `shaderVariantKey` bit 21
opt-in, since the material cannot see whether the mesh carries a color stream. Vertex
colours are LINEAR; `setVertexColorGamma(true)` (variant bit 35 otherwise) decodes them in
the vertex stage. `setUseLighting(false)` drops the direct lights only
(`VT_FEATURE_NO_LIGHTS`); `setUnlit(true)` is the `base + emissive` path (UI, MSDF text,
outlines, the view cube, glTF unlit). A glTF
primitive with `COLOR_0` draws with a parser-made copy of its material carrying that bit;
the parser also flat-shades triangle primitives without NORMAL and honours sparse accessors
on every attribute.

`VT_FEATURE_UNLIT` outputs `baseColor + emissive` rather than base color
alone. Upstream reaches this path through `useLighting = false`, which drops the
lights but keeps the emissive lane — without the emissive term a material with
black diffuse and a bright emissive map (upstream's decals) renders as pure black. The emissive term
is recomputed inside the unlit block because that early return never reaches
`forward-fragment-emissive`. Example: `mesh-decals-example.cpp`.

### Light Cookies
`VT_FEATURE_COOKIE_2D` / `VT_FEATURE_COOKIE_CUBE` (upstream `cookie.js` +
`lightFunctionLight.js`): a texture the light projects onto the
scene, multiplying its color — a 2D texture through a **spot**'s beam, a cubemap
sampled by direction for an **omni**. Authored on the component:
`LightComponent::setCookie(texture)` + `setCookieChannel(CookieChannel)`
(`COOKIE_CHANNEL_RGB/R/G/B/A` — upstream's 3-char swizzle) + `setCookieIntensity`
+ `setCookieFalloff` (spot only; false drops the cone falloff so the projection's
own clip bounds the beam, upstream's `getCookie2DClip` variant).

The cookie mask multiplies the light color BEFORE any falloff. Spot cookies need
a world→cookie-UV projection: a shadow-casting spot reuses its `shadowViewProjection`,
a cookie-only one gets `LightCamera::evalSpotCookieMatrix` (both go through the
shared `LightCamera::spotProjectionBias`). Omni cookies carry the light's world
transform instead; its rotation takes the light→fragment direction into cube space
(X-flipped, matching the engine's cube convention).

Slots: **two 2D + two cubemap per frame** (mirroring the local-shadow pools),
Metal fragment textures 27-28 / 29-30 (`kMaxTextureSlots` 27→31), Vulkan set 3
bindings 17-20 as separate images sharing `linearClampSampler` (combined samplers
would blow the 16-per-stage limit MoltenVK inherits). `GpuLightBlock` carries a
`cookieFlags` float4 and `LightingBlock` a 4-matrix + 4-vec4 cookie block, the ONE layout
both backends bind (`platform/graphics/lightingBlock.h`; its size is asserted there and in
`vulkanRenderPipeline.cpp`, and held against the shader bundle's reflection by
`tests/slangBundleTests.cpp`).

**GOTCHA:** cookie samples sit inside the per-light loop
behind fragment-varying `continue`s, so screen-space derivatives there are
undefined — and an undefined mip LOD reads a fully averaged mip, turning a
heart-shaped cookie into a flat wash of its own average. Both backends sample with
an explicit LOD 0 (`level(0)` / `textureLod`). DEVIATION: upstream mipmaps cookies.

A spot cookie takes upstream's `cookieAngle` / `cookieScale` / `cookieOffset`
(`getCookie2DXform`): the offset is folded into the cookie matrix on the CPU, the 2x2
applied after the clip test. Under clustered lighting (with `cookiesEnabled`) cookies
come from the clustered cookie atlas (`RenderPassCookieRenderer` copies each light's
cookie into its slot; the cluster loop samples it through the light's shadow
projection or omni face pick). Example: `lights-example.cpp`, clustered as upstream.

**Spot cone angles are HALF-angles** (upstream: `cos(outerConeAngle * DEG_TO_RAD)`,
and its shadow/cookie cameras use `fov = outerConeAngle * 2`). Do not halve them
when building `outerConeCos` (`renderer.cpp`, `worldClusters.cpp`): that makes
every spot's lit cone half as wide as the shadow and cookie frustum fitted to the
same light — a beam covering only the middle of its own cookie.

### Lightmaps
`StandardMaterial::setLightMap(texture)` → `VT_FEATURE_LIGHTMAP` variant: the lit shader samples the lightmap at **UV1** (texture slot 19; the box, cylinder, cone and capsule carry upstream's UV1 unwrap, each face or part in its own padded cell, while the plane and sphere use UV0 as UV1, as upstream) and the sRGB-decoded sample **replaces** indirect diffuse. Upstream's `lightmapAdd.js` adds it, but `lit-shader.js` gates the ambient behind `addAmbient = !lightMapEnabled` — adding both double-counts what the bake already contains and visibly washes the surface out. Specular IBL is unaffected. When adding material texture slots: bump `MetalTextureBinder::kMaxTextureSlots` AND the `materialSlots` clear list in `bindMaterialTextures`.

**GPU lightmapper** (`framework/lightmapper/gpuLightmapper.h/.cpp`): upstream's own mechanism — each target mesh is rendered **in UV space** (`VT_FEATURE_LIGHTMAP_BAKE`: the vertex stage writes clip position from UV1, the fragment stage outputs the diffuse LIGHT with no albedo), so occlusion comes from the existing shadow maps instead of rays. The bake rides the normal frame graph like `ReflectionProbe`: one camera per target with `Camera::setLightmapBakePass(true)`, its own RGBA16F render target, and a private layer holding just that mesh; `bake(targets, Options)` then `update()` after every `Engine::render()` (true on the frame the lightmaps are applied). `Options` carries upstream's scene settings under their names and defaults (`lightmapSizeMultiplier` 1, `lightmapMaxResolution` 2048, `ambientBake`, `ambientBakeNumSamples` 1, `ambientBakeSpherePart` 0.4, `ambientBakeOcclusionContrast` / `Brightness` 0, `lightmapFilterEnabled` false, `lightmapFilterRange` 10, `lightmapFilterSmoothness` 0.2) plus the per-light soft-shadow pair as `directionalBakeNumSamples` / `directionalBakeArea`. The bake is a sequence of frames, each frame after the first ADDING to the target (`Camera::setLightmapBakeAccumulate`: additive blend, `VT_FEATURE_LIGHTMAP_BAKE_ACCUM` drops the ambient), in upstream's order: with `ambientBake`, a frame of ambient light alone (env atlas or flat ambient, upstream's `dAmbientLight`), then `ambientBakeNumSamples` frames into a separate occlusion target, each one of upstream's white virtual directional lights (BakeLightAmbient, `lightmapFilters.h`: the same sphere points, given the authored intensity and shaded through `lightRadiance` as every light is) shadow-mapped; a quad pass then writes `saturate(contrast/brightness curve(occlusion)) x ambient` into the lightmap (upstream `bakeLmEnd`); then the scene's lights (without `ambientBake` this is the first frame and writes the unoccluded ambient with them, since a lightmap REPLACES the ambient here); then `directionalBakeNumSamples` frames of rotated sun copies (BakeLightSimple). Last, upstream's `postprocessTextures` as quad passes inside an offline scope (the Slang program `lightmap-filter`): the 15x15 bilateral denoise (kernel and range normaliser from `lightmapFilterRange` / `Smoothness`, computed on the CPU) or a dilate into a temporary target of the same size, then a dilate back. A texel counts as BAKED when its alpha is above zero — the bake cameras clear to alpha 0 — which is upstream's RGBM test; upstream's HDR test (`rgb > 0`) would treat a baked black texel as empty (DEVIATION, at the shader). Each phase sets every scene light from the enabled state it had when the bake began, which the end restores. THREE things it must do that are easy to miss: the bake pass forces `CULLFACE_NONE` (UV winding follows the unwrap, so half the charts would be culled), every scene light gets the bake layer ids appended for the duration (lights are filtered per layer, else only ambient bakes), and the mesh wears `MASK_AFFECT_LIGHTMAPPED` during and after the bake (a `MASK_BAKE` light is lifted to `MASK_AFFECT_LIGHTMAPPED` for the bake so it casts its shadow). DEVIATIONS: no BAKE_COLORDIR (a second bake output plus a directional-lightmap path in both forward chunks); the soft directional copies split the LINEAR scale the light shades with (`intensityForLinearScale` inverts `lightRadiance` per copy), where upstream's `pow(I^2.2 / N, 1 / 2.2)` shades a light of intensity 1 or more `I^1.2` brighter baked than live (`lightmap-sources`' planes and the house's sunlit ground read brighter in upstream's thumbnails for that reason); the ambient light's shadow bias is 0.05 rather than 0.2 (this engine's bias feeds a polygon offset). Backend parity, `lightmap-bake` at frame 90: no pixel of 630k differs by more than 1 count since Vulkan's PCF took Metal's bilinear comparisons (2026-10-06; 322 pixels above 8 counts before, from the directional PCF of the 20 occlusion lights). The CPU `Lightmapper` stays as a ray-traced reference.

**Lightmapper baker** (`framework/lightmapper/lightmapper.h/.cpp`): a **CPU** baker (upstream is a GPU UV-space renderer — DEVIATION). `addLight()` (directional/point/spot) + `addOccluder(mesh, worldTransform)` (world triangles for ray casting) + `bake(targetMesh, worldTransform, Options)` → RGBA8 texture (or `bakeAndApply(material, ...)`). Options mirror upstream's scene-level bake knobs: `sizeMultiplier`/`maxResolution` derive a per-mesh resolution from world bounds (upstream `calculateLightmapSize`), `ambientBake` + `ambientBakeNumSamples`/`SpherePart`/`OcclusionContrast`/`OcclusionBrightness` replace the flat AO term with rays distributed over the top part of the sphere shaped by upstream's `bakeLmEnd` curve, `filterEnabled`/`filterRange`/`filterSmoothness` run a bilateral denoise, and per-light `bakeNumSamples`/`bakeArea` give directional lights soft shadows (upstream spreads N virtual lights over the cone; the ray tracer jitters the shadow ray instead). Per target mesh it reads CPU vertex/index storage (`VertexBuffer::storage()` as 56-byte `PackedVertex`, uv1 at offset 48), rasterizes triangles in **UV1 space** (barycentric per texel → world pos+normal), and shades: direct lighting (Lambert × attenuation + spot cone) with **hard shadow rays**, cosine-weighted-hemisphere **ambient occlusion**, ambient+sky terms AO-modulated; then dilates seams and sRGB-encodes (the shader pow(2.2)-decodes). Ray any-hit uses `LightmapperBvh` (`framework/lightmapper/lightmapperBvh.h`): a median-split binary BVH collapsed to **four children per node**, walked with one 4-box SIMD slab test per node (SSE2 / NEON / scalar), leaves of at most four triangles, boxes padded so float rounding can never reject a real hit. The 4-wide tree forced scalar gains little; the win is the SIMD test. The expensive shading phase is multi-threaded (`std::thread::hardware_concurrency`). DEVIATIONS: LDR RGBA8 only, single bounce (no GI), no color+dir directional lightmaps, no auto lightmap-size/UV-unwrap (uses the mesh's existing UV1 — box faces overlap, so bake receiver-only planes). Mask a lightmapped mesh out of realtime lights with `MeshInstance::setMask(MASK_AFFECT_LIGHTMAPPED)`. Example: `lightmap-bake-example.cpp` (floor baked with soft shadows + AO from occluder boxes/sphere; toggles the lightmap on/off). Test asset: `assets/textures/lightmap-pools.tga` (render-to-texture example ground).

### GPU Profiler
`GraphicsDevice::gpuProfiler()` (nullptr when unsupported; disabled by default — `setEnabled(true)`). Metal impl (`metalGpuProfiler.*`): MTLCounterSampleBuffer stage-boundary timestamps attached per render pass in `startRenderPass` (start-of-vertex → end-of-fragment), 3 triple-buffered sample-buffer slots resolved 2 frames late, tick→ns via correlated `sampleTimestamps`. Results: `passTimings()` (per-pass ms, named via `RenderPass::name()`) + `frameMilliseconds()`. **Both backends resolve through `GpuProfiler::publishTimings`, and the figure is defined to be comparable with upstream's:** a pass costs the delta between consecutive END samples, not its own start-to-end interval, because on a pipelined GPU the tiler starts a pass's vertex work while its predecessor's fragment work still runs and the intervals overlap (upstream's profiler says the same and reports a span). Upstream's span is no use on a native swapchain — the last pass waits for the drawable INSIDE the frame's GPU timeline, so the span is the whole vsync interval (16 ms at 60 Hz) — hence a pass targeting the drawable keeps its own interval instead of a delta that would contain that wait, and the frame is the sum. `tests/gpuProfilerTimingTests.cpp` pins both rules. With vsync off a back-buffer pass can still absorb the wait; that is a benchmark configuration and the back-buffer rows are where it shows. Compute passes not yet instrumented. NOTE: metal-cpp framework extern constants (e.g. `MTL::CommonCounterSetTimestamp`) only link in the `*_PRIVATE_IMPLEMENTATION` TU — compare string values instead inside the engine library.

### GPU Particle System
`engine/src/scene/particles/` + `framework/components/particlesystem/` (upstream particle-system component, GPU-sim subset): **ParticleSystemComponent** — mutate `options()` then `apply()`; upstream's `play/pause/unpause/stop/reset`, `autoPlay` (false builds it paused with its mesh instance hidden), `preWarm` (reset queues one lifetime in 32 steps, dispatched at the next update because a dispatch belongs inside a frame) and `layers`. Simulation is a backend-agnostic compute dispatch (`ParticleEmitter::simulate` builds a `Compute` over the Slang program `particle-sim` and calls `GraphicsDevice::computeDispatch`, ordered before the frame's render encoding) over a persistent 64-byte `GpuParticle` pool with upstream's particle clock: particle i starts at life `-i*rate` (rate 0 is a burst), the unborn are re-spawned every step, a finished particle's life wraps back by `max(lifetime, numParticles*rate)` and is shown again only while the emitter loops, and `stop()` hides the unborn. Hash-seeded spawn (box/sphere shapes); the velocity is the port's initial velocity + spread with gravity and damping PLUS upstream's `localVelocityGraph`/`velocityGraph` (each a per-life random point between the graph and its graph2, 16-sample LUTs in the 1280-byte `GpuParticleSimParams`), and `rotationSpeedGraph`/2 integrates into the angle. Rendering mirrors the gsplat branch: `MeshInstance::particleEmitter()` keyed instanced tri-strip quad per particle, self-contained billboard shader (particle pool vertex slot 7, `GpuParticleRenderParams` slot 11 — SHARED with gsplat slots, a draw is one or the other), curves (`scaleGraph` — a HALF-extent, as upstream — `colorGraph`, `alphaGraph`) quantized to 16-sample LUTs in the render params, upstream's clockwise rotation, `alignToMotion` and `stretch` (upstream's pointAlong and stretch chunks, in view space), sprite-sheet animation (`animTilesX/Y`, `animNumFrames`), additive/normal/premultiplied blending, optional `colorMap` bound via the material baseColor slot (procedural soft disc when null), `intensity` for HDR glow, and upstream's output stage in the fragment (the sRGB colour map decoded, times the colour graph decoded from gamma space when its LUT is built, then tone-mapped with the scene's exposure and gamma-encoded, or left linear on a camera frame's HDR scene; fog is not applied — DEVIATION). The kernel draws its random numbers from a PCG integer hash of the particle index and the emitter's step counter, so both backends draw the same random numbers (they agree to 4 counts on `particles-spark`). The option defaults are upstream's (scale 1, opaque white, BLEND_NORMAL, rate 1, initialVelocity 0). `screenSpace` (upstream SCREEN_SPACE, for a system under a screen-space element) uses the node's world transform as clip space with no view or projection, sizes quads in viewport heights with x scaled by height / width (#9570, also applied to the motion direction), turns off the depth test, and marks the mesh instance screen-space; `ScreenComponent::processDrawOrderSync` numbers a particle system with its elements, so on the UI layer it draws in hierarchy order. Component update hooks the engine "update" event; the emitter mesh instance sets `cull=false` (world-space particles ignore the node transform). Upstream's remaining options are on the GPU path too: `rate2` (each wrap takes the period of a random rate between rate and rate2, `graphParams.w`), `radialSpeedGraph`/2 (away from the emitter's centre, riding in the world velocity LUTs' w), `scaleGraph2` / `alphaGraph2` (per-particle random points, `scaleLut.yz`; `colorGraph2` is kept but, as on upstream's GPU path, never sampled), `wrap` + `wrapBounds` (world-space particles wrap into a box around the emitter, in the vertex stage), `orientation` WORLD / EMITTER with `particleNormal` (upstream's face tangent and binormal), `mesh` (any mesh in the packed 14-float vertex layout, drawn per particle and read as STORAGE by vertex index — Metal buffer 9, Vulkan set 6 binding 2 — turned about z then x by the angle, as upstream), `lighting` / `halfLambert` / `normalMap` (upstream's light cube: the scene ambient plus each directional light, AUTHORED colours as upstream, six directions in `lightCube`, Lambert or half Lambert on the quad's bulged normal or the normal map's TBN), `depthSoftening` (fades against the scene DEPTH GRAB — `CameraComponent::requestSceneDepthMap` — Metal texture 25, Vulkan set 3 binding 11 through the nearest sampler, off without one), `BLEND_NONE` (opaque, the opaque sublayer) and `sort` (DISTANCE / NEWER_FIRST / OLDER_FIRST: upstream's keys, sorted by the Slang program `particle-sort` (its uniform block in `particleSortShaders.h`) — ONE workgroup keys the pool and runs a bitonic sort over its next power of two with a barrier between stages, so it is one dispatch at any size — into a uint draw order the vertex stage reads at Metal buffer 8 / Vulkan set 6 binding 1). The render params (944 bytes) reach the FRAGMENT stage too (Metal buffer 11, Vulkan set 6 binding 3 visible to both stages). DEVIATIONS: GPU path only (no CPU sim; upstream sorts on the CPU, which forces its CPU path, and here sorts in compute, once a step for the active camera that renders first rather than the camera rendering the emitter), initial velocity/gravity/damping beside the graphs. The sim kernel, the sort and the billboard shader are Slang programs (`particle-sim`, `particle-sort`, `particle-render`), one source for both backends; the render program includes the forward pass's tone mapping operators. Sprite-sheet animation is `animTilesX/Y` + `animNumFrames` + `animIndex`, where animIndex selects WHICH animation in the sheet to play: each is animNumFrames tiles long and they run in reading order, so a 4x4 sheet at 4 frames holds four animations. Example: `particles-anim-index-example.cpp`, a port of upstream `graphics/particles-anim-index` (four emitters sharing one sheet, one animIndex each). `particles-spark` ports upstream `graphics/particles-spark` (its simulation measured against upstream's CPU updater: mean height by age within 0.1 at every age), `ui-particle-system` ports upstream `user-interface/particle-system` (screen-space sparkles and a claim burst in the UI draw order), and `render-to-texture` uses upstream's velocity curves. It has to call `options.registerComponentSystem<ParticleSystemComponentSystem>()` in `configure`: component systems come from `AppOptions::componentSystems`, so a component whose system no application registers is constructed and then never updated. `particles-mesh` (opaque lit, textured, motion-aligned torus particles; the torus is generated, as upstream's torus.glb carries no licence) and `particles-snow` (depth softening against the camera's depth map) port upstream's; `particles-random-sprites` is not ported.

### Parallax Occlusion Mapping
`VT_FEATURE_PARALLAX` (upstream `parallax.js`, plus the 2.22 additions): the height
map displaces every texture UV before any map is sampled, so colour, normal,
metal/rough, occlusion and emissive all read the displaced point. Turn it on with
`StandardMaterial::setHeightMap`; `setHeightMapFactor` is the displacement depth in TENTHS of a uv tile, upstream's
unit (default **0.1**, upstream's 2.22 value).

- **`setHeightMapBase`** is the height-map value that sits at the level of the
  geometry, upstream's meaning. Below it the field sinks into the polygon, above
  it the field stands proud. 1 is pure depth below the surface; the default
  **0.5** pivots the relief around mid-grey, as upstream's engine default does. The base moves the ray's
  ENTRY UV as well as the depths — see the gotcha in `AGENTS.md`, because shifting
  only the depths is a no-op that looks like it works.
- **`setHeightMapShadow`** (0..1, default 0 = off) marches the height field a
  second time toward the light and darkens texels the ray passes over, which is
  self-shadowing the cascade map cannot do because it only knows the flat polygon.
  DEVIATION: only the DIRECTIONAL light pays for it; local lights would each need
  their own march. The shadow march samples at an explicit LOD because it runs
  inside the light loop, behind fragment-varying control flow; the view march
  keeps implicit LOD and therefore the height map's mips.

The march itself is an adaptive 8-32 step search with a linear crossing solve, in
`forward/common-parallax.slang`.
Example: `parallax-mapping-example.cpp`, a port of upstream
`materials/parallax-mapping` — a closed brick room and a brick sphere under a spot
and an omni light.

### Wide Lines
`scene/graphics/wideLine.h` + `wideLineRenderer.h` (upstream
`extras/renderers/wide-line*.js`): connected polylines with a colour and a width
PER POINT, which a hardware line cannot do. `WideLine` holds the point data as
packed arrays; `WideLineRenderer` owns an entity and draws every line it has been
given in ONE instanced draw, one instance per segment, through the storage-draw
seam (`MeshInstance::setStorageDraw`). Lines of different widths, colours, caps,
joins and dash patterns stay in the same batch because all of it rides in the
per-segment record.

- Caps are butt, square or round; joins are miter, bevel or round; a line can be
  closed, and can carry a dash/gap/offset pattern measured along its own length so
  the pattern is continuous across segments.
- `setWidthUnits` picks screen PIXELS (the default) or world units. Either way the
  expansion happens in screen space after the projection, which is what keeps a
  pixel width constant with distance.
- The renderer uploads only when a line reports itself dirty, so `update()` every
  frame costs nothing while the lines are still, and a line whose points move does
  not reallocate its buffer unless the segment count grows.
- Example: `wide-line-example.cpp`, a port of upstream `graphics/wide-line`.
- Beyond upstream's renderer, for the transform gizmos' guide lines: a line has an
  `opacity` (written into the colour's alpha, which the shader now passes through — 1
  by default, so an opaque renderer draws exactly as before), and a renderer can be put
  on other layers (`setLayers`), alpha blended (`setBlend`, transparent sublayer) and
  drawn without the depth test (`setDepthTest(false)`). A wide line has NO near-plane
  clip: an end point behind the camera folds the screen-space expansion back across the
  view, so clip such a line on the CPU first (the gizmo does).

### App-facing Compute + Storage Draws
Upstream's `compute/particles` needs two things an application can reach: a compute
shader over app-owned storage buffers, and a draw that expands one instance per record
in the same buffer. Both exist on BOTH backends.

**`Compute` parameters** (`platform/graphics/compute.h`): alongside texture
parameters there are storage buffers (`setParameter(name, shared_ptr<VertexBuffer>)`
— `VertexBuffer` is the engine's generic GPU storage vehicle, so the same object also
binds to a draw) and loose scalars (`setParameter(name, float|uint32_t)`), collapsed into
one uniform block. `setThreadgroupSize` sets the threadgroup size (default 8x8x1, which the
edge-detect kernel uses).

**DEVIATION — no reflection.** Upstream reflects resources out of the WGSL source and
builds the bind group from the reflected names. This port has none, so binding indices
come from parameter NAMES in sorted order: buffers 0..b-1, textures b..b+t-1, the uniform
block at b+t, and the block's members are the scalars again in name order. A shader that
declares them in a different order silently reads the wrong data. A texture-only compute
binds its textures from index 0.

**Storage draws** (`MeshInstance::setStorageDraw(buffer, instanceCount, params, size)`):
the generic form of the emitter/gsplat draw branches — a custom shader reads the buffer,
keyed off the instance id. It SHARES their binding slots (`GraphicsDevice::setStorageDrawState`
forwards to `setParticleState`: Metal vertex slot 7 + params slot 11, Vulkan set 6 bindings
0 and 3), so one mesh instance is a storage draw, a particle draw, or a splat draw — never
two at once.

Example: `particles-example.cpp` (port of upstream `compute/particles` — 1M particles,
Verlet integration, three collision spheres; yellow on impact fading to red). DEVIATION:
upstream draws 6 indices per particle over a vertex-buffer-less mesh keyed on
`vertexIndex / 4`; this port draws one instanced tri-strip quad per particle, as the
engine's own emitter and splat paths do, which avoids a 24 MB index buffer.

### Gaussian Splatting (classic path)
`engine/src/scene/gsplat/` + `framework/components/gsplat/`: 3DGS PLY loading (`GSplatData::loadPly` — binary LE), CPU-precomputed covariance (Sigma = R·S²·Rᵀ) in a 40-byte `GpuSplat` storage buffer (vertex slot 7), background `GSplatSorter` thread (upstream sort-worker counting sort; order reversed farthest-first + behind-camera trim via instance count; the per-splat depth and key run in a 4-lane SIMD kernel, `scene/gsplat/gsplatSortKeys.h`, bit-exact against its scalar reference) filling ping-pong order buffers (slot 8), the Slang program `gsplat-render` (both backends: EWA screen-space covariance projection per upstream `gsplatCorner.js`, normExp falloff, premultiplied alpha, `CULLFACE_NONE` — screen-space quads have no winding) drawn as one instanced tri-strip quad per splat via a renderer branch keyed on `MeshInstance::gsplatInstance()`. Params at vertex slot 11. `GSplatComponent::setResource()` wires it to an entity.

**Tier 2:**
- **View-dependent SH** (bands 1-3): the generic PLY header parser reads `f_rest_*` (9/24/45 coeffs → bands 1/2/3), dequantizes them coefficient-major interleaved (`[c0.rgb, c1.rgb, ...]`, 45 floats/splat zero-padded) into a per-splat SH storage buffer (vertex slot 12). The shader evaluates `gsplatEvalSH` (upstream `gsplatEvalSH.js` basis) by the model-space view direction `normalize(transpose(mat3(modelView)) · viewPos)` and adds it to the DC color in display/gamma space before the sRGB→linear decode. `shBands` rides in `GpuGSplatParams` (runtime branch, no shader variant; SH0 assets bind a 1-float dummy at slot 12). DEVIATION: upstream quantizes SH to 11-10-11 in a texture; this stores raw floats.
- **Compressed `.compressed.ply`** (SuperSplat format): auto-detected by a leading `chunk` element. Per-256-splat chunk min/max bounds (12 or 18 floats) + a uint `vertex` element — 11-10-11 unorm position/scale lerped into the chunk box, 2-10-10-10 largest-component quaternion, 8888 color — dequantized through the SAME covariance/color path as uncompressed (`buildSplat` helper). Optional uchar `sh` element (channel-major, `u8·8/255−4`). ~4× smaller than float PLY. DEVIATION: no WebP-packed SOG format, no unified octree streaming/LOD path.

Example: `gsplat-example` (port of upstream `gaussian-splatting/simple` — a CC-BY-4.0 `tamiya-dt03.compressed.ply` capture on upstream's ground/PCSS-light/orbit setup), which exercises the compressed path. NO example covers SH bands 1-3: their nearest upstream counterpart, `gaussian-splatting/spherical-harmonics`, is the `simple` scene with a different asset. SH parsing is implemented and untested by any example — add one if you touch it. NOT ported: WebP SOG, unified octree streaming/LOD (~13k upstream lines).

### Spec-Gloss / Oren-Nayar / Detail Normals / Displacement / Clearcoat
Four `VT_FEATURE_*` material shader features, plus clearcoat:
- **Spec-gloss** (`VT_FEATURE_SPEC_GLOSS`, KHR_materials_pbrSpecularGlossiness): upstream's specular workflow, which is the DEFAULT (`useMetalness` false, as upstream). `setSpecular` (sRGB-authored, linearised into `specGlossParams.rgb`) + `setGloss` (+ `setGlossInvert`) + `setSpecGlossMap` (Metal only) — F0 = specular colour, roughness = 1-gloss, diffuse NOT scaled by the specular (upstream's combine adds albedo × diffuse light untouched). A material whose specular is black and has no spec-gloss map, clearcoat or metalness renders NO specular at all (`StandardMaterial::rendersSpecular`, upstream's `useSpecular` rule → `VT_FEATURE_NO_SPECULAR`), which is what a code-created `StandardMaterial` gets by default; call `setUseMetalness(true)` for the metal-rough workflow. The spec-gloss texture reuses the metal-rough binding (slot 3, rgb=sRGB specular, a=glossiness). The GLB parser sets `useMetalness` on every metal-rough material and applies KHR_materials_pbrSpecularGlossiness through `setSpecular`/`setGloss`, as upstream's extension handler does. The AMBIENT diffuse is scaled by `1 - specularity` under specular, as upstream's `LIT_SPECULAR` does (both chunks); the DIRECT diffuse is not. GOTCHA: the texture sample MUST be gated on the `hasSpecGlossMap` flags bit (21) — an unbound Metal texture on Apple GPUs reports nonzero `get_width()` but samples zero, silently zeroing specular/gloss for factor-only materials (same trap as the env-atlas bug).
- **Oren-Nayar diffuse** (`VT_FEATURE_OREN_NAYAR`): `setUseOrenNayar(true)` swaps Lambert `N·L` for the fast qualitative Oren-Nayar form (sigma² = roughness²) in both the multi-light loop and the clustered path.
- **Detail normals** (`VT_FEATURE_DETAIL_NORMALS`): `setDetailNormalMap` + `setDetailNormalScale` + `setDetailNormalTransform` — reoriented normal blend (the detail rotated into the base normal's frame) at fragment slot **23**, own UV transform.
- **Displacement** (`VT_FEATURE_DISPLACEMENT`): `setDisplacementMap` + `setDisplacementScale`/`setDisplacementBias` — vertex-stage height sampling (`level(0)`) displaces along the normal before skinning/morph composition. The map routes through a slot>=100 sentinel in `Material::getTextureSlots` to VERTEX texture slot 0 (`MetalTextureBinder`). DEVIATION: standard vertex path only (not instanced/dynamic-batch/skinned).
- **Clearcoat** (`VT_FEATURE_CLEARCOAT`, KHR_materials_clearcoat): `setClearCoat` / `setClearCoatGloss` / `setClearCoatBumpiness` plus the intensity (G), gloss (G) and normal maps on slots 7/13/14, flag bits 14/15/16, read at the base-colour and normal-map UVs. Upstream's composition on BOTH backends: the coat's per-light GGX (Kelemen visibility, F0 0.04) and its env-atlas reflection at the coat gloss accumulate separately and the tail composes `lit * (1 - Fc * cc) + (ccDirect + ccReflection) * cc`, so the coat takes energy from the base. Vulkan reads the three maps as separate images through the shared material sampler at set-1 binding 24. Parity is measured on `clearcoat` (the Khronos ClearCoatTest.glb): mean absolute difference 0.002 counts, 12 pixels of 630,000 above 8 counts. GOTCHA: that shared sampler has to carry the device anisotropy like the per-texture samplers do — without it the ribbed coat normal map alone moves ~1,900 pixels.
Example: `material-stubs-example.cpp` (four spheres A/B-cycling all four features with procedural textures).

### Scalar Material Maps (gloss / thickness / refraction)
`StandardMaterial::setGlossMap` + `setThicknessMap` + `setRefractionMap`, each with a
`set*MapChannel(MapChannel)` selector (upstream's `glossMapChannel` etc., default **G**
for all three so one packed texture can drive them). Each map multiplies its scalar
factor by one channel: gloss scales the gloss factor and REPLACES the roughness derived
from the metal-rough map (upstream treats them as alternative sources, not a product);
thickness and refraction scale `thickness` and `transmissionFactor` for both refraction
paths.

`area-light`'s floor has spatially varying gloss from its gloss map, which is the
intended upstream look.

Presence rides in the SIGN of `MaterialUniforms::mapChannelParams` (`{glossFactor,
glossChannel, thicknessChannel, refractionChannel}`, negative = no map) because the
material `flags` word has a single free bit (20) — 25-27 and 29-31 are the two dither modes.

These are fragment slots **31/32/33**. A `MaterialUniforms` field is one line in
`scene/materials/materialUniformFields.h` (an X-macro list); the C++ struct and the Slang
`MaterialData` in `bindings.slang` are both emitted from it (see "Adding a `MaterialUniforms`
field" in AGENTS.md). A bundle generator failure stops the build with a Python `RuntimeError`,
not a compiler `error:`, so a grep for "error:" misses it.

Both backends. On Vulkan the three maps (and the opacity map, 34) are set-1 bindings
31-34, SEPARATE images read through the shared material sampler at 24, since the
fragment stage is at MoltenVK's 16-sampler limit. They sample UV0 with no transform of
their own (the gloss map has no tiling).

Example: `refraction-example.cpp` (port of upstream `materials/material-refraction`).

### Material System
- `Material` base: glTF PBR metallic-roughness, `MaterialUniforms` struct (560 bytes, emitted from `materialUniformFields.h`) matches GPU `MaterialData`
- `StandardMaterial`: Full PBR (clearcoat, anisotropy, sheen, iridescence, transmission, parallax)
- `ShaderMaterial`: Custom Metal shader with user entry points

## Engine subsystems
They are reference, not rules; the
rules they imply are repeated in the gotcha list of `CLAUDE.md`.
## Examples harness (`ExampleApp`)

Every example derives from `ExampleApp` (`examples/exampleApp.h/.cpp`), which owns
the SDL window, the graphics device, the Engine and the frame loop. Hooks run in
the order `configure()` → `create()` → [`update()` → `preRender()` →
`postRender()`]* → `destroy()`; only `create()` is pure virtual. `destroy()` also
runs when `create()` fails, and always while the engine is still alive — anything
holding a borrowed Engine or GraphicsDevice pointer must be released there rather
than in a derived destructor.

Two backend details live there and nowhere else, which is why no example carries a
backend `#ifdef`:
- The window must be created for the backend already chosen, so `ExampleApp`
  resolves it up front through `defaultBackend()` (VISUTWIN_BACKEND override, else
  Vulkan when compiled in, else Metal). The SDL renderer is created ONLY on the
  Metal path; over a Vulkan window it would be a second, competing presenter.
- `exampleApp.cpp` is the metal-cpp `*_PRIVATE_IMPLEMENTATION` translation unit for
  every example. The engine library deliberately has none, so exactly one TU per
  executable must define those macros (`tools/generate-env-atlas.cpp` and
  `tests/vulkanSmoke.cpp` carry their own).

**Pixel density and antialiasing, as upstream's examples.** The examples render at ONE pixel
per point on every display (`setMaxPixelRatio(1)`): a Retina window is drawn at 900x700 and
scaled up by the system, the density upstream's examples get in a browser by default. The
back buffer is multisampled 4x on every display (`GraphicsDeviceOptions::antialias`, on by
default as upstream's device), and an example whose upstream counterpart creates its device
with `antialias: false` says `ExampleOptions::antialias = false`: `gsplat`, `depth-of-field`,
`post-processing`, `taa` and `pcss-dither` (splats, or a camera frame that antialiases its own
scene target). A camera frame's `rendering.samples` and a render target's `samples` are as
upstream sets them. `VISUTWIN_MAX_PIXEL_RATIO` raises the density and `VISUTWIN_ANTIALIAS=0/1`
overrides the back buffer's MSAA. The log names both ("Back buffer pixel ratio", "Display
pixel density D (back buffer Nx MSAA)"), and the golden script keys its reference sets on the
display density; both sets hold the same images today.

The harness also owns the **performance HUD**, for the same reason: upstream's
example harness puts ministats on every example, so it belongs to the host and no
example carries a line for it. `ExampleApp` loads the two Roboto MSDF fonts, holds the
`MiniStats` (`framework/extras/miniStats/`), passes it every SDL event before the
example sees it, and toggles it with F1, which is free where every useful letter is
already some example's binding.

**It is drawn by the engine, as upstream's is**: one `Render2d` quad list, one mesh
instance, one material, one draw, on the UI layer. Three pieces:

- `Render2d` (`render2d.h`, the Slang program `render2d`): solid rects,
  MSDF glyphs from pages 0 and 1 of a regular and a bold font (material slots 0, 1, 4,
  5, combined samplers on both backends), and graph rows (slot 3). The vertices reuse
  the 56-byte packed layout (`position.z` is the mode, the normal the colour). Colours
  are display space, written as they are. The list lives in its own `UiGeometryArena`
  and is committed only when it changes; a graph scrolls through a uniform cursor
  instead of rewritten vertices (DEVIATION), because a vertex rewritten in place on
  Metal lands in memory the frames in flight still read. The mesh instance is
  `setDrawOncePerFrame`: the first camera to render the UI layer draws it, the others
  skip it (the forward pass claims it as it collects it, `claimDrawThisFrame`).
- `MiniStatsText` places glyphs with text layout's formula, kerned, cut at a width
  with the texture cut too, and ended with an ellipsis (page 1 in Roboto). DEVIATION:
  upstream rasterises system fonts into its own atlas.
- `MiniStatsGraph` / `MiniStatsHistory`: upstream's Graph (the mean and peak over
  `textRefreshMs`, a history column per frame, a count row's scale growing and
  rescaling what it holds), over one RGBA8 history texture uploaded once a frame into
  the next of `maxFramesInFlight()` textures, for the same Metal reason.

- **Somebody has to render the UI layer.** `ExampleApp` adds a camera that renders
  ONLY the UI layer, last and clearing nothing, and enables it each frame only when no
  active camera of the example renders that layer over the whole back buffer
  (`syncUiCamera`; `multi-view` is the one example that needs it, and the log says so).
  A UI-layer camera with a sub-rectangle still draws the HUD into its rectangle.
- It is SUPPRESSED while `VISUTWIN_SCREENSHOT` is armed, so golden and parity
  captures carry no panel and get no extra camera. `VISUTWIN_MINISTATS=0/1` overrides
  that either way, and `1` is the only way to capture a screenshot WITH the HUD;
  `VISUTWIN_MINISTATS=detailed` opens the size with graphs.
- **Upstream's three sizes**: compact counters (draw calls, frame, CPU, GPU, VRAM),
  grouped averages, and grouped averages with peaks and history. A click goes to the
  next size; in a detailed size a click on a heading collapses its section, and the
  wheel scrolls a panel taller than the window. `MiniStats::handleEvent` takes a press
  on the panel, its release and the wheel steps it uses, and `ExampleApp` returns
  without passing those to the example; the camera controls, which read the mouse
  DEVICE (fed first, unconditionally), are blocked while `capturesPointer()`.
- **The per-pass rows are keyed by pass NAME, summed and aged.** Passes sharing a
  name are one row holding their sum (the forward pass draws the scene and then the
  UI layer; a separable blur runs twice) — keyed on the last of them the row would
  show the 0.1 ms UI pass and hide the 3 ms scene pass. A row first appears when its
  pass reports more than zero and is dropped 240 frames after it last did, or a
  one-shot shadow pass keeps its first-frame figure forever.
  `MiniStatsOptions::gpuPassTimings` replaces the device's profiler as their source,
  which is how `tests/miniStatsTests.cpp` drives creation, aging, row reuse and texture
  growth on a stub device that has no profiler.
- **The CPU row is upstream's CpuTimer**: the update phase plus the render phase on
  the CPU, both of the PREVIOUS frame. At "postrender" the engine's update time is this
  frame's and its render time the previous frame's (written after the hook), so
  `latchCpuTimes` holds the update and physics figures back one hook to pair them with
  their own frame's render; summed as they stand, one frame's update spike lands beside
  another frame's render and the peak column shows a frame that never happened. The
  Update, Render and Physics rows read the same latch and add up to the CPU row. The
  render half is `FrameStats::renderTime`, which
  `Engine::render` writes as its own wall time LESS
  `GraphicsDevice::displayWaitMilliseconds()` — the time the backend spent blocked on
  the display (Metal's `nextDrawable`; Vulkan's frame fence, acquire, submit and
  present). Under vsync that wait is the frame pacing and it happens INSIDE
  `render()`: MoltenVK takes the drawable at the queue SUBMIT that renders into it, so
  timing only the acquire would leave the Vulkan CPU row echoing the frame time.
  DEVIATION: the CPU section's rows are Update, Render and Physics (upstream: script
  update and post-update, animation, physics, render, splat sort), the VRAM parts are
  always textures, geometry and buffers, and the Resources rows count the device's
  live textures, render targets, vertex, storage and index buffers, shaders, and render
  and compute pipelines (`GraphicsDevice::liveResourceCounts`). Shaders are COUNTED, not
  registered: each `Shader` co-owns the device's counter, so one that outlives the device
  still decrements live memory. The pipelines are the backend caches' entries
  (`addBackendResourceCounts`); those caches never evict, so the rows are every distinct
  pipeline state the run has needed, which is where a variant explosion shows.
- **Stat presets** (`MiniStatsOptions::statPresets`, upstream's `getDefaultOptions`
  extras): "gsplats" adds a GSplats row to Engine, `FrameStats::gsplats` in millions with a
  budget of 10. Upstream's "gsplatsCopy" counts buffer copies of its unified splat
  renderer, which this port does not have. An example asks for presets through
  `ExampleOptions::miniStatsPresets`; none does yet, because every upstream example that
  asks for one (billions, depth-effects, downtown, flipbook, lod-streaming, relighting,
  weather) is unported, and `gsplat-example` ports `simple`, which uses the default panel.
- **The first frame after the HUD is created or shown samples nothing.** It has no
  previous hook to measure the frame interval from, and its CPU figures belong to a frame
  the HUD never saw; sampled, it put a zero frame time into the averages and the history.
  It still refreshes the panel and the resource counts.
- **The HUD leaves the GPU profiler as it found it.** It enables the profiler while
  shown and, hidden or destroyed, puts back the state it had when the HUD was created or
  last shown, so a caller that enabled the profiler for its own use keeps it.
- The HUD is torn down BEFORE the engine: MiniStats unhooks itself from `postrender`
  and takes its quads off the UI layer, then its fonts go.
- It costs about 0.01 ms of CPU a frame compact and 0.05 ms with graphs (median,
  `postRender` timed directly on `ambient-occlusion`; frames that rebuild the quad
  list reach 0.04 / 0.15 ms).

`visutwin_add_example(<name>)` builds `src/<name>-example.cpp`. Adding an example
is one source file and one line.

## Layers and depth state

`LayerComposition::insert(layer, index)` places BOTH sublayers of a layer at a
position — pair it with `getTransparentIndex` / `getOpaqueIndex` to slot a layer
relative to another. Sublayer order maps are rebuilt in `updateLayerMaps` rather
than maintained incrementally. `pushOpaque` / `pushTransparent` still append.

`DepthState::setFunc(CompareFunction)` is the depth comparison (upstream
`Material.depthFunc`), default `LessEqual` — the skybox needs LessEqual at cleared
depth 1.0. `CompareFunction` is an alias of the existing `StencilCompareFunction`
so both backends share one conversion helper. Metal keeps its four prebuilt
LessEqual states and routes any other function through the depth/stencil cache
(keyed on the function too); Vulkan reads it in `vulkanRenderPipeline` and the
PSO key covers it via `DepthState::key()`. `Greater` + `depthWrite(false)`
is the x-ray trick in `layers-example` — the mesh draws only where something
already rendered in front of it.

A layer with `setClearDepthBuffer(true)` clears depth before it draws, as upstream,
even when its render action is not the first of its forward pass: `RenderPassForward`
clears the pass's depth at the start of the pass for its FIRST action (the load action)
and, for a later action whose LAYER asks for it, mid-pass with `clearDepthInPass`
(`localShadowFace.h`) — the atlas clear's depth-1 triangle under ALWAYS with every
colour write masked off, within the current viewport. Upstream's in-pass
`renderer.clear` does the same on WebGPU. DEVIATION: a later action's colour or stencil
clear, and a camera's own clear on its first action in a pass shared with another
camera, are not performed mid-pass. The gizmo layer and `layers`' front layer rely on
it.

## ECS

- `GraphNode` -> `Entity` -> Components via `ComponentSystem<T>` registry
- O(1) component lookup via `unordered_map<ComponentTypeID, Component*>`
- `GraphNode::lookAt(target, up = +Y)` aims the node's -Z at a world-space target,
  setting WORLD rotation. Some examples carry local yaw/pitch
  helpers instead; those are equivalent.
- **18 component types:** Camera, Render, Light, Script, Animation, Anim (state
  graph), Screen, Element, Button, LayoutGroup, LayoutChild, Scrollbar, ScrollView,
  Collision, RigidBody, Joint, GSplat, ParticleSystem
- **Script phases.** Per frame: systems `update`, scripts `update`, `animationUpdate`,
  systems `postUpdate`, scripts `postUpdate` — so a parameter a script sets in `update`
  drives the animation of the same frame. `update`, `postUpdate` and `fixedUpdate` each walk their own
  execution-ordered list of script components — the ones with a script that overrides
  that method, decided per script TYPE at compile time (`Script::phasesOf<T>()`) and
  stamped on the instance by the registries' factories (`Script::make<T>()`). `initialize`
  and `postInitialize` walk every component. A script from a hand-written factory that
  does not use `Script::make` is treated as implementing every phase.
- **Component systems are supplied by the APPLICATION**, not by the engine:
  `Engine` registers whatever `AppOptions::componentSystems` carries. The examples
  harness registers Render, Camera, Light and Script; anything else is one
  `options.registerComponentSystem<T>()` line in the example's `configure`. A
  component whose system nobody registered still constructs — it just never gets
  its per-frame update, so it looks implemented and inert at the same time.

## UI screens and elements

Upstream's `ScreenComponent` and `ElementComponent` layout
(`framework/components/screen`, `framework/components/element`).

- **Screen.** `setScreenSpace(true)` makes the resolution the canvas size
  (`Engine::canvasSize()`, window points), refreshed by the screen system on
  `update`. `setReferenceResolution` + `setScaleMode(ScreenScaleMode::Blend)` +
  `setScaleBlend` give upstream's log-space blend of the two axis ratios; with
  `None` (the default, and forced for world-space screens) the scale is 1. The
  screen matrix is `ortho(0, w, -h, 0)` over `resolution / scale` — model space
  has its origin at the screen's top-left with y up — and a world-space screen
  additionally scales it by (w/2, h/2) and composes the screen entity's world
  transform, so its elements are ordinary world geometry in screen units.
- **Element.** Anchors are fractions of the parent element's rectangle (or the
  screen's), pivot is the point the position names, margins are left, bottom,
  right, top from the anchors. `ElementComponent` is a `GraphNodeTransformHook`
  (`scene/graphNodeTransformHook.h`): while it has a screen its entity's world
  transform is `screenMatrix * parentModel * anchorTransform * local`, and
  `setPosition` / `setLocalPosition` re-derive the margins. Corners come three
  ways: `screenCorners` (screen units, y up — for a screen-space screen, canvas points,
  the space ElementInput hit-tests in), `canvasCorners` (the same, y down) and
  `worldCorners`.
- **Input** (`framework/input/elementInput.h`, `elementInputEvents.cpp`; upstream
  element-input.js). `Engine::handleInputEvent` hands every SDL event
  to `ElementInput::handleEvent`, which calls the platform-neutral `onMouseDown/Up/Move`,
  `onMouseWheel` and `onTouchStart/Move/End/Cancel` in canvas points. Elements with
  `useInput` that are `active()` receive `mousedown`, `mouseup`, `mousemove`,
  `mousewheel`, `mouseenter`, `mouseleave`, `click` and the touch events as an
  `ElementInputEvent*`, bubbling to parent elements until `stopPropagation()`. Upstream's
  rules: the element under a mouse-down is PRESSED and takes every move and the release;
  `click` fires on release over the pressed element; a touch that ends over its element
  clicks, one that leaves fires `touchleave` once, and a mouse click within 300 ms of a
  touch click on the same element is dropped (the platform's echo). Targeting walks the
  cameras from the last drawn back and, per camera, the elements on layers it draws,
  sorted by transparent layer order, then screen-space before world-space, then draw
  order; a screen-space element is hit by a ray into its screen corners, any other by a
  near-to-far ray through its world corners (the nearest wins, one on a world-space screen
  at once). A button's `hitPadding` grows the corners along the entity's own right and up,
  scaled by the screen and local scales (`ElementInput::buildHitCorners`). SDL's mouse
  events synthesized from touches are dropped and only DIRECT touch devices count (a
  trackpad's fingers are not touches; the `TouchDevice` filters the same way, and reports
  window points as the mouse does). Wheel deltas are taken as SDL gives them: SDL has
  already applied the system's natural-scrolling setting and only flags a FLIPPED device,
  as a browser's deltas are already adjusted. DEVIATION: `touchcancel` never clicks (upstream runs
  it through its touchend handler). `ElementInput::handleEvent` returns whether a handler
  stopped an event, and `Engine::handleInputEvent` then withholds that SDL event from the
  mouse and touch devices (upstream's `stopImmediatePropagation`); `MouseEvent::fromTouch`
  marks SDL's touch-synthesized mouse events for code that listens to both devices.
  `CameraComponent::screenToWorld` (upstream's: perspective `z` is the distance along the
  ray, orthographic `z` a fraction of the clip range) takes canvas points.
- **Buttons** (`framework/components/button`, upstream button/component.js). A button
  follows its entity's element and shows DEFAULT / HOVER / PRESSED / INACTIVE on the IMAGE
  element of its `imageEntity`: `ButtonTransitionMode::Tint` replaces the image's colour and
  opacity with `hoverTint` etc. (faded over `fadeDuration` MILLISECONDS, advanced by the
  engine's frame time — DEVIATION: upstream uses the wall clock), `SpriteChange` shows
  `hoverSprite` / `hoverSpriteFrame` etc. (a null sprite keeps the image's — DEVIATION, as
  the state sprites are Sprites, not assets). The image's own look is the default: the
  element fires `set:color`, `set:opacity`, `set:sprite` and `set:spriteFrame` on a change,
  and the button stores what the APPLICATION sets. An active button re-fires its element's
  events and fires `hoverstart/end` and `pressedstart/end`; `setActive(false)` shows the
  inactive state and silences it (the getter is `isActive()`, since `active()` is
  Component's). Adding a component does not call `onEnable` here, so the button binds in
  `initializeComponentData` and the button system re-checks both elements every update
  (upstream listens for `element:add`). `tests/elementInputTests.cpp` holds the input and
  ports upstream's button tests; `ui-buttons` is upstream's rebuilt buttons example.
- **Drawing.** `ElementInput::syncElements` (called by `Engine::render`) keeps one
  visual per text or image element: a child entity with an identity transform, on
  `ElementComponent::layers()` (empty = LAYERID_UI on any screen, WORLD on none —
  DEVIATION for the last, upstream's default is UI for both). On a screen-space screen
  the mesh instance is `setScreenSpace`, so the vertex stage writes world xy straight
  to clip, with no depth test. The material is emissive-only (colour x texture, alpha
  from the texture), and the visual is rebuilt only when its inputs change: the text
  fields, or the image's `imageVersion` plus its sprite's and atlas's versions; a change
  of size or pivot alone gives the existing parts new geometry and keeps their mesh
  instances and materials.
- **What visuals share.** Geometry: every part's vertices and indices are a run inside
  the `UiGeometryArena`'s shared buffers (`framework/input/uiGeometryArena.h`; chunks of
  2,048 vertices doubling to 65,536, a `RangeAllocator` per chunk for each of vertices
  and indices, indices stored absolute as 32 bits), written through
  `VertexBuffer::writeRange` / `IndexBuffer::writeRange`; a run given back is reusable
  after `maxFramesInFlight` frames. Materials: parts whose `ElementInput::MaterialKey`
  (kind, space, texture, colour, opacity, MSDF range, outline, shadow) are equal draw
  with one `StandardMaterial`, held weakly in a map by key; a part alone with its
  material restyles it in place. So consecutive UI draws keep the same buffers bound,
  and the renderer skips the material bind between draws of one material. Each element
  carries its visual's record (`drawRecord`), so the per-frame pass does no lookup and,
  for an element nothing changed on, no allocation. The visual entity is excluded from
  `Entity::clone`.
- **Images.** `setTexture` + `setRect` (x, y from the bottom, w, h as fractions), or
  `setSprite` + `setSpriteFrame`; each clears the other, as upstream. `setFitMode`
  (stretch, contain, cover) shrinks the quad about the pivot. A `Sprite`
  (`scene/sprite.h`) names frames of a `TextureAtlas` (`scene/textureAtlas.h`, rects in
  pixels from the image BOTTOM, 9-slice borders left/bottom/right/top); a SLICED sprite
  keeps its borders at pixels / pixelsPerUnit (`setPixelsPerUnit` overrides it per
  element) and shrinks the whole grid when the element is narrower than twice its
  left border, as upstream. Both are data only here: there is no sprite component, and
  the grid is built on the CPU (`imageElementGeometry.h`, DEVIATION from upstream's
  vertex-shader slicing, exact against it). TILED repeats the frame's inner region at its
  natural size over the centre (both axes) and the edge strips (along their length), from
  the inner region's left and bottom edges, the last tile cut short — one quad per tile
  (`buildTiledImageGeometry`; DEVIATION from upstream's per-fragment tiling in
  `startNineSlicedTiled`, and `tests/imageElementGeometryTests.cpp` holds it against a
  literal port of that fragment code at 238 probes a case).
- **Layers.** An element with no `layers` of its own draws on LAYERID_UI when it is on a
  screen of either kind, as upstream, so a world-space screen's elements keep their draw
  order (they are depth-tested against the scene); on no screen it draws on WORLD.
- **Draw order.** `ScreenComponent::processDrawOrderSync` numbers the elements under
  the screen depth-first from 1 (priority << 24 on top), queued by binding, unbinding and
  `setPriority` and resolved by the screen system's update; the UI layer's transparent
  sublayer is SORTMODE_MANUAL on it.
- **Text properties:** `setSpacing` (a multiplier on every advance, kerning included;
  default 1), `setLineHeight` (default the font size), `setAutoWidth` /
  `setAutoHeight` (default on; ignored on a split axis), `setWrapLines` (wraps only at a
  fixed width), `setEnableMarkup`. `textSymbols()` / `markupTags()` / `textWidth()` /
  `textHeight()` expose the laid-out text. Markup tags: `[color="#rrggbb"]`,
  `[outline color="#.." thickness=".."]`, `[shadow color="#.." offset=".." offsetX=".."
  offsetY=".."]`, nested and merged innermost-last; `\[` is a literal bracket; an error
  (unclosed tag, bad syntax) logs a warning and draws the text as written.
- **Text layout** is upstream's: glyph metrics scale by `fontSize / 32` (the fonts' em;
  each glyph also divides by its own `scale`), lines step by `fontSize`, the text's width
  is the furthest any symbol's advance reaches (whitespace included), a line closed by a
  line break or a wrap aligns by its width WITHOUT trailing whitespace while the last line
  aligns by its whole advance, an empty text is laid out as one space (it keeps a line's
  height), a missing character takes the space's advance, and the block — from the font's highest glyph top to its lowest bottom (the
  glyph `bounds`) — is placed by `verticalAlign` (default 0.5). Text is laid out per
  CODE POINT, decoded from UTF-8 (malformed bytes become U+FFFD, drawn as the space).
  `setJustify` (upstream `justify`) stretches a line broken at a word wrap flush to both
  edges by widening its word gaps evenly, ignoring the horizontal alignment; lines ended by
  a line break, the last line and a word broken mid-word keep the alignment
  (`TextLine::gaps`). `setRangeStart` / `setRangeEnd` (upstream's) draw only the symbols in
  the range by narrowing each text part's index range to its quads (each part records the
  symbol of every quad), with no new layout; laying the text out again resets the range to
  the whole text.
- **Text.** Fonts are upstream's JSON format with one image per page (`<name>.png`,
  `<name>1.png`, ...). A font whose glyphs carry `range` is MSDF: pages are kept raw and
  bilinear, `pxRange` is scale x range and `intensity` comes from the file. The visual
  builds one mesh per page and a material with `setMsdfMap(page)` +
  `setMsdfFont(pxRange, intensity)`; the shader derives the transition width from
  `fwidth(uv)` and the page size in `MaterialUniforms::msdfParams` (floored at 2.5 screen
  pixels, as upstream). `setOutlineColor` / `setOutlineThickness` (0..1) and
  `setShadowColor` / `setShadowOffset` are upstream's element properties and scaling.
  UI materials are NOT tone mapped: text and image materials turn `useTonemap` off
  (`VT_FEATURE_NO_TONEMAP`), so on a gamma target the fill takes TONEMAP_NONE — neither
  the curve nor exposure — and only the gamma encode, as upstream's element materials
  (`useTonemap = false`, and `useFog = false`). Under a camera frame the forward pass still
  writes linear HDR as for every material. Outline and shadow colours are composited after
  the fill's (absent) tone mapping, in linear, as upstream applies MSDF after its tone
  mapping. A font without
  `range` is a bitmap font: coverage in alpha, nearest filtering.
- **Masks** (upstream `_updateMask`). `setMask(true)` on an IMAGE
  element makes it write the stencil instead of colour: its material keeps alpha test 1
  (`AlphaMode::MASK`, cutoff 1.0, so a sprite's transparent texels are outside the mask),
  turns every colour write off and draws in the transparent sublayer without depth
  write. `ElementInput::syncMasks` walks each element tree depth-first, every frame in
  which some element is a mask and once more after the last one goes: a
  mask under no other mask draws ALWAYS / REPLACE with its depth as the reference
  (starting at 1), a nested one EQUAL parent / INCREMENT_CLAMP, and every element below a
  mask EQUAL / KEEP against it (`maskedBy()`). Each mask gets a second mesh instance on
  its visual, the UNMASK — the same mesh and material, EQUAL (parent + 1) /
  DECREMENT_CLAMP — drawn after its last descendant at that element's `drawOrder` plus an
  offset that starts at 0.5 and falls by 0.001 per request in the frame, so where masks end
  on the same element the inner one unmasks first. That is why `MeshInstance::drawOrder` is
  a `double` (upstream's is a number). The stencil state rides on the mesh instance
  (`setStencil(front, back)`) and the renderer applies it per draw, resetting it after the
  layer. A masked element is hit by input only where its mask chain is hit too
  (`checkElement`). The stencil comes from the BACK BUFFER's depth attachment, which is
  depth-stencil on both backends (see AGENTS.md). DEVIATION: the walk runs every frame,
  where upstream re-runs it on a hierarchy or mask change; it touches only the stencil
  parameters, which are shared per (function, operation, reference). `masking` ports
  upstream's example: a card mask holding a panning photo in a cover mask and a circle
  avatar mask.
- **Layout groups** (`framework/components/layoutgroup`, `layoutchild`; upstream layout-group
  and layout-child). A `LayoutGroupComponent` lays out the element children
  of its entity in a row or column (`setOrientation`), wrapping into more (`setWrap`), fitting
  them to its size per axis (`LayoutFitting::None / Stretch / Shrink / Both`), and placing the
  block by `setAlignment` (x 0 left to 1 right, y 0 bottom to 1 top; default (0, 1)),
  `setPadding` (left, bottom, right, top) and `setSpacing`; `reverseY` is ON by default, so rows
  stack from the top. A child takes part while its entity and element are enabled; a
  `LayoutChildComponent` gives it min/max sizes, a share of what a fit hands out
  (`fitWidthProportion`) or `excludeFromLayout`. The group resets each child's anchors to zero
  and sets its calculated size and local position, then fires `reflow` with the bounds (x, y,
  width, height). The arithmetic is `calculateLayout` (`layoutCalculator.h`), a pure function
  over plain structs; the component gathers the items and applies the result. DEVIATION: the
  system reflows a group whose inputs changed since its last reflow (compared after every
  update) instead of listening for upstream's events; see AGENTS.md.
- **Dragging** (`element/elementDragHelper.h`, upstream element-drag-helper.js). An
  `ElementDragHelper` on an element with `useInput` moves its entity by the pointer's travel
  in the element's plane, in its parent's units (the screen's scale and every ancestor's scale
  divided out), optionally along one axis (`DragAxis`), firing `drag:start`, `drag:move` (the
  new local position) and `drag:end`. Its owner deletes it before the element goes.
- **Scrollbars** (`framework/components/scrollbar`, upstream scrollbar). The track is the
  entity's element, the handle `handleEntity`'s; the scrollbar sizes the handle to `handleSize`
  of the track and places it at `value` (0 to 1, from the left, or DOWN from the top when
  vertical), and a drag helper on the handle sets `value`, fired as `set:value`. A handle
  smaller than its track is a slider (`common-widgets`).
- **Scroll views** (`framework/components/scrollview`, upstream scroll-view). A content element
  in a viewport element (usually a mask), dragged (a drag helper on the content), flicked
  (velocity decaying by `friction` per frame, as upstream), wheeled (`wheelPixelsY` over the
  content's height) or driven by its scrollbars. `scroll` runs 0 to 1 per axis, from the top on
  y; `ScrollMode::Clamp` stops at the ends, `Bounce` goes past them with a log10 tension while
  dragged and springs back by `bounceAmount`, `Infinite` has none. A drag past `dragThreshold`
  turns off `useInput` on every element under the content until it ends. `set:scroll` fires on
  every change. DEVIATIONS: the unset properties upstream leaves undefined default to both axes,
  Bounce, 0.1 and 0.05; and a wheel notch is 100 pixels. `layout-group`, `scroll-view` and
  `common-widgets` port upstream's examples; `tests/uiLayoutScrollTests.cpp` holds all four
  parts through the real engine and ElementInput.
- **Auto fit and max lines** (upstream's). `setAutoFitWidth` /
  `setAutoFitHeight` shrink the font from `maxFontSize` (32) down to `minFontSize` (8) until the
  text fits the element's width / height; each works only while the matching autoWidth /
  autoHeight is off. A width overflow scales the size to `floor(size x width / text width)`, a
  height overflow takes one off, and the layout runs again; the line height scales by the
  fitted size over maxFontSize. `fontSize()` reports the fitted size, `setFontSize` the size
  used when no fit is on (upstream's `_originalFontSize`). The fit follows the element: a new
  width or height lays the text out again. `setMaxLines(n)` stops a WRAPPING text breaking lines
  once it has n, and the rest runs on in the last one, past the width; text that does not wrap
  ignores it, as upstream. `'\r'` breaks a line as `'\n'` does (upstream LINE_BREAK_CHAR), and
  a line break run on into the last line draws and advances nothing. One
  `ElementComponent::measureLayout()` measures for the element and its visual alike.
  `tests/textFitTests.cpp` ports upstream's auto-fit and maxLines cases on the Roboto font, with
  "the largest size that fits" as the oracle where upstream's expectations are its test font's.
- **Custom materials** (upstream image element `material`).
  `ElementComponent::setMaterial` makes an image draw its quad with that material (a
  `ShaderMaterial`, say) instead of its own; the element's colour, opacity and texture are then
  the material's affair, as upstream, and a mask ignores it. The quad's UVs run v DOWN the
  element; on a screen-space screen the model matrix maps it straight to clip space, so a custom
  vertex stage writes `model * position` with z 0.5 (upstream's GL 0 after the remap). The UI
  layer draws only transparent materials in its sorted sublayer, so the material wants
  `setAlphaMode(BLEND)`. `ui-custom-shader` ports upstream's cooldown example, its shader in MSL and
  GLSL and its two uniforms as one `customUniformData` block.
- **Localization** (upstream `framework/i18n`). `Engine::i18n()` is upstream's
  `app.i18n`: a locale (`setLocale`, "change" event with the new and old locale), messages added
  from upstream's JSON format (`addData` / `addDataFromFile`, parsed with nlohmann/json and
  validated always, not only in debug; `removeData`), `getText`, `getPluralText` with upstream's
  CLDR rules per language, and `findAvailableLocale` with its fallback chain (the locale, its
  DEFAULT_LOCALE_FALLBACKS entry, its language's, the first locale added for the language,
  en-US). A text element's `setKey` makes its text the message of that key; it subscribes to
  the i18n events the first time a key is set (a component added to a live entity gets no
  onEnable here), follows the locale and data added later, and `setText` clears the key, as
  upstream. `tests/i18nTests.cpp` ports upstream's i18n test suite less its asset cases.
  DEVIATIONS: no localization assets (data is added directly), no per-locale font swap, and a
  plain-string message is its own single plural form where upstream's getPluralText returns one
  character of it. `text-localization` ports upstream's example; C++ has no Intl, so its price
  is formatted by a small function that writes what the browser's Intl writes for the four
  locales it reaches (U+00A0 before the symbol).
- **Not ported:** right-to-left text.

## Graphics abstraction

- `GraphicsDevice` (abstract) -> `MetalGraphicsDevice` / `VulkanGraphicsDevice`
- Triple-buffered ring buffers for uniforms
- Per-pass texture/uniform binding deduplication
- Pipeline state caching via `MetalRenderPipeline` / `VulkanRenderPipeline`
  (+ `MetalComputePipeline`)
- **Command buffers.** Both backends record a frame into one command buffer. Vulkan's
  is the frame's; Metal's is `MetalGraphicsDevice::openCommandBuffer()`, created on
  demand and shared by render passes, blits, mip generation, compute dispatches, the
  particle simulation and the HUD, committed at frame end with the present attached —
  and earlier, without waiting, after a pass that leaves 256 draws or 300,000 vertices
  in it, so the GPU can start on them. Anything that reads a result back or submits a
  buffer of its own calls `flushCommands()` first. A compute dispatch made outside a
  frame waits in the open buffer for the next frame end on both backends.
- **Ranged buffer writes.** `VertexBuffer::writeRange(offset, data, size)` and
  `IndexBuffer::writeRange` overwrite part of a buffer, CPU copy and GPU: Metal copies
  into the shared storage, Vulkan stages just that range. `setData` re-sends the whole
  buffer. The UI geometry arena is the user.
- **Capability queries.** One list, on the base class, so both backends answer the
  same questions and a caller never guesses. Dimensions: `maxTextureSize()` and
  `maxCubeMapSize()` (Metal derives them from the GPU family — 16384 from Apple3 /
  Mac2, 8192 below; Vulkan reads `maxImageDimension2D` / `maxImageDimensionCube`),
  defaulting to 4096 so a backend that answers nothing behaves as the literals these
  replaced. Filtering: `maxAnisotropy()`, 16 on Metal and `min(16, device limit)` on
  Vulkan, read by Metal's sampler cache and by every Vulkan per-texture
  sampler (both built from `textureSamplerState`) so the two backends filter oblique surfaces identically. Formats:
  `textureHalfFloatRenderable()` / `textureFloatRenderable()` — whether a float
  colour format can be an ATTACHMENT, which is what `SHADOW_VSM_16F` needs and falls
  back to PCF3 without; both default to FALSE, so an unanswered capability loses a
  feature rather than allocating a target the driver refuses. Plus `maxSamples()`,
  `maxFramesInFlight()`, `supportsCompressedFormat()`,
  `supportsDualSourceBlending()`, `supportsCompute()`,
  `supportsGpuInstanceCulling()` and `supportsTimestampQuery()` (derived from
  `gpuProfiler()`, which both backends construct only where timestamps exist).
  Consumers: both lightmappers and the skybox cube bake size their textures against
  the dimension limits, `ShadowMap::create` and `Light::setShadowResolution` clamp
  the shadow resolution against them, and `Light::setShadowType` keys the VSM
  fallback on the half-float answer.
- **Readback.** `Texture::read(out, x, y, w, h, mip, face)` is the public seam —
  upstream's `Texture#read`, blocking rather than promise-returning, since this
  port has no frame-deferred variant. It validates the region, sizes `out` tightly
  packed, and delegates to `gpu::HardwareTexture::read`: Metal blits into a
  shared-storage staging texture (`readMetalTexture`, shared with the back-buffer
  screenshot, which reads a drawable that has no `Texture` in front of it), Vulkan
  copies into a host-visible buffer through `VulkanGraphicsDevice::runOneShotCommands`
  and restores the subresource's layout. The backbuffer screenshot stays separate
  on Vulkan: it must be recorded into the FRAME's command buffer before the present
  transition, which a one-shot buffer cannot do.
- `copyRenderTarget(source, colorDest, depthDest)` and `generateMipmaps(texture)`
  are the generic operations behind the scene grabs. A blit needs matching pixel
  formats; `PIXELFORMAT_BGRA8` exists for the drawable, and
  `backBufferColorFormat()` / `backBufferDepthFormat()` report what the back buffer
  actually is.

## Asset pipeline

- `ResourceLoader`: async single-thread worker, main-thread completion dispatch
- Handlers: `TextureResourceHandler` (stb_image + KTX2), `ContainerResourceHandler`
  (GLB+Draco), `FontResourceHandler`
- **Compressed textures**: `.ktx2` and GLB `KHR_texture_basisu` transcode via
  `Ktx2Transcoder` to **ASTC 4x4** on the loader thread. Generate test assets with
  the vcpkg `basisu` CLI (`-ktx2 -mipmap -uastc`).
- A glTF's images are decoded (or transcoded) in `GlbParser::prepareFromModel`, all at
  once across the machine's cores; tinygltf's image callback only keeps the encoded bytes.
- Parsers: GLB (tinygltf), OBJ (tinyobjloader), STL, Assimp. The GLB parser reads
  KHR_materials_transmission/_ior/_volume/_dispersion and generates tangents for
  triangle primitives with no TANGENT attribute.
- `instantiateRenderEntity()` returns the glTF root node for single-root scenes
  (upstream's rule), so `setLocalScale` / `setLocalRotation` REPLACE the model's
  own root transform.
- stb's vertical-flip flag is **thread-local**. Setting the global one does not
  affect a loader-thread decode.

### Shader creation and caches

- A pass creates its shaders in `RenderPass::prepareShaders()`, which the frame graph
  calls on every pass before the first one executes. On Metal a shader starts
  compiling when it is created (`MetalShader`, asynchronously) and the first use waits
  for it, so a frame's new shaders compile side by side. The forward pass resolves the
  variants of its culled draws there and again right before each layer's draw loop.
- A material remembers its resolved forward shader (`Material::ForwardShaderMemo`),
  keyed on its `uniformsVersion()`, the draw's flags, the library's frame switches and
  the chunk registry.
- The engine's programs are precompiled in the Slang bundle (a metallib, SPIR-V). Metal
  relies on the system's shader cache for the rest between runs; Vulkan keeps its own in
  `ShaderDiskCache`: run-time Slang and custom-GLSL SPIR-V and the device's pipeline cache
  (`GraphicsDeviceOptions::persistentShaderCache` / `shaderCacheDirectory`).

### Render pass types

**21 `RenderPass` subclasses.** The scene-level ones: `RenderPassForward` (main PBR
geometry, multi-light, multi-layer), `RenderPassShadowDirectional` (cascades, PCF +
EVSM_16F), `RenderPassVsmBlur`, `RenderPassShadowLocalClustered` / `NonClustered`,
`RenderPassUpdateClustered`, `RenderPassCookieRenderer`,
and the colour and depth grabs. The camera frame's sub-passes: prepass, SSAO, TAA,
downsample/upsample, bloom, the DOF trio (CoC, far downsample, bokeh blur under a
targetless `RenderPassDof` orchestrator), volumetric fog and its combine, compose,
and the after pass. Every quad-drawing pass derives from `RenderPassShaderQuad`.

### The env family over QuadRender

The environment bake — equirect-to-cube, reproject, convolve, atlas assembly — is
written once over `QuadRender` in `scene/graphics/envBake.h`, `envReproject.h` and
`envLighting.h`, with one Slang program per stage (`env-equirect-to-cube`, `env-reproject`,
`env-convolve`; the shared maps in the `vtenv` module); both
backends run the same importance-sampled convolution over a sample table passed as
an input texture. It runs inside `GraphicsDevice::beginOfflineWork` /
`endOfflineWork`, which is what lets ordinary render passes execute outside the
frame loop: Metal encodes it into its open command buffer and never waits, Vulkan records a one-shot buffer and waits so the frame-scoped uniform ring
and descriptor pools can be reused. A bake still has to run inside a frame, because
the per-draw uniform rings are handed out by `frameStart` (see AGENTS.md). The
effect-pass migration off the device vtable is complete; `copyRenderTarget` and
`generateMipmaps` stay virtual as generic device operations. `tools/generate-env-atlas`
produces deterministic PNGs and is the regression check for the whole family.

## Live gotchas

These bite while working ON the subsystem, so they
belong beside its reference rather than in the file every session loads in full.
The cross-cutting traps stay in `AGENTS.md`.

- **MSAA is a property of the RENDER TARGET on both backends, and the sample
  count reaches the pipeline through it.** `RenderTargetOptions::samples` is
  clamped to `GraphicsDevice::maxSamples()`, which each backend fills in from the
  hardware; a backend that never sets it silently renders every target
  single-sampled. A multisampled
  target owns a multisampled twin of each attachment, renders into that, and
  resolves into the plain texture the later passes sample — so `autoResolve` plus
  `ColorAttachmentOps::resolve` is what makes MSAA visible at all, and a target
  with neither antialiases into a surface nobody reads. Vulkan additionally keys
  its pipeline cache on the sample count (a raster count that disagrees with the
  attachments is invalid), resolves depth with SAMPLE_ZERO rather than an average
  (an averaged depth belongs to no surface), and skips the depth resolve for a
  pass that samples that same depth. An internally-owned depth buffer — the
  `RenderTargetOptions::depth` case with no depth texture — is created
  multisampled and never resolved, since nothing can sample it; that also costs
  it `TRANSFER_SRC`, so a multisampled target whose depth must be GRABBED needs a
  real depth texture. Verify a change here by edge statistics, not by eye: MSAA
  leaves whole-frame mean untouched and trades hard gradient steps for
  intermediate ones.
- **The camera frame's multisampled scene target has INTERNAL depth, and the
  prepass is the only sampleable depth under MSAA** (as upstream, which refuses
  in-scene depth with MSAA). `sanitizeOptions` marks every depth consumer — TAA,
  SSAO in either mode, DOF, volumetric fog — and under MSAA the prepass renders for
  any of them (`prepassRenders`), while the scene pass's multisampled depth is
  discarded, never stored or resolved. Single-sampled the scene pass writes the
  shared depth texture itself, and the prepass renders only for lighting-mode
  SSAO, which reads the depth before the scene pass. Under MSAA the scene target
  is NOT built over the shared depth texture: that would make
  `allocateAttachments` store AND resolve the 4x depth every frame for a texture
  the prepass had already written. Two companions go with it: a render target
  created without host data is PRIVATE on Metal (`Texture::renderTargetUse`, set
  by `RenderTarget`'s constructor; the GPU object is created eagerly, so marking
  RECREATES an untouched one) with a staging blit for any later CPU write
  (`MetalTexture::writeRegion`); and the scene target's twins are
  `transientMultisample` — `StorageModeMemoryless`, tile memory only — except when
  a later pass reloads the target (the transparent pass after a colour grab, the
  fog combine), a store on a memoryless twin being downgraded to a resolve with
  one warning. All three match upstream's structure and cut memory; none of them
  changes the frame time. A timing claim here needs both builds in one session,
  interleaved: day-to-day GPU clock state moves this scene's frame by as much as
  the effects being measured.
- **The shadow pass runs the caster's OPACITY FRONTEND before writing depth**,
  as upstream's `litShadowMain` does. Without it a masked material throws the
  shadow of its quad: a test against `baseColor.a` alone ignores the base-colour
  texture, and a depth-only pass with no fragment stage has neither the alpha test
  nor the shadow dither.
  The shadow's alpha must be the SAME product the forward pass tests, or its edge
  does not follow the visible one. Three consequences for anything touching this:
  `ProgramLibrary::getShadowShader` takes the CASTER'S MATERIAL and derives the
  alpha-test, base-colour-map and shadow-dither features from it, so a shadow
  variant is per material, not per pass; a shadow pass must BIND that material
  (uniforms and textures) for exactly the casters `shadowNeedsMaterial` names, and
  nothing for the rest, which is what keeps an ordinary caster's draw as cheap as
  it was; and on Vulkan the fragment stage is attached only for those casters,
  through `shadowOpacityFragment`, which declares NO colour output — that is the one
  fragment stage a depth-only pass may run, since a stage declaring a colour
  output with no colour attachment silently drops depth writes under MoltenVK. The
  frontend's discard exists only in a variant with `VT_FEATURE_ALPHA_TEST` or
  `_SHADOW_DITHER` (a stage that may discard loses early depth testing on Apple GPUs, so
  Metal, which always runs the frontend, would otherwise pay for it on every caster).
- **A light's shadow map is allocated ONCE, lazily, and only when null, so every
  property that changes what the map must BE — or whether it is needed at all — has
  to drop it** — today `setNumCascades`, `setShadowType`, `setShadowResolution` and
  `setCastShadows`, all four through `Light::destroyShadowMap`. Nothing announces the mismatch: the renderer finds a
  non-null map and renders into it, and the frame still comes out. A stale
  resolution renders a cascade into a fraction of a texture the shader then samples
  across, because the cascade viewport is recomputed per cull and the PCF texel size
  is uploaded per frame while the texture is not. **Each such setter must also
  early-out when the value is unchanged**: `LightComponent::syncToLight` replays
  EVERY property onto the backing `Light` once per frame, so an unconditional drop
  reallocates the map forever. Dropping the map also re-arms a light sitting at
  `SHADOWUPDATE_NONE`, or nothing would ever render into the replacement.
  `tests/shadowMapInvalidationTests.cpp` pins both halves for all four setters.
  The update mode is the one property the sync does NOT replay:
  `LightComponent::setShadowUpdateMode` pushes it once, because the renderer
  consumes `SHADOWUPDATE_THISFRAME` by writing `NONE` back and a per-frame replay
  would re-arm it into a realtime light. Default shadow
  resolution is upstream's 1024.
  **A directional light at `SHADOWUPDATE_NONE` is NOT re-fitted.** Its cascades
  are sliced from the camera frustum, and this port writes the sampling matrices
  (the palette and per-cascade fit) in `ShadowRendererDirectional::cull`, reading
  them at bind time — so re-fitting a light whose map will not re-render moves
  every sampling matrix with the view while the texture stays put: shadows are
  correct until the camera moves. Upstream
  re-fits every frame and gets away with it because it writes `shadowMatrix`
  INSIDE the shadow pass; `Renderer::cullShadowmaps` skips the fit instead, which
  holds the same invariant. The light still joins the per-camera list, because the
  forward pass reads its matrices through it.
  Note that `castShadows()` folds the MASK in, so a bare `Light` — one not driven by
  a `LightComponent`, which pushes its own mask every frame — reports false whatever
  `setCastShadows` said, because this port defaults `Light::_mask` to `MASK_NONE`
  where upstream uses `MASK_AFFECT_DYNAMIC`. Aligning that default is not free: it
  changes `depth-of-field`, whose only light is the environment atlas, so something
  reads `castShadows()` before the first sync and keeps the answer. It belongs with
  the defaults alignment rather than with a shadow-map change.
  The resolution is clamped to the device's `maxTextureSize`, or `maxCubeMapSize`
  for an omni — two limits that are NOT the same number on real hardware. It is
  clamped twice on purpose: in `Light::setShadowResolution`, as upstream does, and
  again in `ShadowMap::create`, because a `Light` may still be constructed with a
  null device and the allocation is the last point that can catch it.
  **`SHADOW_VSM_16F` falls back to `SHADOW_PCF3_32F` where the device cannot render
  half-float colour** (`GraphicsDevice::textureHalfFloatRenderable`) — VSM writes its
  EVSM moments into an RGBA16F ATTACHMENT, so without that capability the type
  cannot be rendered at all. `Light` therefore keeps the REQUEST and the resolved
  type apart: `syncToLight` replays the request every frame, and comparing it
  against the resolved type would differ forever and drop the shadow map each time.
  `tests/deviceCapabilityTests.cpp` pins the fallback and both clamps against a stub
  device, which is the only way to reach them — every GPU here answers yes to
  half-float and allocates far past any resolution an example asks for.
- **Lighting-mode SSAO needs the DEPTH PREPASS, and the pass order says which
  mode is running.** `SSAOTYPE_LIGHTING` folds the occlusion into the ambient term
  as the forward shaders run, so its texture has to be finished BEFORE the scene
  pass — which means something must have written scene depth before that, and the
  only thing that can is `RenderPassPrepass`. `SSAOTYPE_COMBINE` multiplies over
  the finished image in compose and is free to run after the scene, off the real
  scene depth. `collectPasses` places the SSAO pass on that rule, as upstream
  does; running it after the scene in lighting mode is not a small error, it makes
  every lit surface sample the PREVIOUS frame's occlusion.
- **The prepass renders with the SHADOW programs, not a shader pass of its own.**
  This port has no `SHADER_PREPASS`; depth-only drawing is `drawDepthOnly`
  (`scene/renderer/depthOnlyDraw.h`), shared by the two shadow passes and the
  prepass, and it picks the variant for the caster's deformation path and binds
  the material only when the caster's OPACITY decides the depth it writes. What
  each caller still owns is the camera, the pass state and the FILTER: a shadow
  pass draws `castShadow` casters, the prepass draws materials with depthWrite on
  from the camera's sublayers before the Depth layer (the frame's cull sets, with the TAA
  jitter), and the two disagree in both directions — a mesh with shadows off
  still occludes in screen space, and a dithered-shadow caster must not write
  prepass depth.
- **The prepass and the scene pass write the SAME depth texture through two render
  targets — single-sampled, where the prepass runs at all (lighting-mode SSAO).**
  The prepass target is depth-only and single-sampled
  on purpose: under MSAA the scene target's depth is internal and discarded, and
  the texture every later pass samples is the prepass's own output, needing no
  resolve. The cost is that a resize of the shared texture through
  one target leaves the other's attachments stale, and `RenderTarget::resize`
  cannot fix it (the second target's `width()` already reads the new size off the
  shared texture and the resize early-outs), so `RenderPassCameraFrame::frameUpdate`
  rebuilds the prepass target and re-points the pass by hand.
- **Cameras render in PRIORITY order, smallest first** (`CameraComponent::priority`,
  default 0, stable sort). Cameras of equal priority keep construction order, so
  a dynamic reflection probe that must render before the camera sampling it is
  either constructed first or ordered with a priority. The composition's camera fingerprint includes the priority,
  so changing one rebuilds the render actions.
- **PCSS tightens every cascade against the UNION of the cascades' caster boxes.**
  PCSS scales its penumbra by the cascade's caster depth RANGE, so a range that moves
  makes the softness move: a mesh crossing a cascade's cull boundary changes that
  cascade's range and the shadows under it change width. The union makes the range
  depend on the whole visible caster set instead. Only PCSS — the other shadow types
  never read the range and are better off with per-cascade tightening for depth
  precision. This is why `ShadowRendererDirectional::cull` fits in TWO passes: the
  union is not known until every cascade has been swept.

  No shipped example shows it. `procedural-sky` is the only PCSS directional scene and
  its geometry fills every cascade, so the boxes coincide anyway; `shadow-cascades`
  DOES show cascade 0's box differing from the rest, but it is not PCSS. Verify a
  change here by logging the per-cascade box, not by looking.
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
- **A dynamic `ReflectionProbe`'s face cameras must render BEFORE the main
  camera.** Cameras render in priority order and equal priorities keep construction
  order, so construct the probe first or give the main camera a higher priority. The reflective object also has to sit on a layer excluded from the
  probe's capture layers, or it self-captures.
- **A wide line is one instance per SEGMENT, expanded in the vertex shader.** The
  template geometry (quad body, two discs for round caps and joins, two bevel
  triangles) comes from the VERTEX ID rather than a vertex buffer, so every
  instance draws the same vertex count and a piece the current style does not want
  collapses to zero size instead of being skipped. Widths are screen pixels by
  default, which is why the expansion happens after the projection rather than in
  world space. The segment buffer never shrinks: a smaller set is written into its
  FRONT with a full-size payload and the draw's instance count says how much is
  live, because `VertexBuffer::setData` refuses any payload that is not the
  buffer's exact size — uploading only the live records would freeze every line
  from the first frame with fewer segments. `wideLineSegmentBuffer.h` owns
  this and `tests/wideLineSegmentBufferTests.cpp` holds it.
- **GPU instance culling requires the 80-byte stride.** Its kernel compacts fixed
  80-byte records. The instanced shader variant follows THE DRAW, not the
  material: the renderer derives it from the mesh instance's buffer format.
- **Parallax `heightMapBase` shifts the ray's ENTRY UV, not just the depths.**
  The base is the height-map value that sits at the level of the geometry, so
  anything above it lifts off the polygon and the view ray has to enter higher up.
  Offsetting only the depths moves the ray and the field by the same amount and
  changes nothing at all — the images come back bit-identical, so it looks like it
  works. The entry UV must move by the
  lateral distance the ray covers over that height. Base 1 is pure depth below the
  surface; the default 0.5 pivots around mid-grey, as upstream.
- **`heightMapFactor` is in TENTHS of a uv tile**, upstream's unit: a factor of 1
  asks for a relief 0.1 uv deep. Used raw, upstream's own tuned value of 0.4 smears
  brickwork into spikes.
- **Parallax self-shadowing costs a second march and only the DIRECTIONAL light
  pays it.** It runs inside the light loop, which is behind fragment-varying
  control flow, so its height taps use an explicit LOD; the view march sits in
  uniform flow at the top of the shader and keeps implicit LOD so the height map
  keeps its mips. `setHeightMapShadow` defaults to 0, so nothing pays unless asked.
- **Opacity dither is an opaque-pass technique.** Keep the material
  non-transparent; alpha comes from `setOpacity` or texture alpha. `setAlphaDither`
  decouples dither density from opacity and rides in `dispersionParams.y`, where
  NEGATIVE means unset.
- **`detailNormalScale` blends the DETAIL map toward flat too, and the two normals
  combine with a REORIENTED blend, not by adding xy.** Adding xy treats the detail
  slope as if the base were flat, so the combined slope is wrong wherever the base
  is not. Upstream's `blendNormals` rotates the detail into the base normal's frame:
  `n1 = base + (0,0,1)`, `n2 = detail * (-1,-1,1)`, `n1 * dot(n1,n2) / n1.z - n2`,
  left unnormalized because the TBN product is normalized after. Both backends do
  this.
- **`normalScale` blends the sampled normal TOWARD FLAT.** It does not scale xy.
  Upstream's `material_bumpiness` is a `mix(vec3(0,0,1), normalMap, s)` and both
  backends do that. Scaling xy leaves z alone, so it steepens the normal's
  slope exactly where the mix flattens it; the two agree only at 0 and 1, which is
  why the default of 1 hid this. The earlier note here, that
  Vulkan held upstream's form and aligning would move every Metal scene, was wrong
  on both counts. There is also no Gram-Schmidt re-orthonormalization of the
  interpolated tangent: upstream normalizes the tangent and binormal and
  does nothing else. It measured as a no-op, so if a mesh ever shows tangent skew,
  add it back to BOTH backends rather than one.
- **A GPU lightmapper phase starts every scene light from the state it had when the
  bake began.** `GpuLightmapper::enableSceneLights` ANDs that recorded state with what
  the phase wants; switching a light on or off by hand between a `bake()` and the
  `update()` that completes it is overwritten by the next phase.

### Atmosphere (Nishita)
`Sky::setDepthWrite` (upstream's `Sky.depthWrite`, default false) makes a dome or box sky
write its REAL depth, for effects that need a finite depth where the sky is — DOF, fog,
SSAO: the material's depth write compiles `VT_FEATURE_SKY_DEPTH`, which keeps
`forwardSkyVertex` from pinning the sky to the far plane (every other sky stays there).
The infinite sky never writes depth (`SkyMesh::setDepthWrite`). DEVIATION: upstream
keeps that sky on the far plane too and writes its linear depth into a separate
scene-depth output, so geometry beyond the sky mesh is hidden by it here and not there.
Before the feature existed the depth write was a no-op: `shadow-catcher`'s DOF blurred
the dome's floor up to the statue's feet. It is applied to the current mesh and to every
rebuilt one.

Two traps, both of which silently produce no sky at all:
- **`Scene::setAtmosphereEnabled` must rebuild the sky mesh.** The atmosphere
  branch of `Sky::updateSkyMesh` requires the flag to be set ALREADY, and every
  caller writes `setSkyType` then `setAtmosphereEnabled`. The setter calls
  `resetSkyMesh()`.
- **`planetCenterAndRadius.xyz` is CAMERA-LOCAL.** A viewer on the surface needs
  the centre one radius BELOW: `{0, -6371000, 0}`. At the origin the viewer sits at
  the planet's core and every ray starts underground.

With the ray stuck underground, changing sun direction or intensity looks like it
does nothing, which is a misleading symptom.
