// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// Renderer: forward layer rendering. renderForwardLayer runs the stages below in order;
// each stage is a function of its own so the frame can be read top to bottom. The lights
// it gathers are in rendererForwardLights.cpp.
#include "rendererInternal.h"

namespace visutwin::canvas
{
    using namespace renderer_internal;

    namespace
    {
        struct ForwardDrawEntry
        {
            MeshInstance* meshInstance = nullptr;
            Material* material = nullptr;
            // Raw pointers — buffers are kept alive by the Mesh objects (which are kept alive
            // by MeshInstances in the same draw entry). Using raw pointers avoids atomic
            // refcount increments/decrements per draw entry (~200-2000 ops/frame savings).
            std::shared_ptr<VertexBuffer> vertexBuffer;
            std::shared_ptr<IndexBuffer> indexBuffer;
            Primitive primitive;
            uint64_t sortKey = 0;
            // Signed depth along the camera forward vector (see sortDistance.h), NOT a
            // radial distance: transparent draws sort back-to-front on this.
            float sortDistance = 0.0f;
        };

        /// Thin wrapper: the layout itself lives in sortKey.h so a test can hold it.
        uint64_t makeOpaqueSortKey(const MeshInstance* meshInstance)
        {
            const auto* material = meshInstance ? meshInstance->material() : nullptr;
            return makeForwardSortKey(
                meshInstance ? meshInstance->drawBucket() : MeshInstance::kDefaultDrawBucket,
                material && material->alphaMode() == AlphaMode::MASK,
                // No material means the default one, and they all sort together.
                material ? material->id() : 0x7FFFFFu,
                // The mesh's id, never its address: see sortKey.h.
                meshInstance && meshInstance->mesh() ? meshInstance->mesh()->id() : 0u);
        }
    }

    namespace renderer_internal
    {
        // A sky drawn as a finite dome sits where its node puts it; an infinite or an
        // atmosphere sky follows the camera.
        bool skyIsDome(const Scene* scene)
        {
            return scene && scene->sky() && scene->sky()->type() != SKYTYPE_INFINITE &&
                scene->sky()->type() != SKYTYPE_ATMOSPHERE;
        }
    }

    namespace
    {
        Matrix4 nodeWorldTransform(const MeshInstance* meshInstance)
        {
            return meshInstance && meshInstance->node()
                ? meshInstance->node()->worldTransform() : Matrix4::identity();
        }

        Matrix4 forwardModelMatrix(const MeshInstance* meshInstance, const Material* material,
            const Scene* scene, const Vector3& cameraPosition)
        {
            if (material && material->isSkybox() && !skyIsDome(scene)) {
                return Matrix4::translation(cameraPosition);
            }
            return nodeWorldTransform(meshInstance);
        }

        /// Restores the device's viewport and scissor when the scope ends, so a
        /// camera's rect never outlives its camera-layer pass.
        class ViewportScissorScope
        {
        public:
            explicit ViewportScissorScope(GraphicsDevice& device)
                : _device(device),
                  _vx(device.vx()), _vy(device.vy()), _vw(device.vw()), _vh(device.vh()),
                  _sx(device.sx()), _sy(device.sy()), _sw(device.sw()), _sh(device.sh()) {}

            ~ViewportScissorScope()
            {
                _device.setViewport(_vx, _vy, _vw, _vh);
                _device.setScissor(_sx, _sy, _sw, _sh);
            }

            ViewportScissorScope(const ViewportScissorScope&) = delete;
            ViewportScissorScope& operator=(const ViewportScissorScope&) = delete;

        private:
            GraphicsDevice& _device;
            float _vx, _vy, _vw, _vh;
            int _sx, _sy, _sw, _sh;
        };

        ForwardView setupForwardView(GraphicsDevice& device, const Scene* scene, Camera& camera,
            RenderTarget* renderTarget)
        {
            ForwardView view;
            view.camera = &camera;
            view.cameraNode = camera.node();
            if (view.cameraNode) {
                view.position = view.cameraNode->position();
                view.forward = cameraForwardOf(view.cameraNode->worldTransform());
                view.viewMatrix = view.cameraNode->worldTransform().inverse();
            }
            view.activeTarget = renderTarget ? renderTarget : camera.renderTarget().get();
            const int targetWidth = std::max(view.activeTarget ? view.activeTarget->width() : device.size().first, 1);
            const int targetHeight = std::max(view.activeTarget ? view.activeTarget->height() : device.size().second, 1);
            view.viewport = normalizedRectToPixels(camera.rect(), targetWidth, targetHeight);
            view.scissor = normalizedRectToPixels(camera.scissorRect(), targetWidth, targetHeight);

            // ASPECT_AUTO cameras use viewport size, not full target size.
            if (camera.aspectRatioMode() == AspectRatioMode::ASPECT_AUTO && !camera.multiView()) {
                camera.setAspectRatio(static_cast<float>(view.viewport.w) / static_cast<float>(view.viewport.h));
            }

            for (const auto* candidate : instancesOf<CameraComponent>(scene ? scene->componentRegistry() : nullptr)) {
                if (candidate && candidate->camera() == &camera) {
                    view.cameraComponent = candidate;
                    break;
                }
            }

            // A multi-view camera draws the view the pass names: the view's own matrices and
            // viewport, no jitter (nothing reprojects a view's history).
            if (camera.multiView() && !camera.views().empty()) {
                const auto& views = camera.views();
                const auto index = static_cast<size_t>(std::clamp(device.currentViewIndex(), 0,
                    static_cast<int>(views.size()) - 1));
                const CameraView& cameraView = views[index];
                view.viewMatrix = cameraView.view;
                view.projMatrix = cameraView.projection;
                view.position = cameraView.viewInverse.getTranslation();
                view.forward = cameraForwardOf(cameraView.viewInverse);
                // The view's target, whatever target the camera or its render action names.
                if (cameraView.renderTarget) {
                    view.activeTarget = cameraView.renderTarget.get();
                }
                const int viewTargetWidth = std::max(view.activeTarget ? view.activeTarget->width() : device.size().first, 1);
                const int viewTargetHeight = std::max(view.activeTarget ? view.activeTarget->height() : device.size().second, 1);
                PixelRect rect;
                rect.x = std::clamp(static_cast<int>(cameraView.viewport.getX()), 0, viewTargetWidth - 1);
                rect.y = std::clamp(static_cast<int>(cameraView.viewport.getY()), 0, viewTargetHeight - 1);
                rect.w = std::clamp(static_cast<int>(cameraView.viewport.getZ()), 1, viewTargetWidth - rect.x);
                rect.h = std::clamp(static_cast<int>(cameraView.viewport.getW()), 1, viewTargetHeight - rect.y);
                view.viewport = rect;
                view.scissor = rect;
                view.viewProjection = view.projMatrix * view.viewMatrix;
                view.background = cameraView.background;
                // A view without the background draws the sky with a matrix that collapses it:
                // every vertex at the clip origin, so nothing is rasterised. The sky sits AT
                // the far plane, which a depth slice's own projection would not clip.
                static constexpr float kZero[16] = {};
                view.viewProjectionSkybox = view.background ? view.viewProjection : Matrix4::load(kZero);
                camera.storeShaderMatrices(view.viewProjection, 0.0f, 0.0f, device.renderVersion());
                const int sceneTone = scene ? scene->toneMapping() : TONEMAP_LINEAR;
                view.toneMapping = camera.toneMapping() != TONEMAP_INHERIT ? camera.toneMapping() : sceneTone;
                return view;
            }

            view.projMatrix = camera.projectionMatrix();
            // The depth prepass takes the same offset from Camera::jitterOffset.
            const auto [jitterX, jitterY] = camera.jitterOffset(device.renderVersion(), view.viewport.w, view.viewport.h);
            if (camera.jitter() > 0.0f) {
                // Accumulate, do not assign: these are the same two elements that carry an
                // off-center projection offset (Camera::setProjectionOffset), so overwriting them
                // here would silently cancel the shift lens whenever TAA is enabled.
                view.projMatrix.setElement(2, 0, view.projMatrix.getElement(2, 0) + jitterX);
                view.projMatrix.setElement(2, 1, view.projMatrix.getElement(2, 1) + jitterY);
            }
            view.viewProjection = view.projMatrix * view.viewMatrix;
            camera.storeShaderMatrices(view.viewProjection, jitterX, jitterY, device.renderVersion());
            if (camera.projection() == ProjectionType::Perspective) {
                view.viewProjectionSkybox = view.viewProjection;
            } else {
                Matrix4 projSkybox = camera.projectionMatrixSkybox();
                if (camera.jitter() > 0.0f) {
                    projSkybox.setElement(2, 0, projSkybox.getElement(2, 0) + jitterX);
                    projSkybox.setElement(2, 1, projSkybox.getElement(2, 1) + jitterY);
                }
                view.viewProjectionSkybox = projSkybox * view.viewMatrix;
            }

            // Per-camera tone mapping overrides the
            // scene-wide value; TONEMAP_INHERIT keeps the scene's.
            const int sceneToneMapping = scene ? scene->toneMapping() : TONEMAP_LINEAR;
            view.toneMapping = camera.toneMapping() != TONEMAP_INHERIT ? camera.toneMapping() : sceneToneMapping;
            return view;
        }

        /// Which light-dependent shader features any active light needs this frame.
        struct LightFeatureUse
        {
            bool localShadows = false;
            bool omniShadows = false;
            bool cookie2D = false;
            bool cookieCube = false;
            bool vsmShadows = false;
            bool pcssShadows = false;
            bool pcf1Shadows = false;
            bool pcf5Shadows = false;
            bool areaLights = false;
        };

        LightFeatureUse scanLightFeatureUse(ComponentRegistry* registry, const bool clustered,
            const bool clusteredAreaLights)
        {
            LightFeatureUse use;
            for (const auto* lc : instancesOf<LightComponent>(registry)) {
                if (!lc || !lc->active()) {
                    continue;
                }
                const LightType type = lc->type();
                Light* sceneLight = lc->light();

                // Local shadows count only when the map is actually allocated, so the
                // shader never compiles with depth2d / depthcube parameters that would
                // be nil at runtime.
                if (lc->castShadows() && type != LightType::LIGHTTYPE_DIRECTIONAL &&
                    sceneLight && sceneLight->shadowMap()) {
                    if (type == LightType::LIGHTTYPE_OMNI) {
                        use.omniShadows = true;
                    } else {
                        use.localShadows = true;
                    }
                }

                // The cookie's shape has to match the light: a spot projects a 2D
                // texture, an omni samples a cubemap by direction. A mismatch is
                // ignored.
                if (lc->cookie()) {
                    if (type == LightType::LIGHTTYPE_SPOT && !lc->cookie()->isCubemap()) {
                        use.cookie2D = true;
                    } else if ((type == LightType::LIGHTTYPE_OMNI || type == LightType::LIGHTTYPE_POINT) &&
                               lc->cookie()->isCubemap()) {
                        use.cookieCube = true;
                    }
                }

                // Directional EVSM_16F and PCSS: both the shadow program and the
                // forward program need the matching variant.
                if (lc->castShadows() && type == LightType::LIGHTTYPE_DIRECTIONAL && sceneLight) {
                    use.vsmShadows |= sceneLight->shadowType() == SHADOW_VSM_16F;
                    use.pcssShadows |= sceneLight->shadowType() == SHADOW_PCSS_32F;
                    use.pcf1Shadows |= sceneLight->shadowType() == SHADOW_PCF1_32F;
                    use.pcf5Shadows |= sceneLight->shadowType() == SHADOW_PCF5_32F;
                }

                // A shaped light needs the LTC path: always outside clustered lighting
                // (a main-array light) and for a directional light, and under clustered
                // lighting only while the scene enables clustered area lights.
                if (lc->shape() != LightShape::LIGHTSHAPE_PUNCTUAL) {
                    use.areaLights |= !clustered || clusteredAreaLights || type == LightType::LIGHTTYPE_DIRECTIONAL;
                }
            }
            return use;
        }

        void collectDrawEntries(const std::vector<MeshInstance*>& bucket, const ForwardView& view,
            Material* defaultMaterial, const int renderVersion, ObjectPool<ForwardDrawEntry>& pool,
            std::vector<ForwardDrawEntry*>& out)
        {
            for (auto* meshInstance : bucket) {
                auto* mesh = meshInstance ? meshInstance->mesh() : nullptr;
                if (!mesh) {
                    continue;
                }
                auto vertexBuffer = mesh->getVertexBuffer();
                if (!vertexBuffer) {
                    continue;
                }

                auto* entry = pool.allocate();
                entry->meshInstance = meshInstance;
                entry->material = meshInstance->material() ? meshInstance->material() : defaultMaterial;
                if (!entry->material) {
                    continue;
                }
                entry->vertexBuffer = vertexBuffer;
                const size_t style = mesh->drawStyle(static_cast<size_t>(meshInstance->renderStyle()));
                entry->indexBuffer = mesh->getIndexBuffer(style);
                entry->primitive = mesh->getPrimitive(style);
                entry->sortKey = makeOpaqueSortKey(meshInstance);

                const auto worldBounds = meshInstance->aabb();
                if (meshInstance->node() && !entry->material->isSkybox()) {
                    // Signed view-axis depth. A
                    // radial distance would rank an off-axis transparent surface behind a
                    // centred one at the same depth.
                    const auto& customDistance = meshInstance->calculateSortDistance();
                    entry->sortDistance = customDistance
                        ? customDistance(*meshInstance, view.position, view.forward)
                        : forwardSortDistance(worldBounds.center(), view.position, view.forward);
                } else {
                    entry->sortDistance = 0.0f;
                }
                // Claimed last, by an entry that will draw: a drawOncePerFrame instance
                // skipped above for a missing material must not use up its frame.
                if (!meshInstance->claimDrawThisFrame(renderVersion)) {
                    continue;
                }
                out.push_back(entry);
            }
        }

        // The sort mode is a per-layer, per-sublayer
        // property, because the two sublayers want opposite things — the opaque pass
        // wants the fewest state changes, the transparent pass has to composite
        // back-to-front whatever that costs. The defaults are MATERIALMESH and
        // BACK2FRONT respectively.
        void sortDrawEntries(std::vector<ForwardDrawEntry*>& entries, const Layer& layer, const bool transparent)
        {
            const SortMode sortMode = transparent ? layer.transparentSortMode() : layer.opaqueSortMode();

            // A custom comparator may read MeshInstance::sortDistance, so publish what
            // was computed per draw before calling one.
            if (sortMode == SortMode::SORTMODE_CUSTOM) {
                for (auto* entry : entries) {
                    if (entry->meshInstance) {
                        entry->meshInstance->setSortDistance(entry->sortDistance);
                    }
                }
            }

            switch (sortMode) {
            case SortMode::SORTMODE_NONE:
                // Collection order. Nothing to do, and deliberately not a fallthrough to
                // a default: a pass that asked for no sorting must not get one.
                break;

            case SortMode::SORTMODE_MANUAL:
                std::stable_sort(entries.begin(), entries.end(),
                    [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                        const double orderA = a->meshInstance ? a->meshInstance->drawOrder() : 0.0;
                        const double orderB = b->meshInstance ? b->meshInstance->drawOrder() : 0.0;
                        return orderA < orderB;
                    });
                break;

            // Bucket first, then depth: see distanceSortsBefore in sortKey.h.
            case SortMode::SORTMODE_BACK2FRONT:
                std::stable_sort(entries.begin(), entries.end(),
                    [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                        return distanceSortsBefore(a->sortKey, a->sortDistance, b->sortKey, b->sortDistance, true);
                    });
                break;

            case SortMode::SORTMODE_FRONT2BACK:
                std::stable_sort(entries.begin(), entries.end(),
                    [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                        return distanceSortsBefore(a->sortKey, a->sortDistance, b->sortKey, b->sortDistance, false);
                    });
                break;

            case SortMode::SORTMODE_CUSTOM:
                // A null callback leaves the order alone rather than silently falling
                // back to a mode the caller did not ask for.
                if (const auto& callback = layer.customSortCallback()) {
                    std::stable_sort(entries.begin(), entries.end(),
                        [&callback](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                            return callback(a->meshInstance, b->meshInstance);
                        });
                }
                break;

            case SortMode::SORTMODE_MATERIALMESH:
            default:
                std::stable_sort(entries.begin(), entries.end(),
                    [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                        if (a->sortKey != b->sortKey) {
                            return a->sortKey < b->sortKey;
                        }
                        return a->sortDistance < b->sortDistance;
                    });
                break;
            }
        }

        /// Everything the draw loop reads that is the same for every draw of the layer.
        struct ForwardDrawContext
        {
            const std::shared_ptr<GraphicsDevice>& device;
            const Scene* scene;
            ProgramLibrary& programLibrary;
            const ForwardView& view;
            const ForwardLights& lights;
            const Layer& layer;
            Material* defaultMaterial;
            bool transparent;
            bool clusteredEnabled;
            // The visibility range's noise offset: the camera's dither jitter (Renderer::ditherJitterFor).
            float lodJitter[2] = {0.0f, 0.0f};
        };

        /// The inputs ProgramLibrary::bindMaterial resolves a variant from. The
        /// resolution is pure in them, and the states it sets are sticky on the encoder
        /// for the duration of the pass, so consecutive draws that agree on all of them
        /// — which dominate after sort-key ordering — skip it.
        struct ShaderVariantInputs
        {
            const Material* material = nullptr;
            bool dynamicBatch = false;
            bool skinned = false;
            bool morphed = false;
            bool instanced = false;
            bool instanceColor = false;
            bool instanceLightMap = false;
            bool screenSpace = false;
            uint32_t clipPlanes = CLIP_PLANES_NONE;

            bool operator==(const ShaderVariantInputs&) const = default;
        };

        /// What a draw contributes to its shader variant, read off its mesh instance;
        /// `cameraClipped`: the camera the draw is for has clipping planes; `lod`: the
        /// instance's visibility range at that camera (MeshInstance::visibilityFrom).
        ShaderVariantInputs shaderVariantInputs(const MeshInstance* meshInstance, const Material* boundMaterial,
            const bool cameraClipped, const VisibilityState& lod = {})
        {
            ShaderVariantInputs variant;
            variant.material = boundMaterial;
            // The camera's planes clip through the mesh instance's copy of the material
            // block, which a draw without its own material does not get.
            if (cameraClipped && meshInstance && meshInstance->material() && !meshInstance->material()->isSkybox()) {
                variant.clipPlanes |= CLIP_PLANES_CAMERA;
            }
            if (!meshInstance) {
                return variant;
            }
            // Hardware instancing is a property of the draw, not the material: a mesh instance
            // that carries a per-instance buffer gets the instanced vertex stage, and the buffer's
            // stride decides whether that stage also reads a per-instance base color.
            const auto& drawInstancing = meshInstance->instancingData();
            // A GPU-culled draw binds a compacted copy in the same layout as this buffer.
            const auto& instanceBuffer = drawInstancing.compactedVertexBuffer
                ? drawInstancing.compactedVertexBuffer : drawInstancing.vertexBuffer;
            variant.dynamicBatch = meshInstance->isDynamicBatch();
            variant.skinned = meshInstance->skinInstance() != nullptr;
            variant.morphed = meshInstance->morphInstance() != nullptr;
            variant.instanced = instanceBuffer != nullptr && drawInstancing.count > 0;
            variant.instanceColor = variant.instanced && instanceBuffer->format() &&
                instanceBuffer->format()->hasInstanceColor();
            // A lightmap the mesh instance owns (a lightmapper's bake) selects the
            // lightmap variant whatever the material says.
            variant.instanceLightMap = meshInstance->lightMap() != nullptr;
            variant.screenSpace = meshInstance->screenSpace();
            if (meshInstance->clipped() && meshInstance->material()) {
                variant.clipPlanes |= CLIP_PLANES_MESH;
            }
            // Fades through the same copy of the material block as the planes.
            if (meshInstance->material() && !meshInstance->material()->isSkybox()) {
                if (lod.perInstance && variant.instanced) {
                    variant.clipPlanes |= CLIP_LOD_FADE_INSTANCED;
                } else if (!lod.perInstance && visibilityFadePartial(lod.fade)) {
                    variant.clipPlanes |= CLIP_LOD_FADE;
                }
            }
            return variant;
        }

        /// Resolves the forward shader of every draw whose material the library has not
        /// resolved as it is now — a new material, an edited one, or one last drawn under
        /// other frame switches — BEFORE anything draws with them. A backend that
        /// compiles a shader when it is created (Metal: MetalShader) then compiles the
        /// new variants side by side instead of the draw loop stopping at each for a
        /// compile of its own. With nothing new this is one pass over the draws that
        /// reads what the loop is about to read anyway.
        ///
        /// `meshInstanceOf` and `materialOf` read a draw; the frame switches must
        /// already be set on the library (configureForwardShaderFeatures).
        template <typename Draws, typename MeshInstanceOf, typename MaterialOf>
        void resolveNewForwardShaders(ProgramLibrary& programLibrary, const Material* defaultMaterial,
            const bool transparent, const bool cameraClipped, const Draws& draws, MeshInstanceOf&& meshInstanceOf,
            MaterialOf&& materialOf)
        {
            const uint64_t frameBits = programLibrary.forwardFrameBits();
            const Material* lastMaterial = nullptr;
            for (const auto& draw : draws) {
                const Material* drawMaterial = materialOf(draw);
                const Material* material = drawMaterial ? drawMaterial : defaultMaterial;
                if (material == lastMaterial) {
                    continue;
                }
                lastMaterial = material;
                // A shader override is used as it is; a resolved material needs nothing.
                if (!material || material->shaderOverride() ||
                    programLibrary.forwardShaderResolved(material, frameBits)) {
                    continue;
                }
                const ShaderVariantInputs variant = shaderVariantInputs(meshInstanceOf(draw), material, cameraClipped);
                (void)programLibrary.getForwardShader(material, transparent, variant.dynamicBatch,
                    variant.skinned, variant.morphed, variant.instanced, variant.instanceColor,
                    variant.instanceLightMap, variant.screenSpace, variant.clipPlanes);
            }
        }

        void submitGSplatDraw(const ForwardDrawContext& ctx, const ForwardDrawEntry& entry, GSplatInstance& gsplat)
        {
            GraphicsDevice& device = *ctx.device;
            const ForwardView& view = ctx.view;
            const Matrix4 modelMatrix = nodeWorldTransform(entry.meshInstance);
            gsplat.update(view.position, view.forward, modelMatrix, view.viewMatrix, view.projMatrix,
                static_cast<float>(view.viewport.w), static_cast<float>(view.viewport.h));
            if (gsplat.visibleCount() == 0) {
                return;
            }
            device.frameCounters().gsplats += static_cast<int>(gsplat.visibleCount());
#ifndef NDEBUG
            // Splats blend into EVERY sample of a multisampled target, which makes
            // them several times more expensive (Debug only).
            if (view.activeTarget && view.activeTarget->samples() > 1) {
                static std::set<std::string> warnedLayers;
                if (warnedLayers.insert(ctx.layer.name()).second) {
                    spdlog::warn("Gaussian splats on layer '{}' are rendered into a multisampled "
                        "target ({} samples), which makes them several times more expensive to "
                        "render. Render them into a single-sampled target: set the camera's "
                        "RenderingSettings::samples to 1.", ctx.layer.name(), view.activeTarget->samples());
                }
            }
#endif
            // The output stage: a splat carries a GAMMA-space colour, so it owes
            // the target the fog, exposure, tone mapping and encode the forward
            // tail applies to lit colour. Decoding to linear
            // and stopping is right only under a camera frame: on a gamma target the
            // splats would be written linear, untonemapped and unfogged beside
            // tonemapped meshes.
            // A dynamic sky's haze does not reach the splats: they render without fog under it.
            const FogParams fogParams = ctx.scene ? ctx.scene->fogFor(view.camera) : FogParams{};
            const bool splatFog = fogParams.enabled && fogParams.type != FogType::FOG_DYNAMIC_SKY_HAZE;
            GpuGSplatParams splatParams = gsplat.gpuParams();
            splatParams.cameraOrtho = view.camera->projection() == ProjectionType::Orthographic ? 1u : 0u;
            Color fogLinear;
            fogLinear.linear(&fogParams.color);
            splatParams.fogColor[0] = fogLinear.r;
            splatParams.fogColor[1] = fogLinear.g;
            splatParams.fogColor[2] = fogLinear.b;
            splatParams.fogParams[0] = fogParams.start;
            splatParams.fogParams[1] = fogParams.end;
            splatParams.fogParams[2] = fogParams.density;
            splatParams.fogParams[3] = splatFog ? static_cast<float>(fogParams.type) : 0.0f;
            splatParams.output[0] = ctx.scene ? ctx.scene->exposureFor(view.camera) : 1.0f;
            splatParams.output[1] = static_cast<float>(view.toneMapping);
            splatParams.output[2] = device.hdrPass() ? 1.0f : 0.0f;
            for (const auto* buffer : {&gsplat.resource()->splatBuffer(), &gsplat.orderBuffer(),
                                       &gsplat.resource()->shBuffer()}) {
                if (*buffer) {
                    (*buffer)->markStorageUse();   // counts as vram.sb
                }
            }
            device.setGSplatState(gsplat.resource()->splatBuffer(), gsplat.orderBuffer(),
                gsplat.resource()->shBuffer(), &splatParams, sizeof(GpuGSplatParams));
            device.setTransformUniforms(view.viewProjection, modelMatrix);
            device.draw(entry.primitive, entry.indexBuffer,
                static_cast<int>(gsplat.visibleCount()), -1, true, true);
        }

        /// Binds the draw's geometry-kind state and issues the draw: GPU-culled or
        /// direct instancing, a dynamic batch, a storage draw, particles, splats, or
        /// an ordinary (possibly skinned or morphed) mesh.
        void submitForwardDraw(const ForwardDrawContext& ctx, const ForwardDrawEntry& entry,
            const ShaderVariantInputs& variant, const MeshInstance::InstancingData& instData)
        {
            GraphicsDevice& device = *ctx.device;
            FrameCounters& counters = device.frameCounters();
            const ForwardView& view = ctx.view;
            MeshInstance* meshInstance = entry.meshInstance;

            const MeshInstance::GpuCullOutput* culled = meshInstance && meshInstance->gpuCullingEnabled()
                ? meshInstance->culledOutput(view.camera, device.renderVersion()) : nullptr;
            // Arguments a kernel wrote this frame for this camera (MeshInstance::setIndirect).
            const int indirectSlot = meshInstance && !culled && !variant.dynamicBatch &&
                    !meshInstance->skinInstance() && !meshInstance->morphInstance()
                ? meshInstance->indirectSlotFor(view.camera, device.indirectFrame()) : -1;
            if (indirectSlot >= 0 && device.indirectDrawBuffer()) {
                const bool instanced = instData.vertexBuffer && instData.count > 0;
                if (instanced) {
                    device.setVertexBuffer(instData.vertexBuffer, 5);
                }
                device.setIndirectDrawBuffer(device.indirectDrawBuffer()->nativeBuffer());
                const Matrix4 model = instanced
                    ? nodeWorldTransform(meshInstance)
                    : forwardModelMatrix(meshInstance, variant.material, ctx.scene, view.position);
                device.setTransformUniforms(view.viewProjection, model, meshInstance->ditherId(model));
                device.draw(entry.primitive, entry.indexBuffer, 0, indirectSlot, true, true);
            } else if (culled) {
                // GPU-culled for THIS camera: the compacted visible set at slot 5, the
                // instance count from the indirect arguments the cull wrote.
                device.setVertexBuffer(culled->compacted, 5);
                device.setIndirectDrawBuffer(culled->culler->indirectArgsNativeBuffer());
                // Each instance is placed in its NODE's space (model * instance), so the
                // node's world matrix goes up as the model matrix.
                device.setTransformUniforms(view.viewProjection, nodeWorldTransform(meshInstance));
                device.draw(entry.primitive, entry.indexBuffer, 0, 0, true, true);
            } else if (instData.indirectArgsBuffer && instData.indirectSlot >= 0 && instData.compactedVertexBuffer) {
                // GPU-culled indirect instancing:
                // Bind the compacted buffer (visible instances only) at slot 5.
                // Instance count comes from the GPU via indirect draw arguments.
                device.setVertexBuffer(instData.compactedVertexBuffer, 5);
                device.setIndirectDrawBuffer(instData.indirectArgsBuffer);
                // Each instance is placed in its NODE's space (model * instance),
                // so the node's world matrix goes up as the model matrix and the shaders compose the two.
                device.setTransformUniforms(view.viewProjection, nodeWorldTransform(meshInstance));
                device.draw(entry.primitive, entry.indexBuffer, 0, instData.indirectSlot, true, true);
            } else if (instData.vertexBuffer && instData.count > 0) {
                // Direct instancing: all instances drawn, CPU-provided count. The
                // node's world matrix: instances live in its space (see above).
                device.setVertexBuffer(instData.vertexBuffer, 5);
                const Matrix4 model = forwardModelMatrix(meshInstance, variant.material, ctx.scene, view.position);
                device.setTransformUniforms(view.viewProjection, model, meshInstance->ditherId(model));
                drawWithCommands(device, entry.primitive, entry.indexBuffer, instData.count,
                    meshInstance->drawCommandsFor(view.camera));
            } else if (variant.dynamicBatch) {
                // Dynamic batch draw: dynamic batches use SkinBatchInstance with a
                // per-frame matrix palette, which the vertex shader indexes with a
                // per-vertex bone index; the model matrix is identity.
                if (auto* sbi = meshInstance->skinBatchInstance()) {
                    device.setDynamicBatchPalette(sbi->paletteData(), sbi->paletteSizeBytes(),
                        sbi->paletteVersion());
                }
                // The batch's dither offset, which each member mixes with its palette index.
                device.setTransformUniforms(view.viewProjection, Matrix4::identity(),
                    meshInstance->ditherId(Matrix4::identity()));
                device.draw(entry.primitive, entry.indexBuffer, 1, -1, true, true);
            } else if (meshInstance && meshInstance->storageDrawCount() > 0) {
                // App-driven storage draw: a custom shader expands one instance per
                // record in an app-owned buffer (typically filled by a compute shader).
                // Same binding slots as the emitter/splat paths, no engine-side simulation.
                const auto& storageParams = meshInstance->storageParams();
                if (meshInstance->storageBuffer()) {
                    meshInstance->storageBuffer()->markStorageUse();   // counts as vram.sb
                }
                device.setStorageDrawState(meshInstance->storageBuffer(),
                    storageParams.data(), storageParams.size());
                device.setTransformUniforms(view.viewProjection, nodeWorldTransform(meshInstance));
                device.draw(entry.primitive, entry.indexBuffer, meshInstance->storageDrawCount(), -1, true, true);
            } else if (auto* particles = meshInstance ? meshInstance->particleEmitter() : nullptr) {
                // GPU particles: one instanced billboard quad per particle over the
                // compute-simulated pool (dead particles emit clipped vertices).
                const Matrix4 modelMatrix = nodeWorldTransform(meshInstance);
                particles->prepareRender(view.viewMatrix, view.projMatrix, modelMatrix,
                    static_cast<float>(view.viewport.w), static_cast<float>(view.viewport.h));
                // Tone map and gamma-encode on a gamma target, leave
                // both to compose on a camera frame's linear HDR scene.
                particles->setOutput(ctx.scene ? ctx.scene->exposureFor(view.camera) : 1.0f, view.toneMapping, device.hdrPass());
                {
                    // The camera's fog, as the splats take it: no dynamic sky haze, and none for
                    // a screen-space emitter, whose depth is not a distance.
                    const FogParams fog = ctx.scene ? ctx.scene->fogFor(view.camera) : FogParams{};
                    const bool fogged = fog.enabled && fog.type != FogType::FOG_DYNAMIC_SKY_HAZE &&
                        !particles->options().screenSpace;
                    Color fogLinear;
                    fogLinear.linear(&fog.color);
                    particles->setFog(fogLinear, fogged ? static_cast<int>(fog.type) : 0, fog.start, fog.end,
                        fog.density);
                }
                if (particles->options().lighting) {
                    // The light cube: the scene ambient in every direction, plus each
                    // directional light weighted by how far the direction faces along it —
                    // both as AUTHORED colours.
                    static constexpr float kCubeDirections[6][3] = {
                        {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
                    float cube[6][3];
                    const Color ambient = ctx.scene ? ctx.scene->ambientLight() : Color(0.0f, 0.0f, 0.0f, 1.0f);
                    for (auto& face : cube) {
                        face[0] = ambient.r;
                        face[1] = ambient.g;
                        face[2] = ambient.b;
                    }
                    for (const auto& entry : ctx.lights.directional) {
                        const GpuLightData& light = entry.light;
                        for (int c = 0; c < 6; ++c) {
                            const float weight = std::max(kCubeDirections[c][0] * light.direction.getX() +
                                kCubeDirections[c][1] * light.direction.getY() +
                                kCubeDirections[c][2] * light.direction.getZ(), 0.0f) * light.intensity;
                            cube[c][0] += light.color.r * weight;
                            cube[c][1] += light.color.g * weight;
                            cube[c][2] += light.color.b * weight;
                        }
                    }
                    particles->setLightCube(cube);
                }
                particles->setSoftening(view.camera ? view.camera->nearClip() : 0.1f,
                    view.camera ? view.camera->farClip() : 1000.0f, device.sceneDepthGrabMap() != nullptr);
                if (particles->particleBuffer()) {
                    particles->particleBuffer()->markStorageUse();   // counts as vram.sb
                }
                device.setParticleState(particles->particleBuffer(), particles->orderBuffer(),
                    particles->meshVertexBuffer(), &particles->renderParams(), sizeof(GpuParticleRenderParams));
                device.setTransformUniforms(view.viewProjection, modelMatrix);
                device.draw(entry.primitive, entry.indexBuffer,
                    static_cast<int>(particles->numParticles()), -1, true, true);
            } else if (auto* gsplat = meshInstance ? meshInstance->gsplatInstance() : nullptr) {
                // Gaussian splats: one instanced quad per splat, back-to-front via the
                // per-instance order buffer filled by the background depth sorter.
                submitGSplatDraw(ctx, entry, *gsplat);
            } else {
                // GPU skinning: refresh the bone palette (no-op if already updated this
                // frame) and bind it at slot 6. The palette is node-relative; the model
                // matrix stays the node's world transform (they cancel in the shader).
                if (variant.skinned) {
                    auto* si = meshInstance->skinInstance();
                    // The mesh's hierarchy and the skeleton's (which can sit elsewhere, or above the
                    // mesh) were drawn this frame: their anim components are visible.
                    if (meshInstance->node() && !device.precompilePass()) {
                        meshInstance->node()->markDeformedRendered(device.renderVersion());
                    }
                    if (si->rootBone() && !device.precompilePass()) {
                        si->rootBone()->markDeformedRendered(device.renderVersion());
                    }
                    {
                        const ScopedMilliseconds skinTimer(counters.skinTime);
                        si->updateMatrixPalette(meshInstance->node());
                    }
                    device.setDynamicBatchPalette(si->paletteData(), si->paletteSizeBytes(),
                        si->paletteVersion());
                    counters.skinDrawCalls++;
                }
                // Morph targets: bind the shared delta buffer + current weights.
                if (variant.morphed) {
                    auto* mi = meshInstance->morphInstance();
                    if (meshInstance->node() && !device.precompilePass()) {
                        meshInstance->node()->markDeformedRendered(device.renderVersion());
                    }
                    if (mi->morph() && mi->morph()->deltaBuffer()) {
                        const MorphInstance::GpuMorphParams* params = nullptr;
                        {
                            const ScopedMilliseconds morphTimer(counters.morphTime);
                            params = &mi->gpuParams();
                        }
                        device.setMorphState(mi->morph()->deltaBuffer(), params, sizeof(*params));
                    }
                }

                const bool sky = variant.material && variant.material->isSkybox();
                const Matrix4 model = forwardModelMatrix(meshInstance, variant.material, ctx.scene, view.position);
                // The mesh's dither offset, fixed at its first draw (MeshInstance::ditherId).
                device.setTransformUniforms(sky ? view.viewProjectionSkybox : view.viewProjection, model,
                    meshInstance ? meshInstance->ditherId(model) : 0.0f);
                const DrawCommands* commands = meshInstance && !variant.skinned && !variant.morphed
                    ? meshInstance->drawCommandsFor(view.camera) : nullptr;
                drawWithCommands(device, entry.primitive, entry.indexBuffer, 1, commands);
            }
        }

        void drawForwardEntries(const ForwardDrawContext& ctx, const std::vector<ForwardDrawEntry*>& entries)
        {
            GraphicsDevice& device = *ctx.device;
            FrameCounters& counters = device.frameCounters();
            const Scene* scene = ctx.scene;
            Camera& camera = *ctx.view.camera;

            const auto ambientColor = scene ? scene->ambientLight() : Color(0.0f, 0.0f, 0.0f, 1.0f);
            const auto fogParams = scene ? scene->fogFor(&camera) : FogParams{};
            const float exposure = scene ? scene->exposureFor(&camera) : 1.0f;
            const Vector3* ambientSH = (scene && scene->hasAmbientSH()) ? scene->ambientSH().data() : nullptr;
            ShadowParams noShadow;
            noShadow.enabled = false;

            // The main light array for the light mask of the draw. 95%+ of mesh
            // instances use MASK_AFFECT_DYNAMIC (the default), so it is built for that
            // mask up front and rebuilt only when a draw's mask differs.
            static thread_local std::vector<GpuLightData> maskedLights;
            maskedLights.reserve(kMaxMainLights);
            uint32_t maskedLightsMask = MASK_AFFECT_DYNAMIC;
            filterLightsForMask(ctx.lights, maskedLightsMask, ctx.clusteredEnabled, maskedLights);

            // New variants start compiling before the loop needs the first of them. The
            // frame graph did this for the whole frame before any pass ran
            // (prepareForwardShaders); this is the exact one, with the frame switches as
            // they are now, for whatever that could not foresee.
            const ClipPlanes& cameraClip = camera.clipping();
            resolveNewForwardShaders(ctx.programLibrary, ctx.defaultMaterial, ctx.transparent, !cameraClip.empty(),
                entries,
                [](const ForwardDrawEntry* entry) { return entry->meshInstance; },
                [](const ForwardDrawEntry* entry) { return entry->material; });

            ShaderVariantInputs boundVariant;

            // The lighting block depends on the DRAW only through its light mask and
            // receiveShadow; everything else in it is the layer's. Set it on the layer's
            // first draw and when either changes, not per draw: each set repacks ~2.8 KB
            // on Metal and makes Vulkan allocate and upload a fresh copy
            // (setLightingUniforms flags an upload).
            bool lightingSet = false;
            uint32_t lightingSetMask = 0;
            bool lightingSetReceivesShadow = true;

            // The material's base cull mode reads its parameter map, so it is cached
            // per material; the node-scale flip is applied per draw.
            const Material* lastCullMaterial = nullptr;
            CullMode cachedCullMode = CullMode::CULLFACE_BACK;

            for (const auto* entry : entries) {
                MeshInstance* meshInstance = entry->meshInstance;
                const Material* boundMaterial = entry->material ? entry->material : ctx.defaultMaterial;
                // A probe volume bake's geometry cannot be blended: blended materials are left out.
                if (camera.probeVolumeGeometryPass() && boundMaterial && boundMaterial->blendState() &&
                    boundMaterial->blendState()->enabled()) {
                    continue;
                }

                // A lightmap the mesh instance owns (a lightmapper's bake) overrides the
                // material's, so it both selects the lightmap variant and is bound by the
                // device over the material's lightmap slot.
                Texture* instanceLightMap = meshInstance ? meshInstance->lightMap().get() : nullptr;

                // The visibility range at this camera: the cull dropped what it hides, so only a
                // fade band is left to see here.
                MeshInstance::LodFadeDraw lod;
                if (meshInstance && ctx.view.cameraNode) {
                    lod.state = meshInstance->visibilityFrom(ctx.view.position);
                    lod.viewer = ctx.view.position;
                    lod.jitter[0] = ctx.lodJitter[0];
                    lod.jitter[1] = ctx.lodJitter[1];
                }
                const ShaderVariantInputs variant = shaderVariantInputs(meshInstance, boundMaterial,
                    !cameraClip.empty(), lod.state);

                device.setInstanceLightMap(instanceLightMap);
                // The instance's parameter overrides, applied over the bound material's block,
                // and the clipping planes of the instance and of the camera and the fade
                // written in.
                const MaterialUniforms* instanceUniforms = meshInstance && entry->material
                    ? meshInstance->overriddenUniforms(boundMaterial,
                        (variant.clipPlanes & CLIP_PLANES_CAMERA) ? &cameraClip : nullptr,
                        (variant.clipPlanes & (CLIP_LOD_FADE | CLIP_LOD_FADE_INSTANCED)) ? &lod : nullptr)
                    : nullptr;
                device.setInstanceMaterialUniforms(instanceUniforms,
                    instanceUniforms ? meshInstance->overriddenUniformsVersion() : 0);
                // Per-draw stencil (UI masks); null leaves the stencil off.
                if (meshInstance) {
                    device.setStencilState(meshInstance->stencilFront(), meshInstance->stencilBack());
                } else {
                    device.setStencilState();
                }

                if (variant != boundVariant) {
                    ctx.programLibrary.bindMaterial(ctx.device, boundMaterial, ctx.transparent,
                        variant.dynamicBatch, variant.skinned, variant.morphed, variant.instanced,
                        variant.instanceColor, variant.instanceLightMap, variant.screenSpace, variant.clipPlanes);
                    boundVariant = variant;
                }

                const uint32_t drawLightMask = meshInstance ? meshInstance->mask() : MASK_AFFECT_DYNAMIC;
                if (drawLightMask != maskedLightsMask) {
                    filterLightsForMask(ctx.lights, drawLightMask, ctx.clusteredEnabled, maskedLights);
                    maskedLightsMask = drawLightMask;
                }

                // A mesh instance with receiveShadow=false gets no shadow params (SHADERDEF_NOSHADOW).
                const bool drawReceivesShadow = !meshInstance || meshInstance->receiveShadows();
                if (!lightingSet || drawLightMask != lightingSetMask ||
                    drawReceivesShadow != lightingSetReceivesShadow) {
                    // The draw's mask also picks which clustered lights it accepts (dynamic
                    // or lightmapped), which is why the block follows the mask.
                    device.setLightingUniforms(ambientColor, maskedLights, ctx.view.position, true, exposure,
                        fogParams, drawReceivesShadow ? ctx.lights.shadowParams : noShadow,
                        ctx.view.toneMapping, ambientSH, &ctx.view.viewProjection, drawLightMask);
                    lightingSet = true;
                    lightingSetMask = drawLightMask;
                    lightingSetReceivesShadow = drawReceivesShadow;
                }

                if (boundMaterial != lastCullMaterial) {
                    counters.materialSwitches++;   // stats.frame.materials
                    cachedCullMode = resolveMaterialCullMode(boundMaterial);
                    lastCullMaterial = boundMaterial;
                }
                auto cullMode = applyNodeScaleFlip(cachedCullMode, meshInstance ? meshInstance->node() : nullptr);
                if (ctx.programLibrary.oitAccumulate()) {
                    // An order-independent transparency accumulation: every draw adds into the
                    // pass's two attachments, whatever its material blends with, and tests
                    // itself against the scene depth in its shader (the target has no depth).
                    device.setBlendState(RenderPassOit::accumulationBlendState());
                    device.setDepthState(DepthState::noDepth());
                }
                if (camera.lightmapBakeAccumulate()) {
                    // Ambient-occlusion virtual lights: each pass adds its own contribution
                    // to the lightmap already in the target, so the draw blends additively
                    // and the camera does not clear between passes.
                    static const auto additive = std::make_shared<BlendState>(BlendState::additiveBlend());
                    device.setBlendState(additive);
                }
                if (!camera.cullFaces()) {
                    cullMode = CullMode::CULLFACE_NONE;
                }
                if (camera.lightmapBakePass()) {
                    // UV-space bake: triangle winding follows the unwrap, not the surface —
                    // mirrored charts (and the v-flip that puts UV origin at the top) make
                    // triangles face either way, so face culling would drop them; the
                    // bake renders double-sided.
                    cullMode = CullMode::CULLFACE_NONE;
                }
                device.setCullMode(cullMode);

                device.setVertexBuffer(entry->vertexBuffer, 0);
                submitForwardDraw(ctx, *entry, variant,
                    meshInstance ? meshInstance->instancingData() : MeshInstance::InstancingData{});
                counters.forwardDrawCalls++;
            }

            // No other pass may inherit the last draw's lightmap or uniform overrides.
            device.setInstanceLightMap(nullptr);
            device.setInstanceMaterialUniforms(nullptr, 0);
            device.setStencilState();
        }
    }

    void Renderer::configureForwardShaderFeatures(ProgramLibrary& programLibrary, const Camera& camera,
        const RenderTarget* renderTarget, const bool clusteredEnabled)
    {
        // One fragment output per colour attachment of the target (the back buffer has one).
        programLibrary.setColorOutputCount(renderTarget ? std::max(renderTarget->colorBufferCount(), 1) : 1);

        // tell ProgramLibrary whether a skybox cubemap is available
        // so skybox shaders compile with VT_FEATURE_SKY_CUBEMAP.
        programLibrary.setSkyCubemapAvailable(_scene && _scene->skybox() != nullptr);

        // when camera is in depth pass mode, compile shaders
        // with VT_FEATURE_PLANAR_REFLECTION_DEPTH_PASS to override fragment output
        // with distance-from-reflection-plane (setShaderPass).
        programLibrary.setPlanarReflectionDepthPass(camera.planarReflectionDepthPass());
        programLibrary.setLightmapBakePass(camera.lightmapBakePass());
        programLibrary.setLightmapBakeAccumulate(camera.lightmapBakeAccumulate());
        programLibrary.setProbeGBufferPass(camera.probeVolumeGeometryPass());

        // Debug surface-quantity output (setDebugShaderPass). One variant covers every mode;
        // the mode itself rides in a uniform, uploaded with the environment.
        programLibrary.setDebugPassEnabled(camera.debugShaderPass() != DebugShaderPass::DEBUGPASS_NONE);

        // Light-dependent variants are enabled only when a light actually needs
        // them: an unbound shadow map or cookie parameter would be nil at draw time.
        const LightFeatureUse lightFeatures = scanLightFeatureUse(componentRegistry(),
            _scene && _scene->clusteredLightingEnabled(), _scene && _scene->lighting().areaLightsEnabled);
        programLibrary.setLocalShadowsEnabled(lightFeatures.localShadows);
        programLibrary.setOmniShadowsEnabled(lightFeatures.omniShadows);
        programLibrary.setCookie2DEnabled(lightFeatures.cookie2D);
        programLibrary.setCookieCubeEnabled(lightFeatures.cookieCube);
        programLibrary.setVsmShadowsEnabled(lightFeatures.vsmShadows);
        programLibrary.setPcssShadowsEnabled(lightFeatures.pcssShadows);
        programLibrary.setPcf1ShadowsEnabled(lightFeatures.pcf1Shadows);
        programLibrary.setPcf5ShadowsEnabled(lightFeatures.pcf5Shadows);
        programLibrary.setAreaLightsEnabled(lightFeatures.areaLights);
        programLibrary.setClusterAreaLightShapes(_scene ? _scene->lighting().areaLightShapes() : 0u);

        // LTC lookup tables: created lazily the first time an area light is
        // seen, then bound at fragment slots 20/21 every frame they're needed.
        if (lightFeatures.areaLights && !_areaLightLuts.lut1) {
            _areaLightLuts = AreaLightLuts::create(_device.get());
        }
        _device->setAreaLightLuts(
            lightFeatures.areaLights ? _areaLightLuts.lut1.get() : nullptr,
            lightFeatures.areaLights ? _areaLightLuts.lut2.get() : nullptr);

        // Clustered lighting: compile forward shaders with VT_FEATURE_LIGHT_CLUSTERING
        // so the fragment shader samples the cluster grid.
        programLibrary.setClusteredLightingEnabled(clusteredEnabled);

        // SSAO per-material: when the device has a forward SSAO texture, compile
        // forward shaders with VT_FEATURE_SSAO so the fragment shader modulates
        // ambient occlusion by sampling the SSAO texture at screen-space UV.
        programLibrary.setSsaoEnabled(_device->ssaoForwardTexture() != nullptr);
        programLibrary.setLightProbesEnabled(_scene && _scene->hasAmbientSH());
        programLibrary.setEnvAtlasEnabled(_scene && _scene->envAtlas() != nullptr);
        programLibrary.setEnvAtlasEncoding(
            sceneEnvEncodingOf(_scene ? _scene->envAtlas() : nullptr, _scene ? _scene->skybox() : nullptr));
        programLibrary.setSkyboxReflectionEnabled(_scene && !_scene->envAtlas() && _scene->skybox());
        const int toneMapping = camera.toneMapping() != TONEMAP_INHERIT ? camera.toneMapping()
            : (_scene ? _scene->toneMapping() : TONEMAP_LINEAR);
        programLibrary.setToneMapAgxEnabled(toneMapping == TONEMAP_AGX || toneMapping == TONEMAP_AGX_PUNCHY);
        const FogParams* cameraFog = camera.fog();
        programLibrary.setSkyHazeEnabled(cameraFog && cameraFog->enabled && cameraFog->type == FogType::FOG_DYNAMIC_SKY_HAZE);
        programLibrary.setReflectionProbeEnabled(_scene && _scene->reflectionProbe() != nullptr);

        // Atmosphere scattering: when enabled on the scene, compile skybox shaders
        // with VT_FEATURE_ATMOSPHERE and push atmosphere uniforms to the device.
        const bool atmosphereEnabled = _scene && _scene->atmosphereEnabled();
        programLibrary.setAtmosphereEnabled(atmosphereEnabled);
        _device->setAtmosphereEnabled(atmosphereEnabled);
        if (atmosphereEnabled) {
            _device->setAtmosphereUniforms(_scene->atmosphereUniformData(), _scene->atmosphereUniformSize());
        }
    }

    void Renderer::resolveClusterConfig()
    {
        // Resolve the grid shape the scene asked for, once. Every pooled grid is
        // built with it; the grids themselves are created on demand, one per distinct
        // light set (clustersForLightSet). The shadow atlas, by contrast, is
        // configured every frame where it updates (ForwardRenderer::buildFrameGraph),
        // since a scene may change shadowAtlasResolution at any time.
        if (_clusterConfigResolved || !_scene) {
            return;
        }
        _clusterConfigResolved = true;
        const auto& lightingParams = _scene->lighting();
        _clusterConfig.cellsX = std::max(1, lightingParams.cellsX);
        _clusterConfig.cellsY = std::max(1, lightingParams.cellsY);
        _clusterConfig.cellsZ = std::max(1, lightingParams.cellsZ);
        _clusterConfig.maxLightsPerCell = std::max(1, lightingParams.maxLightsPerCell);
    }

    void Renderer::bindLayerClusterLights(const std::vector<ClusterLightData>& lights,
        const std::vector<const void*>& lightSetMembers)
    {
        // The light list is per (camera, layer) — the gather filters on
        // LightComponent::rendersLayer — so the grid has to be too. One grid for the
        // frame would light a layer whose lights differ by another layer's cells, and
        // leave a layer with no clustered lights lit by the previous layer's buffers.
        //
        // Grids are keyed on the light set, so two layers that see the same lights
        // still share one and it is built once. The grid is sized from the lights
        // alone (see WorldClusters::computeGridBounds).
        WorldClusters* clusters = clustersForLightSet(lightSetHash(lightSetMembers), lights);

        // Bind the clustered shadow atlas for this frame.
        if (_lightTextureAtlas) {
            _device->setClusterShadowAtlas(_lightTextureAtlas->shadowAtlasTexture());
            _device->setClusterCookieAtlas(_lightTextureAtlas->cookieAtlasTexture());
        }

        bindLayerClusters(clusters);
    }

    bool Renderer::forwardShaderPreparationDue()
    {
        if (!_device) {
            return false;
        }
        // Decided once a frame: the first frame, and any frame after one that built a
        // variant (new content tends to arrive over several frames).
        const int frame = _device->renderVersion();
        if (frame != _shaderPreparationFrame) {
            _shaderPreparationFrame = frame;
            const auto programLibrary = getProgramLibrary(_device);
            const uint64_t created = programLibrary ? programLibrary->forwardVariantsCreated() : 0;
            _shaderPreparationDue = !_shaderPreparationEver || created != _shaderPreparationVariantsSeen;
            _shaderPreparationVariantsSeen = created;
            _shaderPreparationEver = true;
        }
        return _shaderPreparationDue;
    }

    void Renderer::prepareForwardShaders(Camera* camera, const RenderTarget* renderTarget, Layer* layer,
        const bool transparent)
    {
        if (!camera || !layer || !_device || !forwardShaderPreparationDue()) {
            return;
        }
        const auto programLibrary = getProgramLibrary(_device);
        if (!programLibrary) {
            return;
        }
        // The frame switches as renderForwardLayer will set them for this camera. One
        // of them can still change before it runs (the lighting-mode SSAO texture is
        // published by a pass of this frame); renderForwardLayer resolves again, with
        // the switches as they then are.
        configureForwardShaderFeatures(*programLibrary, *camera, renderTarget, _scene && _scene->clusteredLightingEnabled());
        programLibrary->setClusteredLightingEnabled(_scene && _scene->clusteredLightingEnabled() &&
            layerMayHaveClusteredLights(*layer, _scene.get()));
        const CulledInstances& visible = culledInstances(camera, camera->node(), layer);
        const auto defaultMaterial = getDefaultMaterial(_device);
        resolveNewForwardShaders(*programLibrary, defaultMaterial.get(), transparent, !camera->clipping().empty(),
            transparent ? visible.transparent : visible.opaque,
            [](const MeshInstance* meshInstance) { return meshInstance; },
            [](const MeshInstance* meshInstance) { return meshInstance ? meshInstance->material() : nullptr; });
    }

    Vector4 Renderer::ditherJitterFor(const Camera* camera)
    {
        if (!camera || camera->jitter() <= 0.0f) {
            return Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        if (_blueNoiseJitterVersion != _device->renderVersion()) {
            _blueNoiseJitterVersion = _device->renderVersion();
            _blueNoiseJitter = _blueNoise.vec4();
        }
        return _blueNoiseJitter;
    }

    void Renderer::renderForwardLayer(Camera* camera, RenderTarget* renderTarget, Layer* layer, bool transparent)
    {
        if (!camera || !layer || !_device) {
            return;
        }

        FrameCounters& counters = _device->frameCounters();
        const ScopedMilliseconds forwardTimer(counters.forwardTime);

        auto programLibrary = getProgramLibrary(_device);
        if (!programLibrary) {
            spdlog::error("ProgramLibrary is not initialized. Forward rendering requires ProgramLibrary.");
            return;
        }

        const bool clusteredEnabled = _scene && _scene->clusteredLightingEnabled();
        configureForwardShaderFeatures(*programLibrary, *camera, renderTarget, clusteredEnabled);
        if (clusteredEnabled) {
            resolveClusterConfig();
        }

        const auto defaultMaterial = getDefaultMaterial(_device);

        ForwardView view = setupForwardView(*_device, _scene.get(), *camera, renderTarget);

        // Apply the camera's rect on the active render target for this pass only.
        const ViewportScissorScope viewportScope(*_device);
        _device->setViewport(
            static_cast<float>(view.viewport.x),
            static_cast<float>(view.viewport.y),
            static_cast<float>(view.viewport.w),
            static_cast<float>(view.viewport.h));
        _device->setScissor(view.scissor.x, view.scissor.y, view.scissor.w, view.scissor.h);

        // This function runs once per camera x layer x (opaque|transparent), so several
        // times a frame: the entries and the vectors holding them are reused across
        // calls rather than allocated each time. thread_local for the same reason.
        static thread_local ObjectPool<ForwardDrawEntry> drawEntryPool(256);
        static thread_local std::vector<ForwardDrawEntry*> drawEntries;
        static thread_local ForwardLights lights;
        drawEntryPool.freeAll();
        drawEntries.clear();
        drawEntries.reserve(256);

        // Culling has already happened, once for this (camera, layer) pair — see
        // Renderer::cullMeshInstancesInto. Each sublayer reads its own bucket.
        CulledInstances& visible = resolveCulledInstances(camera, view.cameraNode, layer);
        view.hasFrustum = view.cameraNode != nullptr;
        view.frustum = view.hasFrustum ? buildCameraFrustum(camera, view.cameraNode) : Frustum{};

        // While the set is reused, its bucket's draw order from the last sort stands as
        // well: the sort's inputs are the set's stamps (keys and bounds through the
        // CullVersion, the view through the frustum) and the layer's mode. The entries
        // are then collected in that order and the sort skipped. The order is kept
        // only as a permutation of the WHOLE bucket (an entry dropped for the frame,
        // such as a drawOncePerFrame instance another camera claimed, would otherwise
        // be missing from it for good) and never under a comparator or a sort distance
        // the stamps do not cover.
        const std::vector<MeshInstance*>& bucket = transparent ? visible.transparent : visible.opaque;
        CulledInstances::SortedOrder& order = transparent ? visible.transparentOrder : visible.opaqueOrder;
        const SortMode sortMode = transparent ? layer->transparentSortMode() : layer->opaqueSortMode();
        const bool reuseOrder = order.valid && order.mode == sortMode;

        collectDrawEntries(reuseOrder ? order.instances : bucket, view,
            defaultMaterial.get(), _device->renderVersion(), drawEntryPool, drawEntries);
        if (!reuseOrder) {
            {
                const ScopedMilliseconds sortTimer(counters.sortTime);
                sortDrawEntries(drawEntries, *layer, transparent);
            }
            bool keep = sortMode != SortMode::SORTMODE_CUSTOM && drawEntries.size() == bucket.size();
            for (const ForwardDrawEntry* entry : drawEntries) {
                if (!keep) {
                    break;
                }
                keep = entry->meshInstance && !entry->meshInstance->calculateSortDistance();
            }
            order.valid = keep;
            order.mode = sortMode;
            order.instances.clear();
            if (keep) {
                order.instances.reserve(drawEntries.size());
                for (const ForwardDrawEntry* entry : drawEntries) {
                    order.instances.push_back(entry->meshInstance);
                }
            }
        } else if (_cullCheck) {
            // Sort the bucket afresh and hold the reused order to it.
            static thread_local ObjectPool<ForwardDrawEntry> checkPool(256);
            static thread_local std::vector<ForwardDrawEntry*> checkEntries;
            checkPool.freeAll();
            checkEntries.clear();
            collectDrawEntries(bucket, view, defaultMaterial.get(), _device->renderVersion(), checkPool, checkEntries);
            sortDrawEntries(checkEntries, *layer, transparent);
            // The first collect claimed this frame's drawOncePerFrame instances, so the
            // second has none of them: compare the orders without them.
            const auto claimedOnce = [](const ForwardDrawEntry* entry) {
                return entry->meshInstance && entry->meshInstance->drawOncePerFrame();
            };
            std::erase_if(checkEntries, claimedOnce);
            static thread_local std::vector<ForwardDrawEntry*> drawnEntries;
            drawnEntries.assign(drawEntries.begin(), drawEntries.end());
            std::erase_if(drawnEntries, claimedOnce);
            bool same = checkEntries.size() == drawnEntries.size();
            for (size_t i = 0; i < checkEntries.size() && same; ++i) {
                same = checkEntries[i]->meshInstance == drawnEntries[i]->meshInstance;
            }
            if (!same) {
                ++_cullCheckMismatches;
                spdlog::error("[Renderer] VISUTWIN_CULL_CHECK: the reused {} draw order of camera {} layer {} "
                    "differs from a fresh sort ({} vs {} entries)", transparent ? "transparent" : "opaque",
                    static_cast<const void*>(camera), layer->id(), drawEntries.size(), checkEntries.size());
            }
        }

        lights.reset();
        gatherForwardLights(view, *layer, _scene.get(), clusteredEnabled, camera, lights);

        bindLayerEnvironment(*_device, _scene.get(), view);
        // The probe volume, as the camera lights with it: a camera capturing for a bake reads the
        // probes of the bake's previous bounce.
        _device->setProbeVolumeData(_device->probeVolumeBlock(camera && camera->probeVolumeCapture()));

        // The opacity dither and the visibility range's dither move with the TAA jitter, one
        // step a frame, and stand still for a camera that does not jitter.
        const Vector4 ditherJitter = ditherJitterFor(camera);
        _device->setDitherJitter(ditherJitter);

        if (clusteredEnabled) {
            static thread_local std::vector<ClusterLightData> clusterLights;
            static thread_local std::vector<const void*> lightSetMembers;
            const bool clusteredCookies = _scene && _scene->lighting().cookiesEnabled &&
                _lightTextureAtlas && _lightTextureAtlas->cookieAtlasTexture();
            const uint32_t clusteredAreaShapes = _scene ? _scene->lighting().areaLightShapes() : 0u;
            const bool clusteredShadows = !_scene || _scene->lighting().shadowsEnabled;
            buildClusterLightInput(lights.local, clusteredShadows, clusteredCookies, clusteredAreaShapes, clusterLights,
                lightSetMembers);
            bindLayerClusterLights(clusterLights, lightSetMembers);
            // A layer with no clustered light draws without the cluster loop: compiled in, the
            // loop costs every fragment its registers though no light is ever found
            // (anti-aliasing's forward pass: 0.416 -> 0.376 ms).
            programLibrary->setClusteredLightingEnabled(!clusterLights.empty());
        }

        drawForwardEntries(ForwardDrawContext{
            .device = _device,
            .scene = _scene.get(),
            .programLibrary = *programLibrary,
            .view = view,
            .lights = lights,
            .layer = *layer,
            .defaultMaterial = defaultMaterial.get(),
            .transparent = transparent,
            .clusteredEnabled = clusteredEnabled,
            .lodJitter = {ditherJitter.getX(), ditherJitter.getY()},
        }, drawEntries);
    }
}
