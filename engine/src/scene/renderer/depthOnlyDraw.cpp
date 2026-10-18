// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2026
//
#include "depthOnlyDraw.h"

#include "core/scopedTimer.h"
#include "framework/batching/skinBatchInstance.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/camera.h"
#include "scene/clipPlanes.h"
#include "scene/graphNode.h"
#include "scene/gsplat/gsplatInstance.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/morph.h"
#include "scene/skinInstance.h"
#include "scene/shader-lib/programLibrary.h"
#include "scene/shader-lib/slangShaders.h"
#include "shadowCasterFiltering.h"
#include "cullModeResolve.h"

namespace visutwin::canvas
{
    namespace
    {
        // A splat cloud cast as opaque footprints: every splat, its gaussian clipped at the
        // instance's alpha clip, from the pass's camera at the viewport's pixel size.
        bool drawGSplatDepthOnly(GraphicsDevice* device, MeshInstance* meshInstance, GSplatInstance& gsplat,
            const Matrix4& viewProjection, DepthOnlyShaders& shaders, Camera* camera)
        {
            GSplatResource* resource = gsplat.resource();
            // A pass that never set its viewport would size every footprint to nothing.
            if (!camera || !camera->node() || !resource || !resource->splatBuffer() || resource->numSplats() <= 0 ||
                device->vw() <= 0.0f || device->vh() <= 0.0f) {
                return false;
            }
            if (!shaders.gsplat) {
                shaders.gsplat = getOrCreateSlangShader(device, "gsplat-render", shaders.vsm ? "shadow-vsm" : "shadow");
                if (!shaders.gsplat) {
                    return false;
                }
            }
            const bool orthographic = camera->projection() == ProjectionType::Orthographic;
            // A perspective VSM pass stores the distance from the light (a spot), as the mesh
            // casters' moments do; an orthographic one the depth.
            const int vsmMode = shaders.vsm ? (orthographic ? 1 : 2) : 0;
            const Matrix4 model = meshInstance->node() ? meshInstance->node()->worldTransform() : Matrix4::identity();
            const GpuGSplatParams params = gsplat.shadowParams(model, camera->node()->worldTransform().inverse(),
                camera->projectionMatrix(), device->vw(), device->vh(), orthographic, vsmMode, camera->farClip());

            device->setCullMode(CullMode::CULLFACE_NONE);
            device->setMaterial(nullptr);
            device->setShader(shaders.gsplat);
            device->setVertexBuffer(meshInstance->mesh()->getVertexBuffer(), 0);
            resource->splatBuffer()->markStorageUse();
            // The shadow variant reads no order; the splats stand in for the order binding.
            device->setGSplatState(resource->splatBuffer(), resource->splatBuffer(), resource->shBuffer(),
                &params, sizeof(params));
            device->setTransformUniforms(viewProjection, model);
            device->draw(meshInstance->mesh()->getPrimitive(), meshInstance->mesh()->getIndexBuffer(),
                resource->numSplats(), -1, true, true);
            device->setShader(shaders.plain);
            return true;
        }
    }

    void prepareDepthOnlyShaders(const std::shared_ptr<GraphicsDevice>& device, const bool vsm)
    {
        if (const auto programLibrary = getProgramLibrary(device)) {
            programLibrary->prepareDepthOnlyShaders(vsm);
        }
    }

    bool drawDepthOnly(GraphicsDevice* device, ProgramLibrary* programLibrary,
        MeshInstance* meshInstance, const Matrix4& viewProjection, DepthOnlyShaders& shaders,
        Camera* camera)
    {
        if (!device || !programLibrary || !meshInstance || !meshInstance->mesh()) {
            return false;
        }
        // A screen-space instance's vertices are already in clip space; no depth-only
        // view of it means anything (see MeshInstance::setScreenSpace).
        if (meshInstance->screenSpace()) {
            return false;
        }

        if (auto* gsplat = meshInstance->gsplatInstance()) {
            if (!drawGSplatDepthOnly(device, meshInstance, *gsplat, viewProjection, shaders, camera)) {
                return false;
            }
            meshInstance->setVisibleThisFrame(true);
            return true;
        }

        meshInstance->setVisibleThisFrame(true);
        // The caster's own cull mode, as the forward pass applies it. Left unset,
        // the depth passes drew with whatever the previous draw had chosen: the
        // device default on the first frame and the last full-screen quad's
        // CULLFACE_NONE on every frame after — so a torch light sitting inside its
        // handle escaped the cylinder once (back faces culled) and was sealed in by
        // it forever after, on ambient-occlusion's one-shot omni shadows.
        device->setCullMode(resolveCullMode(meshInstance->material(), meshInstance->node()));
        device->setVertexBuffer(meshInstance->mesh()->getVertexBuffer(), 0);

        // A caster whose depth depends on its material — masked alpha, or shadow
        // dithering — needs its uniforms and base-colour texture bound so the shader
        // can run the opacity frontend before writing depth. Every other caster binds
        // nothing, exactly as the depth-only passes always did.
        const Material* frontendMaterial = shadowFrontendMaterial(meshInstance);

        // A clipped caster discards what its planes, and the pass camera's, clip. The
        // planes ride in the instance's copy of its material's block, so the material is
        // bound for it whether or not its opacity decides the depth.
        Material* material = meshInstance->material();
        const ClipPlanes* cameraClip = shaders.cameraClip && !shaders.cameraClip->empty() ? shaders.cameraClip : nullptr;
        uint32_t clip = CLIP_PLANES_NONE;
        if (material) {
            clip |= meshInstance->clipped() ? CLIP_PLANES_MESH : 0u;
            clip |= cameraClip ? CLIP_PLANES_CAMERA : 0u;
        }
        device->setMaterial(clip ? material : frontendMaterial);
        const MaterialUniforms* clipUniforms = clip ? meshInstance->overriddenUniforms(material, cameraClip) : nullptr;
        device->setInstanceMaterialUniforms(clipUniforms, clipUniforms ? meshInstance->overriddenUniformsVersion() : 0);

        const auto* mesh = meshInstance->mesh();
        const auto& instancing = meshInstance->instancingData();
        const bool isInstanced = instancing.vertexBuffer && instancing.count > 0;

        if (isInstanced) {
            // Hardware instancing: the caster cloud transforms through the per-instance
            // matrices in the vertex stage, so bind the instance buffer at slot 5 and
            // draw instanced — the same contract the forward pass uses. Without this
            // every instance would draw from the mesh instance's own node transform.
            const bool instanceColor = instancing.vertexBuffer->format() &&
                instancing.vertexBuffer->format()->hasInstanceColor();
            auto& cachedVariant = instanceColor ? shaders.instancedColor : shaders.instanced;
            if (const auto variant = shadowCasterShader(programLibrary, frontendMaterial,
                    cachedVariant, false, false, false, true, instanceColor, shaders.vsm, clip)) {
                device->setShader(variant);
            }
            device->setVertexBuffer(instancing.vertexBuffer, 5);
            // Instances live in the node's space (matrix_model * instance).
            device->setTransformUniforms(viewProjection, meshInstance->node()
                ? meshInstance->node()->worldTransform() : Matrix4::identity());
            device->draw(mesh->getPrimitive(), mesh->getIndexBuffer(), instancing.count, -1, true, true);
            // Unbind the per-instance buffer — the backends pick the instancing vertex
            // layout by scanning the bound slots, so a leftover binding would follow the
            // next, non-instanced caster into its pipeline.
            device->setVertexBuffer(nullptr, 5);
            device->setShader(shaders.plain);
        } else if (meshInstance->isDynamicBatch()) {
            if (const auto variant = shadowCasterShader(programLibrary, frontendMaterial,
                    shaders.dynamicBatch, true, false, false, false, false, shaders.vsm, clip)) {
                device->setShader(variant);
            }
            if (auto* sbi = meshInstance->skinBatchInstance()) {
                device->setDynamicBatchPalette(sbi->paletteData(), sbi->paletteSizeBytes(),
                    sbi->paletteVersion());
            }
            device->setTransformUniforms(viewProjection, Matrix4::identity());
            device->draw(mesh->getPrimitive(), mesh->getIndexBuffer(), 1, -1, true, true);
            device->setShader(shaders.plain);
        } else if (meshInstance->skinInstance() || meshInstance->morphInstance()) {
            // Skinned/morphed caster: switch to the matching variant, bind the bone
            // palette (slot 6) and/or the morph buffers (slots 9/10).
            const bool skinned = meshInstance->skinInstance() != nullptr;
            const bool morphed = meshInstance->morphInstance() != nullptr;
            auto& cachedVariant = skinned
                ? (morphed ? shaders.skinnedMorphed : shaders.skinned)
                : shaders.morphed;
            if (const auto variant = shadowCasterShader(programLibrary, frontendMaterial,
                    cachedVariant, false, skinned, morphed, false, false, shaders.vsm, clip)) {
                device->setShader(variant);
            }
            if (skinned) {
                auto* si = meshInstance->skinInstance();
                {
                    // A shadow pass often reaches a skin before the forward pass does,
                    // so the palette update is timed wherever it actually happens.
                    const ScopedMilliseconds skinTimer(device->frameCounters().skinTime);
                    si->updateMatrixPalette(meshInstance->node());
                }
                device->setDynamicBatchPalette(si->paletteData(), si->paletteSizeBytes(),
                    si->paletteVersion());
            }
            if (morphed) {
                auto* mi = meshInstance->morphInstance();
                if (mi->morph() && mi->morph()->deltaBuffer()) {
                    const MorphInstance::GpuMorphParams* params = nullptr;
                    {
                        const ScopedMilliseconds morphTimer(device->frameCounters().morphTime);
                        params = &mi->gpuParams();
                    }
                    device->setMorphState(mi->morph()->deltaBuffer(), params, sizeof(*params));
                }
            }
            const auto modelMatrix = meshInstance->node()
                ? meshInstance->node()->worldTransform() : Matrix4::identity();
            device->setTransformUniforms(viewProjection, modelMatrix);
            device->draw(mesh->getPrimitive(), mesh->getIndexBuffer(), 1, -1, true, true);
            device->setShader(shaders.plain);
        } else {
            // The pass-wide shader is already bound; a caster with an opacity frontend
            // swaps in its own variant and puts the plain one back, as above.
            if (frontendMaterial || clip) {
                if (const auto variant = programLibrary->getShadowShader(frontendMaterial,
                        false, false, false, false, false, shaders.vsm, clip)) {
                    device->setShader(variant);
                }
            }
            const auto modelMatrix = meshInstance->node()
                ? meshInstance->node()->worldTransform() : Matrix4::identity();
            device->setTransformUniforms(viewProjection, modelMatrix);
            device->draw(mesh->getPrimitive(), mesh->getIndexBuffer(), 1, -1, true, true);
            if (frontendMaterial || clip) {
                device->setShader(shaders.plain);
            }
        }
        if (clipUniforms) {
            device->setInstanceMaterialUniforms(nullptr, 0);
        }
        return true;
    }
}
