# Architecture Reference

Per-subsystem reference for VisuTwin Canvas: how each feature works, how to turn
it on, and where it deviates from upstream. Split out of `CLAUDE.md` on
2026-09-04, verbatim, so that file could stay small enough to load at the start of
every session.

Read `CLAUDE.md` first. It holds the rules and the live gotchas, and where the two
files overlap, `CLAUDE.md` is authoritative. Completed-work narrative lives in
`ENGINEERING-LOG.md`.

## Contents

Rendering and materials: instancing, shadows, tangent frames, opacity dither,
vertex-color routing, spec-gloss and friends, scalar material maps, the material
system. Lighting: ambient SH probes, reflection probes, LTC area lights, light
cookies, lightmaps. Geometry and effects: skinning and morphs, the anim state
graph, refraction, screen-space reflections, Gaussian splatting, particles,
app-facing compute, extras, the GPU profiler.

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
  cull kernel compacts fixed 80-byte records — and now refuses the 64-byte one
  instead of misreading it.
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
in). DEVIATION: Vulkan's `forward_instanced.vert` reads the matrix only and
never consumed a per-instance color, so 80-byte buffers render with the material
color there.

### Directional shadows
- **Shadow bias convention** (fixed 2026-08-14): `LightComponent::setShadowBias` takes upstream's **0..1 authoring value** (default 0.05) and remaps it to the internal `Light::shadowBias` as `-0.01 * clamp(v,0,1)` — negative on purpose, because the shadow passes apply `shadowBias * -1000` as the hardware polygon offset and that product must be POSITIVE to push casters AWAY from the light. Passing the raw component value straight through inverted it, so a larger bias produced MORE shadow (a self-casting ground went fully black at 0.05). Hardware bias is skipped for PCSS (biases in-shader) and non-clustered omni (stores distance, not depth), as upstream does. The directional PCF shader uses a fixed 0.0001 receiver bias, NOT the light's; only local/clustered lights consume the light bias in-shader (negated + upstream's ×20 spot scale, since our shader subtracts it from the receiver depth). Ground planes must stay shadow CASTERS — the directional shadow camera fits its depth range to casters only, so a receiver-only ground falls outside it and catches no shadow.
- **PCF3_32F** (default): hardware-compared depth2d, 4-tap bilinear PCF reconstructing a 3×3 kernel.
- **PCSS_32F** (`SHADOW_PCSS_32F`): contact-hardening soft shadows (upstream `shadowSoft.js` PCSSDirectional). Also supported on **spot/omni local lights** (upstream `shadowPCSS.js`, 2026-07-13): a runtime uniform branch (no extra variant) driven by `LightingData::localShadowPcss0/1` = {searchArea UV (0=off), near, far}; spot = Vogel-disk blocker+filter with per-tap depth linearization, omni = Vogel-sphere direction perturbation on the depth cube; `searchArea = penumbraSize/shadowResolution (*fovRatio for spot)` — local-light `penumbraSize` is in shadow-map PIXELS (~10-40), NOT the directional world-space scale. Example: `pcss-local-example.cpp`. FIXED two pre-existing spot local shadow bugs found here: the spot shadow camera was missing upstream's `rotateLocal(-90,0,0)` (camera looks -Z, light emits -Y) and `MetalUniformBinder` uploaded `localShadowMatrix` transposed (`Matrix4::getElement` takes **(col,row)** — was called (row,col)); spot 2D shadow maps never worked before. Vogel-disk blocker search + filter (in-shader sample generation, `fractSinRand` seed), world-space penumbra: `penumbra = shape * penumbraSize * depthRange` with `shape = 1-(1-t)^penumbraFalloff`. Per-cascade ortho radii + caster depth ranges flow via `LightingUniforms::pcssCascadeRadii/pcssCascadeDepthRanges`; `pcssParams` = {filterSamples 16, blockerSamples 16, penumbraSize, penumbraFalloff}. DEVIATION: reuses the standard PCF hardware depth map sampled RAW (non-comparison `shadowRawSampler`) instead of upstream's dedicated R32F color map — the `pcf=true` flag in its `shadowTypeInfo` entry selects the depth attachment. Configure: `setShadowType(SHADOW_PCSS_32F)` + `setPenumbraSize(0.02-0.05 — upstream example scale; ~tan of light angular size)` + `setPenumbraFalloff(>=1)`. `VT_FEATURE_PCSS_SHADOWS` set per frame like VSM. GOTCHA: a huge ground plane left as shadow CASTER inflates the fitted caster depth range → whole-plane acne (PCF) and blown-up penumbras (PCSS) — set `render->setCastShadows(false)` on large receiver-only ground.
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
normal and the pixel. Metal has always guarded it (`length_squared(T) >= 1e-6`,
falling back to the geometric normal); the Vulkan chunk did not until 2026-09-03
and now does the same.

DIVERGENCE, deliberately left alone: the two backends interpret `normalScale`
differently — Metal blends the sampled normal toward flat
(`normalize(mix(float3(0,0,1), sample, normalScale))`), Vulkan scales xy
(`tn.xy *= normalScale`, which is upstream's form). Aligning them changes every
normal-mapped Metal scene, so it wants its own commit and its own verification.

### Opacity Dither
`VT_FEATURE_OPACITY_DITHER` (upstream `opacity-dither.js`, BAYER8 variant): `StandardMaterial::setOpacityDither(true)` renders partial opacity in the OPAQUE pass by discarding fragments against a screen-space 8×8 Bayer threshold (pow-2.2-linearized) — no sorting artifacts, correct depth writes. Keep the material non-transparent; alpha comes from `setOpacity`/texture alpha. DEVIATIONS: no blue-noise/IGN variants, no per-frame jitter (static pattern). Bayer helpers live in the `common-dither` chunk (`ditherThreshold` / `ditherDiscards`, shared by the forward and shadow chunks); the forward discard block sits after the alpha-test block.

**Decoupled strength — `alphaDither`** (upstream `StandardMaterial.alphaDither`, 2026-08-21): opacity normally drives BOTH the alpha blend and the dither density. `setAlphaDither(v)` splits them so opacity drives only the blend and `v` only the dither; `clearAlphaDither()` restores the coupled default. It rides in `MaterialUniforms::dispersionParams.y`, where NEGATIVE means unset. That sign also gates the legacy `alpha = 1.0` write: coupled use is an opaque-pass technique so alpha is forced, while decoupled use is upstream's blend-AND-dither case where alpha must survive to drive the blend.

**Shadow dither — `opacityShadowDither`** (flags bits 29-31, independent of the forward mode in bits 25-27): a partially-opaque caster discards the same Bayer pattern in the shadow pass and so throws a THINNED shadow. Two things had to change for it to fire, and both are easy to miss: the shadow pass otherwise **bypasses materials entirely** (the device hands the shader a default-constructed `MaterialUniforms`), so `renderPassShadowDirectional` now binds the caster's real material — but ONLY for casters that opted in, leaving every other scene's shadow pass untouched; and `shadowCasterFiltering` excluded transparent materials from casting at all, so the very casters this feature targets never reached the pass. DEVIATION: implemented in the Metal shadow fragment chunk, which serves both PCF and VSM. On Vulkan the PCF shadow pass is depth-only with no fragment stage, so only the VSM path can dither.

Example: `pcss-dither-example.cpp` (port of upstream `graphics/dithered-transparency`).

### Ambient SH Light Probes
`VT_FEATURE_LIGHT_PROBES`: 9-coefficient spherical-harmonics ambient replacing the flat ambient (upstream AMBIENTSH basis: `sh[0] + sh[1]x + sh[2]y + sh[3]z + sh[4]xz + sh[5]zy + sh[6]yx + sh[7](3z²-1) + sh[8](x²-y²)`). Enable via `Scene::setAmbientSH(std::array<Vector3,9>)` / `clearAmbientSH()`; the renderer sets `ProgramLibrary::setLightProbesEnabled` per frame and uploads the coefficients in `LightingUniforms::ambientSH[9]`. Coefficients are premultiplied (Ramamoorthi irradiance convolution + 1/π baked in, so a uniform environment of radiance A gives flat ambient A); `sh::projectEquirect` (`scene/graphics/sphericalHarmonics.h`) projects float or 8-bit-sRGB equirect radiance maps, `sh::evaluate` is the CPU mirror. When probes are active they replace both the flat ambient AND the env-atlas Lambert diffuse (specular IBL stays). NOTE: `ProgramLibrary::setEnvAtlasEnabled` (set per frame from `scene->envAtlas()`) gates VT_FEATURE_ENV_ATLAS — without it, unbound-atlas sampling returned nonzero `get_width()` on Apple GPUs and silently overwrote flat ambient with black. Example: `light-probes-example.cpp` (gradient sky projected to SH9, auto-cycles flat vs SH).

### Anim State Graph (modern `anim` component)
Upstream `framework/anim/controller` + `components/anim` port at `engine/src/framework/anim/controller/`, `anim/state-graph/`, `components/anim/`:
- **AnimController**: state machine with transitions (conditions on typed parameters, exit times, transition offsets, interruption sources, priorities), crossfades via clip blend weights, previous-state stack for interrupted transitions.
- **Blend trees**: 1D, 2D cartesian, 2D directional, direct — all four upstream variants in one file (`animBlendTree.h/.cpp`), built from typed `AnimBlendTreeDesc` (DEVIATION: no JSON graph format; `AnimStateGraph` is a builder-style C++ data model).
- **AnimComponent** (`anim` system id, registered like other systems via `registerComponentSystem<AnimComponentSystem>()`): layers (each own AnimController+AnimEvaluator), parameters (float/int/bool/trigger — single float storage), `assignAnimation("State.Leaf", track)` paths. DEVIATION: no cross-layer target blending/masks (layers apply in order) and no animation events yet.
- **AnimEvaluator** now composites N clips sequentially (first contributor sets, later clips lerp by their blendWeight — mirrors upstream anim-evaluator.js); legacy AnimationComponent crossfade unchanged.
- Example: `examples/src/anim-stategraph-example.cpp` (fox: Survey ⇄ 1D Walk/Run blend tree driven by a "speed" parameter; auto-demo cycles it).

### GPU Skinning + Morph Targets
- **Skinning** (`VT_FEATURE_SKINNING`): 4-bone weighted blend. GLB parser reads JOINTS_0/WEIGHTS_0 into an 88-byte skinned vertex layout (PackedVertex + weights float4 @56 + joint indices float4 @72, attributes 11/12). `SkinInstance` builds a node-relative float4x4 bone palette per frame (deduped via `SkinInstance::beginFrame()` counter) uploaded through the slot-6 palette ring (shared with dynamic batching — mutually exclusive). Skins resolve bones by glTF node index at `instantiateRenderEntity()` (DEVIATION: upstream resolves by name). **Skinned culling**: the GLB parser computes per-bone bind-space AABBs (weight > 1e-4 influences, reading POSITION+JOINTS/WEIGHTS in parseSkins) stored on `Skin`; `MeshInstance::aabb()` unions each used bone's AABB transformed by `bone.worldTransform * inverseBind`, so skinned instances are frustum-culled (`cull` stays false only for skins without bone AABBs, e.g. non-GLB paths). Verified: fox forward-pass GPU 0.165→0.071 ms when camera turns away.
- **Morphs** (`VT_FEATURE_MORPHS`): DEVIATION from upstream's render-to-texture accumulation — all target deltas live in one static buffer (per target/vertex: float4 posDelta + float4 nrmDelta, vertex slot 9); the vertex shader sums the top-8 active targets driven by an 80-byte `MorphParams` uniform (slot 10, `MorphInstance::gpuParams()`). Applied in bind space before skinning. **Morph weight animation**: glTF "weights" channels parse into `AnimCurve{propertyPath="weights"}` (N components = target count), `AnimTransform.weights` blends through AnimEvaluator like transforms, and `AnimBinder::resolveMorphInstances` (DefaultAnimBinder: mesh-node entity → RenderComponent morph instances) applies them — works through both legacy AnimationComponent and the anim state graph. Test asset: `assets/models/morph_wave.glb` (generated). NOTE: `morph-anim-example.cpp` covered this (plus skinned bone-AABB culling) and was deleted 2026-08-17 — no example exercises morph weight animation now, and `morph_wave.glb` has no remaining consumer.
- Both compose with shadow passes (directional + local non-clustered fetch skinned/morphed shadow shader variants lazily). Draco-compressed primitives skip skin/morph attributes.

`VT_FEATURE_VSM_SHADOWS` is set per-frame by the renderer based on the active directional light's `shadowType`. When set:
- `shadow-fragment.metal` writes EVSM moments to RGBA16F instead of relying on hardware depth.
- `forward-fragment-head.metal` binds `shadowTexture` as `texture2d<float>` instead of `depth2d<float>`.
- `forward-fragment-lighting.metal` calls `getShadowVSM16` (Chebyshev) instead of `getShadowPCF3x3`.

### Reflection Probes (box-projected cubemap)
`VT_FEATURE_REFLECTION_PROBE` (upstream `cubeMapProject.js` BOX + `reflectionEnv.js`): a local, parallax-corrected cubemap reflection replacing the global env-atlas specular IBL. Scene-level (like `setSkybox`/`setEnvAtlas`): `Scene::setReflectionProbe(cubemap, position, boxMin, boxMax, boxProjection=true, intensity=1)` / `clearReflectionProbe()`. The prefiltered cubemap binds at **fragment slot 24** (`texturecube<float> reflectionProbeCube`; `kMaxTextureSlots` 24→25); `LightingData::reflectionProbeBoxMin/Max/Params` (params = {boxProjection flag, intensity, maxLod}) carry the box + settings. In `forward-fragment-ambient.metal` the reflection dir `reflect(-V,N)` is box-projected (intersect the box, re-aim from box center — parallax) when `boxProjection`, X-flipped for the engine cube convention, sampled at a roughness→mip LOD, sRGB-decoded, Fresnel-weighted, and OVERWRITES `indirectSpecular`. Runtime feature: `ProgramLibrary::setReflectionProbeEnabled` per frame from `scene->reflectionProbe()`; `MetalUniformBinder::setReflectionProbeUniforms` fills the uniforms + stores the cube. DEVIATIONS: single scene-level probe (no per-mesh assignment/blending); roughness uses hardware trilinear cube mips, NOT upstream's GGX-prefiltered-per-level cube; no GGX cube→cube prefilter path yet. Example: `reflection-probe-example.cpp` (chrome sphere + polished floor in a colored-room cube; auto-toggles box projection ON — floor reflects the walls parallax-correctly — vs OFF — floor reflection collapses to a near-uniform direction-only color).

**Dynamic scene-capture bake** (`framework/extras/reflectionProbe.h/.cpp`, 2026-07-14): `ReflectionProbe` renders the live scene into the probe cubemap at runtime instead of using a supplied/authored cube. It owns a mipmapped RGBA8 color cubemap + 6 face `RenderTarget`s + 6 `CameraComponent`s pointed along ±X/±Y/±Z (reusing `LightCamera::pointLightRotations`, 90° FOV, aspect 1). The six face cameras render as ordinary cameras in the normal frame graph (each `RenderTarget` targets one cube face via `RenderTargetOptions.face`), so **construct the probe BEFORE the main camera** (layer composition renders cameras in construction order → faces captured before the main camera samples the probe). Per frame `update()` (called AFTER `engine->render()`) runs `GraphicsDevice::generateCubemapMips` (blit `generateMipmaps` on its own command buffer) to rebuild the roughness mips from the freshly-rendered level-0 faces, and installs the cube via `setReflectionProbe` on the first call. Modes: `setDynamic(true)` re-captures every frame (reflections track the scene); `false` = one-shot then disable the face cameras. Two enabling engine changes: (1) `MetalGraphicsDevice::startRenderPass` now sets the **color** attachment slice from `activeTarget->face()` for cube textures (previously only the depth attachment did, for omni shadows); (2) `GraphicsDevice::generateCubemapMips`. Captured faces hold the normal tonemapped/gamma-encoded forward output, which the probe shader sRGB-decodes — matching the static path. DEVIATIONS: hardware-mip roughness (no GGX cube prefilter); the reflective object must sit on a layer excluded from the probe's capture layers or it self-captures (probe camera is at the probe center); probe faces miss directional-shadow cascades (fit only for the presentation camera). Example: `reflection-probe-dynamic-example.cpp` (chrome sphere on a probe-excluded layer reflects a ring of orbiting emissive boxes captured live — no env atlas/skybox, so the colored reflections come purely from the runtime capture).

### LTC Area Lights
`VT_FEATURE_AREA_LIGHTS` area lights (`LIGHTTYPE_AREA_RECT`) use **LTC** (linearly transformed cosines, Heitz et al.) — port of upstream `ltc.js` with all three shapes: **rect, disk, sphere** (`LightComponent::setAreaShape(AreaLightShape::LIGHTSHAPE_RECT/DISK/SPHERE)`, 2026-07-13). Shape rides in `GpuLight::typeCastShadows.y` (area lights never cast shadows). Disk = `ltcEvaluateDisk` (upstream LTC_EvaluateDisk cubic-solver ellipse integral, LUT2.w horizon-clipped-sphere scale, NaN-guarded) inscribed in the width/height quad; sphere = billboarded quad toward the reflection vector + disk specular, wrap-Lambert diffuse with radius falloff (radius = max half-extent). Helpers in `common.metal` (`ltcUv`, `ltcEvaluateRect`, edge/clipped-sphere form factors); the light-loop branch in `forward-fragment-lighting.metal` accumulates diffuse (identity transform, ×16 to match the constant in `getFalloffInvSquared`, `1-specFres` energy conservation), specular (inverse matrix from LUT 1, Fresnel/geometry magnitudes from LUT 2), and clearcoat, then `continue`s past the shared punctual GGX. Distance attenuation is range-window only (`getFalloffWindow`) — physical falloff comes from the form factor. The two 64×64 RGBA16F LUTs are **embedded** in the engine (`scene/graphics/areaLightLuts.h` + generated `areaLightLutsData.inc`; DEVIATION: upstream ships them as an app-loaded JSON) — the renderer creates them lazily on the first area light and binds fragment slots 20/21 through `GraphicsDevice::setAreaLightLuts`. Area lights do not cast/receive local shadows. GOTCHA: with `StandardMaterial`, set surface properties via `setDiffuse/setMetalness/setGloss(+setGlossInvert)` — the base-Material `setBaseColorFactor/setMetallicFactor/setRoughnessFactor` values are overwritten by `StandardMaterial::updateUniforms` when no base-color texture is present. Example: `area-light-example.cpp` (warm panel over five floor strips, roughness 0.05→0.75).

### Dynamic Grab-Pass Refraction
`VT_FEATURE_DYNAMIC_REFRACTION` (upstream `refractionDynamic.js`): transmission samples a mid-frame **scene color grab** instead of the env atlas. `StandardMaterial::setUseDynamicRefraction(true)` (+ transmission/thickness/IOR + `setTransparent(true)` so the mesh draws after the grab) and `CameraComponent::requestSceneColorMap(true)` (the depth-layer `RenderPassColorGrab` — previously a stub — now blits the scene color into a persistent full-mip texture via `GraphicsDevice::grabSceneColor`, works for both offscreen RTs and the drawable since `framebufferOnly=false`). Bound at fragment slot 22 (`kMaxTextureSlots` now 23); rough/high-IOR surfaces read blurrier mips (upstream `iorToRoughness`); the shader projects the refracted exit point with `LightingData::viewProjection` (new field, also in `LightingUniforms`). **KHR_materials_volume + dispersion (2026-07-13)**: `setAttenuationColor`/`setAttenuationDistance` enable Beer-law transmittance `exp(-(-log(attColor)/attDist)*thickness)` (distance 0 keeps the legacy baseColor^thickness tint; applies to BOTH dynamic and env-atlas paths); `setDispersion` (dynamic path only) samples R/G/B at spread etas (`halfSpread = (ior-1)*0.025*dispersion`). The GLB parser now reads KHR_materials_transmission/_ior/_volume/_dispersion (`applyVolumeExtensions`). **Linear-grab**: the pow(2.2) grab decode is gated on flagsAndPad bit 5 — under the HDR CameraFrame path the mid-frame grab is linear and is NOT decoded. DEVIATIONS: no model-scale extraction (thickness is world units). Example: `refraction-example.cpp` (glass sphere over colorful columns, auto-cycles dynamic vs env-atlas).

### Screen-Space Reflections
`VT_FEATURE_SSR` (upstream `reflectionSSR.js` in spirit — per-fragment world-space march, not upstream's post-process view-space HiZ; 2026-07-14): a glossy surface reflects the on-screen opaque scene by ray-marching the reflection vector against a **scene depth grab** and sampling the **scene color grab** at the hit. `StandardMaterial::setUseScreenSpaceReflection(true)` + `setTransparent(true)` (so the surface draws AFTER the mid-frame grabs, in the depth layer) and BOTH `CameraComponent::requestSceneColorMap(true)` + `requestSceneDepthMap(true)`. Reuses the refraction color grab (slot 22); adds a **depth copy** — `GraphicsDevice::grabSceneDepth(RenderTarget*)` mirrors `grabSceneColor`, blitting the pass depth into a persistent `Depth32Float` private texture bound at **fragment slot 25** (`depth2d<float> ssrSceneDepthTexture`; `kMaxTextureSlots` 25→26). A depth COPY is required because the live depth buffer is still attached during the transparent draw (can't sample the target you're writing). The march (in `forward-fragment-ambient.metal`, after the reflection-probe block) projects each world-space step through `LightingData::viewProjection` to screen UV, compares the step's clip-space `w` (view distance) against the linearized scene depth (`sceneZ = near*far/(far - rawDepth*(far-near))`, near/far via new `LightingData::cameraNearFar` fed by `GraphicsDevice::setCameraClipPlanes` per frame from the camera). On a hit within `ssrThickness` it decodes the grab (pow(2.2) unless the HDR camera-frame path, flagsAndPad bit 5), applies an edge fade + roughness fade (`saturate(gloss*1.2-0.2)` — sharp march, no roughness cone) + `getFresnel`, and `mix`es OVER the probe/env-atlas `indirectSpecular`. Variant key bit 47; runtime gate `options.ssr = stdMat->useScreenSpaceReflection()`. DEVIATIONS: forward-pass per-fragment march (no HiZ acceleration, fixed 48 steps / 60-unit range), sharp reflections only (no roughness blur/mip cone), no temporal accumulation or hit fade-by-thickness, off-screen rays fall back to env/probe (no screen-edge stretch). Example: `ssr-example.cpp` (three colored opaque objects on a dark mirror floor; auto-toggles SSR every 3 s — ON, each floor region reflects the object above it; OFF, the floor collapses to flat ambient).

### Extras: OutlineRenderer + ViewCube
`engine/src/framework/extras/` (ports of upstream `extras/`):
- **OutlineRenderer** (`outline-renderer.js`): colored selection outlines. A dedicated "Outline" layer (id 100, excluded from the main camera's default layer list) + offscreen camera render flat **unlit clone** mesh instances (sharing mesh+node with the source — DEVIATION: upstream re-renders originals through a shader-pass override) into an RGBA8 target; H/V extend quad passes (5-tap dilate + edge alpha, offsets/srcMultiplier baked into two shader variants since quad passes carry no uniforms) then an alpha-blend quad composite over the back buffer. The three post passes register via **`Renderer::addAppendPass`** — a new engine mechanism appending app passes to the END of the frame graph. GOTCHA that motivated it: rendering to the back buffer AFTER `Engine::render()` crashes (frameEnd presents the drawable; a stale `_frameDrawable` reuse is a pointer-auth SIGSEGV — the older edge-detect example has this latent bug). Per frame: `outline->frameUpdate(cameraEntity)` before render.
- **ViewCube** (`view-cube.js`): world-axis orientation gizmo. DEVIATION: upstream is DOM/SVG; this port renders unlit sphere handles + axis rods on the IMMEDIATE layer with depth test off, anchored each frame to the camera's top-right corner (`update(cameraEntity)`). `onClick(x, y, w, h, cameraEntity)` unprojects a ray (standard-Z, near plane at clip z=0), ray-sphere picks the six handles, and fires `ViewCube::EVENT_CAMERAALIGN` with the world axis (EventHandler payload).
- Example: `outline-viewcube-example.cpp` (auto-cycling outline over three objects + view cube; includes an onClick scan self-test).

### Vertex Color Routing + Unlit Emissive
`StandardMaterial::setDiffuseVertexColor` / `setEmissiveVertexColor` (upstream's
two flags of the same name, 2026-08-17). A mesh's vertex colors modulate the
DIFFUSE lane by default — which is how every vertex-colored material here behaved
before — and material flag bit 28 turns that off while bit 23 routes them to
EMISSIVE instead. `emissiveVertexColor` also implies the vertex-color variant
(`options.vertexColors`), which is otherwise an explicit `shaderVariantKey` bit 21
opt-in, since the material cannot see whether the mesh carries a color stream.

`VT_FEATURE_UNLIT` now outputs `baseColor + emissive` rather than base color
alone. Upstream reaches this path through `useLighting = false`, which drops the
lights but keeps the emissive lane — a material with black diffuse and a bright
emissive map (upstream's decals) rendered as pure black before. The emissive term
is recomputed inside the unlit block because that early return never reaches
`forward-fragment-emissive`. Example: `mesh-decals-example.cpp`.

### Light Cookies
`VT_FEATURE_COOKIE_2D` / `VT_FEATURE_COOKIE_CUBE` (upstream `cookie.js` +
`lightFunctionLight.js`, 2026-08-17): a texture the light projects onto the
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
would blow the 16-per-stage limit MoltenVK inherits). `GpuLightUniform` grew a
`cookieFlags` uint4 (80→96 bytes) and `LightingUniforms` a 4-matrix + 4-vec4
cookie block; `VulkanLightingUBO` mirrors both (2000→2448 bytes — the size is
asserted in `vulkanRenderPipeline.cpp` AND in the shader-bundle validator).

**GOTCHA that cost the most time:** cookie samples sit inside the per-light loop
behind fragment-varying `continue`s, so screen-space derivatives there are
undefined — and an undefined mip LOD reads a fully averaged mip, turning a
heart-shaped cookie into a flat wash of its own average. Both backends sample with
an explicit LOD 0 (`level(0)` / `textureLod`). DEVIATION: upstream mipmaps cookies.

Other DEVIATIONS: no `cookieTransform`/`cookieOffset` (upstream's `getCookie2DXform`
pair); the clustered cookie atlas (`RenderPassCookieRenderer`, still filter-only)
is NOT ported — cookies work on the non-clustered forward path, which is what
upstream's own example exercises. Example: `lights-example.cpp`.

**Spot cone angles are HALF-angles** (upstream: `cos(outerConeAngle * DEG_TO_RAD)`,
and its shadow/cookie cameras use `fov = outerConeAngle * 2`). The renderer used to
halve them when building `outerConeCos`, so every spot's lit cone was half as wide
as the shadow and cookie frustum fitted to the same light — a beam covering only the
middle of its own cookie. Fixed 2026-08-17 in `renderer.cpp` and `worldClusters.cpp`;
it widens every existing spot-light example to upstream's geometry.

### Lightmaps
`StandardMaterial::setLightMap(texture)` → `VT_FEATURE_LIGHTMAP` variant: the lit shader samples the lightmap at **UV1** (texture slot 19; procedural primitives mirror UV0 into UV1) and the sRGB-decoded sample **replaces** indirect diffuse. Upstream's `lightmapAdd.js` adds it, but `lit-shader.js` gates the ambient behind `addAmbient = !lightMapEnabled` — adding both double-counts what the bake already contains and visibly washes the surface out (fixed 2026-08-16, both backends). Specular IBL is unaffected. When adding material texture slots: bump `MetalTextureBinder::kMaxTextureSlots` AND the `materialSlots` clear list in `bindMaterialTextures`.

**GPU lightmapper** (`framework/lightmapper/gpuLightmapper.h/.cpp`, 2026-08-16): upstream's own mechanism — each target mesh is rendered **in UV space** (`VT_FEATURE_LIGHTMAP_BAKE`: the vertex stage writes clip position from UV1, the fragment stage outputs the diffuse LIGHT with no albedo), so occlusion comes from the existing shadow maps instead of rays. ~40 ms for the house scene versus ~12 s for the CPU baker. The bake rides the normal frame graph like `ReflectionProbe`: one camera per target with `Camera::setLightmapBakePass(true)`, its own render target, and a private layer holding just that mesh; `bake()` then `update()` after `Engine::render()`. THREE things it must do that are easy to miss: the bake pass forces `CULLFACE_NONE` (UV winding follows the unwrap, so half the charts would be culled), every scene light gets the bake layer ids appended for the duration (lights are filtered per layer, else only ambient bakes), and the mesh wears `MASK_BAKE` during the bake and `MASK_AFFECT_LIGHTMAPPED` after (upstream's scheme — bake lights carry `MASK_BAKE` so they cannot light the mesh again at runtime). DEVIATIONS: no AO virtual lights, no bounces, no BAKE_COLORDIR, no GPU dilate/denoise, and **no cast shadows yet** — the bake captures direct light and ambient only (the CPU baker's shadows are ray-traced and still work). The CPU `Lightmapper` stays as the quality reference (ray-traced AO + soft shadows).

**Lightmapper baker** (`framework/lightmapper/lightmapper.h/.cpp`, 2026-07-14): a **CPU** baker (upstream is a GPU UV-space renderer — DEVIATION). `addLight()` (directional/point/spot) + `addOccluder(mesh, worldTransform)` (world triangles for ray casting) + `bake(targetMesh, worldTransform, Options)` → RGBA8 texture (or `bakeAndApply(material, ...)`). Options mirror upstream's scene-level bake knobs: `sizeMultiplier`/`maxResolution` derive a per-mesh resolution from world bounds (upstream `calculateLightmapSize`), `ambientBake` + `ambientBakeNumSamples`/`SpherePart`/`OcclusionContrast`/`OcclusionBrightness` replace the flat AO term with rays distributed over the top part of the sphere shaped by upstream's `bakeLmEnd` curve, `filterEnabled`/`filterRange`/`filterSmoothness` run a bilateral denoise, and per-light `bakeNumSamples`/`bakeArea` give directional lights soft shadows (upstream spreads N virtual lights over the cone; the ray tracer jitters the shadow ray instead). Per target mesh it reads CPU vertex/index storage (`VertexBuffer::storage()` as 56-byte `PackedVertex`, uv1 at offset 48), rasterizes triangles in **UV1 space** (barycentric per texel → world pos+normal), and shades: direct lighting (Lambert × attenuation + spot cone) with **hard shadow rays**, cosine-weighted-hemisphere **ambient occlusion**, ambient+sky terms AO-modulated; then dilates seams and sRGB-encodes (the shader pow(2.2)-decodes). Ray any-hit uses a **median-split BVH** over occluder triangles; the expensive shading phase is multi-threaded (`std::thread::hardware_concurrency`). ~0.9 s for a 512² map with 24 AO samples over 4.6k triangles. DEVIATIONS: LDR RGBA8 only, single bounce (no GI), no color+dir directional lightmaps, no auto lightmap-size/UV-unwrap (uses the mesh's existing UV1 — box faces overlap, so bake receiver-only planes). Mask a lightmapped mesh out of realtime lights with `MeshInstance::setMask(MASK_AFFECT_LIGHTMAPPED)`. Example: `lightmap-bake-example.cpp` (floor baked with soft shadows + AO from occluder boxes/sphere; toggles the lightmap on/off). Test asset: `assets/textures/lightmap-pools.tga` (render-to-texture example ground).

### GPU Profiler
`GraphicsDevice::gpuProfiler()` (nullptr when unsupported; disabled by default — `setEnabled(true)`). Metal impl (`metalGpuProfiler.*`): MTLCounterSampleBuffer stage-boundary timestamps attached per render pass in `startRenderPass` (start-of-vertex → end-of-fragment), 3 triple-buffered sample-buffer slots resolved 2 frames late, tick→ns via correlated `sampleTimestamps`. Results: `passTimings()` (per-pass ms, named via `RenderPass::name()`) + `frameMilliseconds()`. Compute passes not yet instrumented. NOTE: metal-cpp framework extern constants (e.g. `MTL::CommonCounterSetTimestamp`) only link in the `*_PRIVATE_IMPLEMENTATION` TU — compare string values instead inside the engine library.

### GPU Particle System
`engine/src/scene/particles/` + `framework/components/particlesystem/` (upstream particle-system component, GPU-sim subset; implemented 2026-07-13): **ParticleSystemComponent** — mutate `options()` then `apply()`; `play/pause/stop/reset`. Simulation is a backend-agnostic compute dispatch (`ParticleEmitter::simulate` builds a `Compute` over the kernels in `scene/particles/particleSimShaders.h` — MSL and GLSL under one name — and calls `GraphicsDevice::computeDispatch`, ordered before the frame's render encoding) over a persistent 48-byte `GpuParticle` pool: deterministic staggered births (no atomics; particle i born at `i*birthInterval`, respawn keeps the stream continuous), hash-seeded spawn (box/sphere shapes), velocity base+spread, gravity, damping, per-particle lifetime/rotation ranges. Rendering mirrors the gsplat branch: `MeshInstance::particleEmitter()` keyed instanced tri-strip quad per particle, self-contained billboard shader (particle pool vertex slot 7, `GpuParticleRenderParams` slot 11 — SHARED with gsplat slots, a draw is one or the other), curves (`scaleGraph`/`colorGraph`/`alphaGraph`) quantized to 16-sample LUTs in the render params, sprite-sheet animation (`animTilesX/Y`, `animNumFrames`), additive/normal/premultiplied blending, optional `colorMap` bound via the material baseColor slot (procedural soft disc when null), `intensity` for HDR glow. Component update hooks the engine "update" event; the emitter mesh instance sets `cull=false` (world-space particles ignore the node transform). DEVIATIONS: GPU path only (no CPU sim), no sorting, unlit, screen-aligned billboards only (no stretch/alignToMotion/mesh particles), constant initial velocity instead of velocity/radial graphs, no wrap/depth-softening/pre-warm. NOTE: `MetalParticleComputePass` (flow-viz velocity-field advector) is a separate, unrelated compute pass. The sim kernel + billboard shader (and the gsplat shader) live as editable `.metal` files under `engine/shaders/metal/embedded/` and are wrapped into raw-string constants at build time by `tools/embed_msl.cmake` (CMake `add_custom_command` → generated `.inc` `#include`d inside the anonymous namespace; regenerates on `.metal` edit, no runtime filesystem dependency — DEVIATION from the runtime-loaded ShaderChunks registry, matching the compile-time `areaLightLuts` embed). Sprite-sheet animation is `animTilesX/Y` + `animNumFrames` + `animIndex`, where animIndex selects WHICH animation in the sheet to play: each is animNumFrames tiles long and they run in reading order, so a 4x4 sheet at 4 frames holds four animations. Example: `particles-anim-index-example.cpp`, a port of upstream `graphics/particles-anim-index` (four emitters sharing one sheet, one animIndex each), verified on both backends 2026-09-05. It has to call `options.registerComponentSystem<ParticleSystemComponentSystem>()` in `configure`: component systems come from `AppOptions::componentSystems`, so a component whose system no application registers is constructed and then never updated. Upstream's other counterparts, `graphics/particles-spark` / `particles-snow` / `particles-random-sprites` / `particles-mesh`, are not ported.

### Parallax Occlusion Mapping
`VT_FEATURE_PARALLAX` (upstream `parallax.js`, plus the 2.22 additions): the height
map displaces every texture UV before any map is sampled, so colour, normal,
metal/rough, occlusion and emissive all read the displaced point. Turn it on with
`StandardMaterial::setHeightMap`; `setHeightMapFactor` is the displacement depth in TENTHS of a uv tile, upstream's
unit (default **0.1**, upstream's 2.22 value — this was 0.05 before, a deliberate
BREAKING change for anything that relied on the default).

- **`setHeightMapBase`** is the height-map value that sits at the level of the
  geometry, upstream's meaning. Below it the field sinks into the polygon, above
  it the field stands proud. 1 is pure depth below the surface, which is what the
  port marched before the parameter existed; the default **0.5** pivots the relief
  around mid-grey, as upstream's engine default does. The base moves the ray's
  ENTRY UV as well as the depths — see the gotcha in `AGENTS.md`, because shifting
  only the depths is a no-op that looks like it works.
- **`setHeightMapShadow`** (0..1, default 0 = off) marches the height field a
  second time toward the light and darkens texels the ray passes over, which is
  self-shadowing the cascade map cannot do because it only knows the flat polygon.
  DEVIATION: only the DIRECTIONAL light pays for it; local lights would each need
  their own march. The shadow march samples at an explicit LOD because it runs
  inside the light loop, behind fragment-varying control flow; the view march
  keeps implicit LOD and therefore the height map's mips.

The march itself is an adaptive 8-32 step search with a linear crossing solve,
shared by both backends as `common-parallax.metal` / `common-parallax.glsl`.
Example: `parallax-mapping-example.cpp`, a port of upstream
`materials/parallax-mapping` — a closed brick room and a brick sphere under a spot
and an omni light.

### App-facing Compute + Storage Draws
Upstream's `compute/particles` needs two things an application can reach: a compute
shader over app-owned storage buffers, and a draw that expands one instance per record
in the same buffer. Both landed 2026-08-21 on BOTH backends.

**`Compute` parameters** (`platform/graphics/compute.h`): alongside the existing texture
parameters there are now storage buffers (`setParameter(name, shared_ptr<VertexBuffer>)`
— `VertexBuffer` is the engine's generic GPU storage vehicle, so the same object also
binds to a draw) and loose scalars (`setParameter(name, float|uint32_t)`), collapsed into
one uniform block. `setThreadgroupSize` replaces the hardcoded 8x8x1 (still the default,
so the edge-detect kernel is unaffected).

**DEVIATION — no reflection.** Upstream reflects resources out of the WGSL source and
builds the bind group from the reflected names. This port has none, so binding indices
come from parameter NAMES in sorted order: buffers 0..b-1, textures b..b+t-1, the uniform
block at b+t, and the block's members are the scalars again in name order. A shader that
declares them in a different order silently reads the wrong data. A texture-only compute
keeps the indices it had before, which is why edge-detect needed no changes.

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
`engine/src/scene/gsplat/` + `framework/components/gsplat/`: 3DGS PLY loading (`GSplatData::loadPly` — binary LE), CPU-precomputed covariance (Sigma = R·S²·Rᵀ) in a 40-byte `GpuSplat` storage buffer (vertex slot 7), background `GSplatSorter` thread (upstream sort-worker counting sort; order reversed farthest-first + behind-camera trim via instance count) filling ping-pong order buffers (slot 8), self-contained Metal shader (`shaders/metal/embedded/gsplat-render.metal`: EWA screen-space covariance projection per upstream `gsplatCorner.js`, normExp falloff, premultiplied alpha, `CULLFACE_NONE` — screen-space quads have no winding) drawn as one instanced tri-strip quad per splat via a renderer branch keyed on `MeshInstance::gsplatInstance()`. Params at vertex slot 11. `GSplatComponent::setResource()` wires it to an entity.

**Tier 2 (2026-07-14):**
- **View-dependent SH** (bands 1-3): the generic PLY header parser reads `f_rest_*` (9/24/45 coeffs → bands 1/2/3), dequantizes them coefficient-major interleaved (`[c0.rgb, c1.rgb, ...]`, 45 floats/splat zero-padded) into a per-splat SH storage buffer (vertex slot 12). The shader evaluates `gsplatEvalSH` (upstream `gsplatEvalSH.js` basis) by the model-space view direction `normalize(transpose(mat3(modelView)) · viewPos)` and adds it to the DC color in display/gamma space before the sRGB→linear decode. `shBands` rides in `GpuGSplatParams` (runtime branch, no shader variant; SH0 assets bind a 1-float dummy at slot 12). DEVIATION: upstream quantizes SH to 11-10-11 in a texture; this stores raw floats.
- **Compressed `.compressed.ply`** (SuperSplat format): auto-detected by a leading `chunk` element. Per-256-splat chunk min/max bounds (12 or 18 floats) + a uint `vertex` element — 11-10-11 unorm position/scale lerped into the chunk box, 2-10-10-10 largest-component quaternion, 8888 color — dequantized through the SAME covariance/color path as uncompressed (`buildSplat` helper). Optional uchar `sh` element (channel-major, `u8·8/255−4`). ~4× smaller than float PLY. DEVIATION: no WebP-packed SOG format, no unified octree streaming/LOD path.

Example: `gsplat-example` (port of upstream `gaussian-splatting/simple` — a CC-BY-4.0 `tamiya-dt03.compressed.ply` capture on upstream's ground/PCSS-light/orbit setup), which exercises the compressed path. NO example covers SH bands 1-3 any more: `gsplat-tier2-example` was deleted 2026-08-15 (its nearest upstream counterpart, `gaussian-splatting/spherical-harmonics`, is the `simple` scene with a different asset). SH parsing is still implemented and untested by any example — reinstate one if you touch it. NOT ported: WebP SOG, unified octree streaming/LOD (~13k upstream lines).

### Spec-Gloss / Oren-Nayar / Detail Normals / Displacement
The last four stubbed `VT_FEATURE_*` shader features (implemented 2026-07-13):
- **Spec-gloss** (`VT_FEATURE_SPEC_GLOSS`, KHR_materials_pbrSpecularGlossiness): `StandardMaterial::setUseSpecGloss(true)` + `setSpecularColor`/`setGlossiness`/`setSpecGlossMap` — F0 = specular color, roughness = 1-glossiness, diffuse scaled by `1-max(spec)`. The spec-gloss texture reuses the metal-rough binding (slot 3, rgb=sRGB specular, a=glossiness). The GLB parser applies the true parameterization (plus the old metal-rough approximation as fallback state). GOTCHA: the texture sample MUST be gated on the `hasSpecGlossMap` flags bit (21) — an unbound Metal texture on Apple GPUs reports nonzero `get_width()` but samples zero, silently zeroing specular/gloss for factor-only materials (same trap as the env-atlas bug).
- **Oren-Nayar diffuse** (`VT_FEATURE_OREN_NAYAR`): `setUseOrenNayar(true)` swaps Lambert `N·L` for the fast qualitative Oren-Nayar form (sigma² = roughness²) in both the multi-light loop and the clustered path.
- **Detail normals** (`VT_FEATURE_DETAIL_NORMALS`): `setDetailNormalMap` + `setDetailNormalScale` + `setDetailNormalTransform` — UDN blend (detail xy added to base normal xy) at fragment slot **23**, own UV transform.
- **Displacement** (`VT_FEATURE_DISPLACEMENT`): `setDisplacementMap` + `setDisplacementScale`/`setDisplacementBias` — vertex-stage height sampling (`level(0)`) displaces along the normal before skinning/morph composition. The map routes through a slot>=100 sentinel in `Material::getTextureSlots` to VERTEX texture slot 0 (`MetalTextureBinder`). DEVIATION: standard vertex path only (not instanced/dynamic-batch/skinned).
Example: `material-stubs-example.cpp` (four spheres A/B-cycling all four features with procedural textures).

### Scalar Material Maps (gloss / thickness / refraction)
`StandardMaterial::setGlossMap` + `setThicknessMap` + `setRefractionMap`, each with a
`set*MapChannel(MapChannel)` selector (upstream's `glossMapChannel` etc., default **G**
for all three so one packed texture can drive them). Each map multiplies its scalar
factor by one channel: gloss scales the gloss factor and REPLACES the roughness derived
from the metal-rough map (upstream treats them as alternative sources, not a product);
thickness and refraction scale `thickness` and `transmissionFactor` for both refraction
paths.

`glossMap` existed as a property before but was **dead** — never bound, never sampled.
Turning it on changes any scene that already set one (`area-light`'s floor now has
spatially varying gloss, which is the intended upstream look).

Presence rides in the SIGN of `MaterialUniforms::mapChannelParams` (`{glossFactor,
glossChannel, thicknessChannel, refractionChannel}`, negative = no map) because the
material `flags` word has no spare bits left — 25-27 and 29-31 are the two dither modes.

Slots 0-30 were all taken, so these are fragment slots **31/32/33** and
`MetalTextureBinder::kMaxTextureSlots` went 31 -> 34. Adding a field to
`MaterialUniforms` means updating FOUR places or the build breaks:
`scene/materials/material.h`, `shaders/metal/chunks/common-structs.metal`,
`shaders/vulkan/forward.{frag,vert}`, and BOTH size checks — the `static_assert` in
`vulkanRenderPipeline.cpp` and the reflected-block table in
`tools/generate_vulkan_shader_bundle.py` (the latter fails the build with a
`RuntimeError`, not a compiler `error:`, so a grep for "error:" misses it).

DEVIATION: the three maps are **Metal only**. Vulkan's fragment stage already declares
15 combined image samplers and MoltenVK inherits a 16-per-stage limit, so three more
would need the separate-image + shared-sampler treatment the light cookies use. The
uniform field is plumbed on both backends, so Vulkan renders the scene correctly minus
the maps.

Example: `refraction-example.cpp` (port of upstream `materials/material-refraction`).

### Material System
- `Material` base: glTF PBR metallic-roughness, `MaterialUniforms` struct (352 bytes) matches GPU `MaterialData`
- `StandardMaterial`: Full PBR (clearcoat, anisotropy, sheen, iridescence, transmission, parallax)
- `ShaderMaterial`: Custom Metal shader with user entry points

## Engine subsystems
These moved out of `CLAUDE.md` for size. They are reference, not rules; the
rules they imply are repeated in that file's gotcha list.
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
(now keyed on the function too); Vulkan reads it in `vulkanRenderPipeline` and the
PSO key already covered it via `DepthState::key()`. `Greater` + `depthWrite(false)`
is the x-ray trick in `layers-example` — the mesh draws only where something
already rendered in front of it.

## ECS

- `GraphNode` -> `Entity` -> Components via `ComponentSystem<T>` registry
- O(1) component lookup via `unordered_map<ComponentTypeID, Component*>`
- `GraphNode::lookAt(target, up = +Y)` aims the node's -Z at a world-space target,
  setting WORLD rotation. Some examples predating it still carry local yaw/pitch
  helpers; those are equivalent.
- **12 component types:** Camera, Render, Light, Script, Animation, Anim (state
  graph), Screen, Element, Button, Collision, RigidBody, GSplat, ParticleSystem
- **Component systems are supplied by the APPLICATION**, not by the engine:
  `Engine` registers whatever `AppOptions::componentSystems` carries. The examples
  harness registers Render, Camera, Light and Script; anything else is one
  `options.registerComponentSystem<T>()` line in the example's `configure`. A
  component whose system nobody registered still constructs — it just never gets
  its per-frame update, so it looks implemented and inert at the same time.

## Graphics abstraction

- `GraphicsDevice` (abstract) -> `MetalGraphicsDevice` / `VulkanGraphicsDevice`
- Triple-buffered ring buffers for uniforms
- Per-pass texture/uniform binding deduplication
- Pipeline state caching via `MetalRenderPipeline` / `VulkanRenderPipeline`
  (+ `MetalComputePipeline`)
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
- Parsers: GLB (tinygltf), OBJ (tinyobjloader), STL, Assimp. The GLB parser reads
  KHR_materials_transmission/_ior/_volume/_dispersion and generates tangents for
  triangle primitives with no TANGENT attribute.
- `instantiateRenderEntity()` returns the glTF root node for single-root scenes
  (upstream's rule), so `setLocalScale` / `setLocalRotation` REPLACE the model's
  own root transform.
- stb's vertical-flip flag is **thread-local**. Setting the global one does not
  affect a loader-thread decode; this silently flipped every env atlas once.

### Render pass types

**8 render pass types:** `RenderPassForward` (main PBR geometry, multi-light,
multi-layer), `RenderPassShadowDirectional` (cascades, PCF + EVSM_16F),
`RenderPassVsmBlur`, `RenderPassShadowLocalClustered` / `NonClustered`,
`RenderPassUpdateClustered`, `RenderPassPostprocessing`, `RenderPassCookieRenderer`.

### The env family, the last effects still on the device vtable

The env family is the remaining real work and is not a mechanical lift. It runs
OUTSIDE the frame loop at asset-load time, on each backend's own offline channel,
and there is no way to BEGIN a render pass outside a frame — Metal is close,
Vulkan is not (frame command buffer, uniform ring and descriptor pools are all
frame-scoped). The two convolve implementations also genuinely differ: Metal
importance-samples a table of up to 1024 float4 samples, Vulkan approximates with
a roughness parameter and ignores the table. Unifying changes Vulkan's output.
Passing the table as an input TEXTURE would fit the existing 8-slot seam.
Verification exists: `tools/generate-env-atlas` produces deterministic PNGs.

## Live gotchas
