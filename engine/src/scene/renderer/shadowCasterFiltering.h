// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#pragma once

#include <memory>
#include <vector>

#include "core/math/primitives.h"
#include "core/shape/boundingBox.h"

namespace visutwin::canvas
{
    class Camera;
    class CameraComponent;
    class Material;
    class MeshInstance;
    class ProgramLibrary;
    class RenderComponent;
    class Shader;

    // Checks component/entity state and camera layer compatibility for shadow casting.
    bool shouldRenderShadowRenderComponent(const RenderComponent* renderComponent, const Camera* camera);

    // The same test for a whole sweep: what it needs to know about the camera — its
    // component, found by a search of every camera — is resolved ONCE here instead of
    // once per render component.
    class ShadowCasterComponentFilter
    {
    public:
        explicit ShadowCasterComponentFilter(const Camera* camera);
        [[nodiscard]] bool accepts(const RenderComponent* renderComponent) const;

    private:
        // A lightmap bake camera takes casters from every layer (see accepts).
        bool _everyLayer = false;
        const CameraComponent* _cameraComponent = nullptr;
    };

    // Collects every shadow caster in the scene: each enabled RenderComponent's mesh
    // instances plus the batch mesh instances, which belong to no RenderComponent and
    // would otherwise cast nothing. Appends; does not clear.
    //
    // `camera` filters components by layer compatibility, and a caller that draws or
    // fits for a particular camera must pass it. Every caller that collects casters
    // goes through here rather than sweeping itself: two sweeps drift apart, and if
    // the directional fit and pass disagree about which meshes cast (batch meshes, for
    // one), the fit sizes the shadow map's depth range to a scene the pass does not draw.
    void collectShadowCasters(std::vector<MeshInstance*>& casters, const Camera* camera = nullptr);

    // The mesh-level caster rules that do NOT depend on a camera: castShadow, node
    // state, material transparency (with the alpha-test and dithered-shadow
    // exceptions) and the presence of geometry. Split out so a caller that does its
    // own visibility test — the omni classification, which tests six faces at once —
    // pays these once rather than once per face.
    bool shouldRenderShadowMeshInstanceIgnoringVisibility(MeshInstance* meshInstance);

    // A shadow caster with what a light's bounds test reads, so that test runs over one
    // contiguous array instead of reaching through every mesh instance to its node
    // (and, for a skinned one, re-deriving the bounds from its bones) once per light.
    struct ShadowCasterBounds
    {
        MeshInstance* meshInstance = nullptr;
        // World bounds, taken only when `cull` is set.
        BoundingBox aabb;
        // MeshInstance::cull(): a caster with culling off is in every list, untested.
        bool cull = true;
    };

    // The scene's casters through every rule that depends on neither a light nor a
    // camera: collectShadowCasters, then visible() and
    // shouldRenderShadowMeshInstanceIgnoringVisibility, with each caster's bounds.
    // Clears `casters` first, keeps collection order. What is left per light is a
    // bounds test, so a frame with many shadowed local lights collects and filters the
    // scene once, not once per light.
    void collectLightIndependentShadowCasters(std::vector<ShadowCasterBounds>& casters);

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
