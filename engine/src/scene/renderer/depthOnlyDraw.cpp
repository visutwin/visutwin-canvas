// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "depthOnlyDraw.h"

#include "framework/batching/skinBatchInstance.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/graphNode.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/morph.h"
#include "scene/skinInstance.h"
#include "scene/shader-lib/programLibrary.h"
#include "shadowCasterFiltering.h"
#include "cullModeResolve.h"

namespace visutwin::canvas
{
    void drawDepthOnly(GraphicsDevice* device, ProgramLibrary* programLibrary,
        MeshInstance* meshInstance, const Matrix4& viewProjection, DepthOnlyShaders& shaders)
    {
        if (!device || !programLibrary || !meshInstance || !meshInstance->mesh()) {
            return;
        }

        meshInstance->setVisibleThisFrame(true);
        // The caster's own cull mode, as upstream's shadow pass (`setCullMode(true,
        // false, meshInstance)`) and this port's forward pass apply it. Left unset,
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
        device->setMaterial(frontendMaterial);

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
                    cachedVariant, false, false, false, true, instanceColor)) {
                device->setShader(variant);
            }
            device->setVertexBuffer(instancing.vertexBuffer, 5);
            device->setTransformUniforms(viewProjection, Matrix4::identity());
            device->draw(mesh->getPrimitive(), mesh->getIndexBuffer(), instancing.count, -1, true, true);
            // Unbind the per-instance buffer — the backends pick the instancing vertex
            // layout by scanning the bound slots, so a leftover binding would follow the
            // next, non-instanced caster into its pipeline.
            device->setVertexBuffer(nullptr, 5);
            device->setShader(shaders.plain);
        } else if (meshInstance->isDynamicBatch()) {
            if (const auto variant = shadowCasterShader(programLibrary, frontendMaterial,
                    shaders.dynamicBatch, true)) {
                device->setShader(variant);
            }
            if (auto* sbi = meshInstance->skinBatchInstance()) {
                device->setDynamicBatchPalette(sbi->paletteData(), sbi->paletteSizeBytes());
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
                    cachedVariant, false, skinned, morphed)) {
                device->setShader(variant);
            }
            if (skinned) {
                auto* si = meshInstance->skinInstance();
                si->updateMatrixPalette(meshInstance->node());
                device->setDynamicBatchPalette(si->paletteData(), si->paletteSizeBytes());
            }
            if (morphed) {
                auto* mi = meshInstance->morphInstance();
                if (mi->morph() && mi->morph()->deltaBuffer()) {
                    const auto& params = mi->gpuParams();
                    device->setMorphState(mi->morph()->deltaBuffer(), &params, sizeof(params));
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
            if (frontendMaterial) {
                if (const auto variant = programLibrary->getShadowShader(frontendMaterial)) {
                    device->setShader(variant);
                }
            }
            const auto modelMatrix = meshInstance->node()
                ? meshInstance->node()->worldTransform() : Matrix4::identity();
            device->setTransformUniforms(viewProjection, modelMatrix);
            device->draw(mesh->getPrimitive(), mesh->getIndexBuffer(), 1, -1, true, true);
            if (frontendMaterial) {
                device->setShader(shaders.plain);
            }
        }
    }
}
