// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
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
        renderPass->frameUpdate();

        auto& beforePasses = renderPass->beforePasses();
        for (auto pass : beforePasses) {
            if (pass->enabled()) {
                addRenderPass(pass);
            }
        }

        if (renderPass->enabled()) {
            _renderPasses.push_back(renderPass);
        }

        for (auto pass : renderPass->afterPasses()) {
            if (pass->enabled()) {
                addRenderPass(pass);
            }
        }
    }

    void FrameGraph::compile() {
        // Reset merge flags from previous compiles: passes are persistent objects
        // reused across frames, and stale skipStart/skipEnd from a frame with
        // different pass adjacency would skip render-target begin/end incorrectly.
        for (auto renderPass : _renderPasses) {
            renderPass->setSkipStart(false);
            renderPass->setSkipEnd(false);
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
                        prevColorOps[j]->store = true;
                    }
                }

                if (depthStencilOps && prevPass->depthStencilOps()) {
                    if (!depthStencilOps->clearDepth) {
                        prevPass->depthStencilOps()->storeDepth = true;
                    }
                    if (!depthStencilOps->clearStencil) {
                        prevPass->depthStencilOps()->storeStencil = true;
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
            // also only merge passes that have a render target
            if (firstRT != secondRT || firstRT == nullptr) {
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
                        lastColorArrayOps[j]->genMipmaps = false;
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

    void FrameGraph::render(GraphicsDevice* device) {
        compile();

        for (auto pass : _renderPasses) {
            pass->render();
        }
    }
}