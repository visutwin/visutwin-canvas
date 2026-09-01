#version 450

#include "shader_features.glsl"

// Composed from engine/shaders/vulkan/chunks/ — the SAME chunk names the
// Metal backend uses, so ShaderChunks overrides address one shader on both.
// Include order here is the build-time mirror of ProgramLibrary's registered
// order; keep the two in step.
#include "chunks/forward-fragment-head.glsl"
#include "chunks/common-dither.glsl"
#include "chunks/common-parallax.glsl"
#include "chunks/common-shadow-pcss.glsl"
#include "chunks/common-shadow-vsm.glsl"
#include "chunks/common-cookie.glsl"
#include "chunks/common-utils.glsl"
#include "chunks/common-tonemap.glsl"
#include "chunks/common-material-flags.glsl"
#include "chunks/common-ltc.glsl"
#include "chunks/common-brdf.glsl"
#include "chunks/common-atmosphere.glsl"
#include "chunks/forward-fragment-surface.glsl"
#include "chunks/forward-fragment-lights.glsl"
#include "chunks/forward-fragment-clustered.glsl"
#include "chunks/forward-fragment-ambient.glsl"
#include "chunks/forward-fragment-emissive.glsl"
#include "chunks/forward-fragment-tail.glsl"
