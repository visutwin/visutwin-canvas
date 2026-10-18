// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025
//

#include "frameGraph.h"

#include <algorithm>

namespace visutwin::canvas
{
    void FrameGraph::reset()
    {
        _renderPasses.clear();
    }

    void FrameGraph::addRenderPass(const std::shared_ptr<RenderPass>& renderPass)
    {
        // A disabled pass is skipped with all its before and after passes, and is not updated.
        if (!renderPass->enabled()) {
            return;
        }

        // Can add or remove before and after passes, and enable or disable them.
        renderPass->frameUpdate();

        for (const auto& pass : renderPass->beforePasses()) {
            addRenderPass(pass);
        }

        // frameUpdate can disable the pass, which then stays out of the compile step.
        if (renderPass->enabled()) {
            _renderPasses.push_back(renderPass);
        }

        for (const auto& pass : renderPass->afterPasses()) {
            addRenderPass(pass);
        }
    }

    void FrameGraph::compile() {
        // Reset merge flags from previous compiles: passes are persistent objects
        // reused across frames, and stale skipStart/skipEnd from a frame with
        // different pass adjacency would skip render-target begin/end incorrectly.
        // The same for the stores and mipmap generation this graph changes below: they
        // are undone first and derived afresh from THIS frame's adjacency.
        for (auto renderPass : _renderPasses) {
            renderPass->setSkipStart(false);
            renderPass->setSkipEnd(false);
            renderPass->undoGraphAttachmentEdits();
        }

        // The map tracks the most recent pass per target WITHIN THIS FRAME. Passes are
        // persistent objects reused across frames, so a stale entry would make the first
        // pass of a new frame propagate stores onto last frame's pass (and keep it alive).
        _renderTargetMap.clear();

        for (auto renderPass : _renderPasses) {
            RenderTarget* renderTarget = renderPass->renderTarget().get();

            // A null target is the default back-buffer and participates like any other:
            // it is a real surface that a later pass can read without clearing. Skipping it
            // here meant a back-buffer pass was never told to store, so when the scene color
            // grab split the frame into two back-buffer passes, the second one loaded a depth
            // buffer the first had discarded — the skybox then passed the depth test
            // everywhere and repainted over every opaque draw from the first pass.
            //
            // Passes with no color ops at all (the grab passes, which only blit) carry no
            // load/store contract of their own, so they must not displace the previous pass
            // in the map — otherwise the pass that actually reads the surface would look at
            // the grab instead of the draw pass that filled it.
            const auto& colorArrayOps = renderPass->colorArrayOps();
            const auto depthStencilOps = renderPass->depthStencilOps();
            const bool participates = !colorArrayOps.empty() || depthStencilOps != nullptr;

            // previous pass using the same render target
            if (auto it = _renderTargetMap.find(renderTarget); it != _renderTargetMap.end()) {
                auto prevPass = it->second;

                // if we use the RT without clearing, make sure the previous pass stores data
                const auto& prevColorOps = prevPass->colorArrayOps();
                const size_t count = std::min(colorArrayOps.size(), prevColorOps.size());
                for (size_t j = 0; j < count; j++) {
                    const auto colorOps = colorArrayOps[j];
                    if (!colorOps->clear) {
                        prevPass->setAttachmentFlagByGraph(prevColorOps[j], prevColorOps[j]->store, true);
                    }
                }

                if (const auto prevDepth = prevPass->depthStencilOps(); depthStencilOps && prevDepth) {
                    if (!depthStencilOps->clearDepth) {
                        prevPass->setAttachmentFlagByGraph(prevDepth, prevDepth->storeDepth, true);
                    }
                    if (!depthStencilOps->clearStencil) {
                        prevPass->setAttachmentFlagByGraph(prevDepth, prevDepth->storeStencil, true);
                    }
                }
            }

            // add the pass to the map
            if (participates) {
                _renderTargetMap[renderTarget] = renderPass;
            }
        }

        // merge passes if possible
        if (_renderPasses.size() < 2) return;
        for (size_t i = 0; i < _renderPasses.size() - 1; i++) {
            auto firstPass = _renderPasses[i];
            auto firstRT = firstPass->renderTarget();
            auto secondPass = _renderPasses[i + 1];
            auto secondRT = secondPass->renderTarget();

            // if the render targets are different, we can't merge the passes
            // also only merge passes that have a render target. A null target is the BACK
            // BUFFER, which merges like any other; a pass with no target at all (a grab, an
            // orchestrator) starts no device pass and never merges. Refusing every null
            // target left a camera frame's compose and its after pass (the UI layer) as two
            // back-buffer passes, the second loading and re-resolving what the first stored.
            if (firstRT != secondRT || !firstPass->hasRenderTarget() || !secondPass->hasRenderTarget()) {
                continue;
            }

            // do not merge if the second pass clears any of the attachments
            const auto secondDepthStencilOps = secondPass->depthStencilOps();
            if (secondDepthStencilOps->clearDepth ||
                secondDepthStencilOps->clearStencil) {
                continue;
            }

            const auto& secondColorArrayOps = secondPass->colorArrayOps();
            bool anyColorClear = false;
            for (const auto& colorOps : secondColorArrayOps) {
                if (colorOps->clear) {
                    anyColorClear = true;
                    break;
                }
            }
            if (anyColorClear) {
                continue;
            }

            // first pass cannot contain after passes
            if (firstPass->afterPasses().size() > 0) {
                continue;
            }

            // second pass cannot contain before passes
            if (secondPass->beforePasses().size() > 0) {
                continue;
            }

            // merge the passes
            firstPass->setSkipEnd(true);
            secondPass->setSkipStart(true);
        }

        // A merged pass is begun with its FIRST pass's ops, and a backend fixes the end of
        // the pass at begin (Metal's store and resolve actions, Vulkan's dynamic rendering
        // info): so the first pass ends the merged pass the way its LAST pass would. The
        // store raised above because the next pass loads the target is an encoder-internal
        // load now, and storing it anyway writes a whole MSAA surface (and its depth) that
        // nothing reads. Walked backwards so a chain of three takes the third's ops. The
        // edits are the graph's, undone at the next compile like the stores above.
        // DEVIATION: upstream keeps the first pass's raised stores.
        for (size_t i = _renderPasses.size() - 1; i-- > 0;) {
            const auto& firstPass = _renderPasses[i];
            if (!firstPass->skipEnd()) {
                continue;
            }
            const auto& lastPass = _renderPasses[i + 1];
            const auto& firstColorOps = firstPass->colorArrayOps();
            const auto& lastColorOps = lastPass->colorArrayOps();
            const size_t count = std::min(firstColorOps.size(), lastColorOps.size());
            for (size_t j = 0; j < count; j++) {
                firstPass->setAttachmentFlagByGraph(firstColorOps[j], firstColorOps[j]->store, lastColorOps[j]->store);
                firstPass->setAttachmentFlagByGraph(firstColorOps[j], firstColorOps[j]->resolve,
                    lastColorOps[j]->resolve);
            }
            const auto firstDepth = firstPass->depthStencilOps();
            const auto lastDepth = lastPass->depthStencilOps();
            if (firstDepth && lastDepth) {
                firstPass->setAttachmentFlagByGraph(firstDepth, firstDepth->storeDepth, lastDepth->storeDepth);
                firstPass->setAttachmentFlagByGraph(firstDepth, firstDepth->storeStencil, lastDepth->storeStencil);
                firstPass->setAttachmentFlagByGraph(firstDepth, firstDepth->resolveDepth, lastDepth->resolveDepth);
            }
        }

        // Walk over render passes to find passes rendering to the same cubemap texture.
        // If those passes are separated only by passes not requiring cubemap (shadows ..),
        // we skip the mipmap generation till the last rendering to the cubemap, to avoid
        // mipmaps being generated after each face.
        Texture* lastCubeTexture = nullptr;
        std::shared_ptr<RenderPass> lastCubeRenderPass = nullptr;

        for (auto renderPass : _renderPasses) {
            auto renderTarget = renderPass->renderTarget();
            Texture* thisTexture = renderTarget ? renderTarget->colorBuffer() : nullptr;

            if (thisTexture && thisTexture->isCubemap()) {
                // if the previous pass used the same cubemap texture, it does not need mipmaps generated
                if (lastCubeTexture == thisTexture) {
                    auto& lastColorArrayOps = lastCubeRenderPass->colorArrayOps();
                    size_t count = lastColorArrayOps.size();
                    for (size_t j = 0; j < count; j++) {
                        lastCubeRenderPass->setAttachmentFlagByGraph(lastColorArrayOps[j],
                            lastColorArrayOps[j]->genMipmaps, false);
                    }
                }

                lastCubeTexture = renderTarget->colorBuffer();
                lastCubeRenderPass = renderPass;
            } else if (renderPass->requiresCubemaps()) {
                // if the cubemap is required, break the cubemap rendering chain
                lastCubeTexture = nullptr;
                lastCubeRenderPass = nullptr;
            }
        }

        _renderTargetMap.clear();
    }

    void FrameGraph::render(GraphicsDevice* /*device*/) {
        compile();

        // Every pass creates its shaders before any pass draws (RenderPass::prepareShaders).
        for (const auto& pass : _renderPasses) {
            pass->prepareShadersWithChildren();
        }

        for (auto pass : _renderPasses) {
            pass->render();
        }
    }
}