// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#pragma once

#include <memory>
#include <vector>

#include "core/math/primitives.h"

namespace visutwin::canvas
{
    class Camera;
    class Material;
    class MeshInstance;
    class ProgramLibrary;
    class RenderComponent;
    class Shader;

    // Checks component/entity state and camera layer compatibility for shadow casting.
    bool shouldRenderShadowRenderComponent(const RenderComponent* renderComponent, const Camera* camera);

    // Collects every shadow caster in the scene: each enabled RenderComponent's mesh
    // instances plus the batch mesh instances, which belong to no RenderComponent and
    // would otherwise cast nothing. Appends; does not clear.
    //
    // `camera` filters components by layer compatibility, and a caller that draws or
    // fits for a particular camera must pass it. Every caller that collects casters
    // goes through here rather than sweeping itself — the directional fit and the
    // directional pass used to have one hand-written sweep each, they disagreed about
    // whether batch meshes were casters, and the fit therefore sized the shadow map's
    // depth range to the unbatched scene while the pass drew batches into it.
    void collectShadowCasters(std::vector<MeshInstance*>& casters, const Camera* camera = nullptr);

    // The mesh-level caster rules that do NOT depend on a camera: castShadow, node
    // state, material transparency (with the alpha-test and dithered-shadow
    // exceptions) and the presence of geometry. Split out so a caller that does its
    // own visibility test — the omni classification, which tests six faces at once —
    // pays these once rather than once per face.
    bool shouldRenderShadowMeshInstanceIgnoringVisibility(MeshInstance* meshInstance);

    // Checks mesh-level shadow caster rules (castShadow/material/frustum/cull/node state).
    bool shouldRenderShadowMeshInstance(MeshInstance* meshInstance, Camera* shadowCamera);

    // Prebuilt-frustum variant: callers looping over many casters should build
    // the shadow camera's frustum once (buildCameraFrustum) and pass it here.
    bool shouldRenderShadowMeshInstance(MeshInstance* meshInstance, Camera* shadowCamera,
        const Frustum& shadowFrustum);

    // The material a shadow draw has to BIND for this caster, or null when the
    // caster's shadow does not depend on its material. Only a masked (alpha-tested)
    // or shadow-dithered caster needs one: the shadow shader then reads its opacity
    // uniforms and samples its base-colour texture before writing depth. Every other
    // caster draws with no material at all, exactly as the shadow passes always did.
    const Material* shadowFrontendMaterial(MeshInstance* meshInstance);

    // The shadow program for one caster. `frontendMaterial` is what
    // shadowFrontendMaterial returned; `cached` is the pass's lazily built variant
    // for this vertex stage and is used — and filled — only for a caster that needs
    // no frontend, so the common caster still costs one library call per pass.
    std::shared_ptr<Shader> shadowCasterShader(ProgramLibrary* programLibrary,
        const Material* frontendMaterial, std::shared_ptr<Shader>& cached,
        bool dynamicBatch = false, bool skinning = false, bool morphing = false,
        bool instancing = false, bool instancingColor = false, bool vsm = false);
}
