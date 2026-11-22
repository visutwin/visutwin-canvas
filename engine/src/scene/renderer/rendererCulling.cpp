// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// Renderer: mesh-instance culling (the per camera and layer sets kept across frames) and
// GPU instance culling.
#include "rendererInternal.h"

namespace visutwin::canvas
{
    using namespace renderer_internal;

    namespace
    {
        // Exact comparison on purpose: the two frusta are built from the same matrices
        // by the same code, so they are bit-identical unless the camera actually
        // changed. A tolerance here would hide the case this guard exists for.
        bool frustumsEqual(const Frustum& a, const Frustum& b)
        {
            for (int i = 0; i < 6; ++i) {
                if (a.planes[i] != b.planes[i]) {
                    return false;
                }
            }
            return true;
        }
    }

    void Renderer::setPrecompileFrame(const bool active, const bool frustumCulling)
    {
        if (_precompileFrame && !active) {
            _culledInstances.clear();
        }
        _precompileFrame = active;
        _precompileFrustumCulling = active && frustumCulling;
    }

    bool Renderer::cullSetCurrent(const CulledInstances& set, const Camera* camera, const Frustum& frustum) const
    {
        if (_precompileFrame || !_cullReuse || !set.valid || set.cullVersion != CullVersion::current()) {
            return false;
        }
        const uint32_t cullingMask = camera ? camera->cullingMask() : 0xFFFFFFFFu;
        const bool skyVisible = camera ? camera->skyVisible() : true;
        return set.cullingMask == cullingMask && set.skyVisible == skyVisible && frustumsEqual(set.frustum, frustum);
    }

    void Renderer::checkReusedCullSet(Camera* camera, GraphNode* cameraNode, Layer* layer,
        const CulledInstances& reused)
    {
        CulledInstances fresh;
        cullMeshInstancesInto(camera, cameraNode, layer, fresh);
        if (fresh.opaque == reused.opaque && fresh.transparent == reused.transparent) {
            return;
        }
        ++_cullCheckMismatches;
        spdlog::error("[Renderer] VISUTWIN_CULL_CHECK: the reused cull set of camera {} layer {} differs from "
            "the sweep (opaque {} vs {}, transparent {} vs {}); something the sweep reads changed without "
            "moving the CullVersion", static_cast<const void*>(camera), layer ? layer->id() : -1,
            reused.opaque.size(), fresh.opaque.size(), reused.transparent.size(), fresh.transparent.size());
    }

    void Renderer::requestMeshInstanceCull(Camera* camera, Layer* layer)
    {
        if (!camera || !layer) {
            return;
        }
        auto& layers = _cullRequests[camera];
        if (layers.empty()) {
            _cullCameras.push_back(camera);
        }
        // De-duplicated, because a camera asks for the same layer twice — once for
        // its opaque sublayer and once for its transparent one — and culling it
        // twice is the waste this whole cache exists to remove.
        if (std::find(layers.begin(), layers.end(), layer) == layers.end()) {
            layers.push_back(layer);
        }
    }

    void Renderer::executeMeshInstanceCull()
    {
        _device->frameCounters().camerasRendered += static_cast<int>(_cullCameras.size());
        for (Camera* camera : _cullCameras) {
            // Before the frustum is built, so a listener may still move the camera.
            // Listeners get the camera itself: there is no back pointer from Camera
            // to its component, and an internal camera (shadow, reflection, picker)
            // has none.
            if (_scene) {
                _scene->fire("precull", camera);
            }
            // Every layer this camera asked for whose kept set is not current, in ONE
            // sweep of the scene. The stamps are compared here, after precull, so a
            // listener that moved something there (the CullVersion) or the camera (the
            // frustum) re-culls; one that changes things in postcull is seen next frame,
            // as the cull itself would not see it this frame either.
            GraphNode* cameraNode = camera ? camera->node() : nullptr;
            const std::vector<Layer*>& layers = _cullRequests[camera];
            static thread_local std::vector<CulledInstances*> outs;
            static thread_local std::vector<Layer*> layersToCull;
            outs.clear();
            layersToCull.clear();
            {
                const ScopedMilliseconds cullTimer(_device->frameCounters().cullTime);
                const bool hasCameraFrustum = camera && cameraNode;
                const Frustum frustum = hasCameraFrustum ? buildCameraFrustum(camera, cameraNode) : Frustum{};
                for (Layer* layer : layers) {
                    CulledInstances& set = _culledInstances[{camera, layer}];
                    if (cullSetCurrent(set, camera, frustum)) {
                        set.used = true;
                        ++_device->frameCounters().cullSetsReused;
                        if (_cullCheck) {
                            checkReusedCullSet(camera, cameraNode, layer, set);
                        }
                        continue;
                    }
                    layersToCull.push_back(layer);
                    outs.push_back(&set);
                }
            }
            if (!layersToCull.empty()) {
                cullMeshInstances(camera, cameraNode, layersToCull.data(), outs.data(), layersToCull.size());
            }
            if (_scene) {
                _scene->fire("postcull", camera);
            }
        }
        _cullCameras.clear();
        _cullRequests.clear();
    }

    const Renderer::CulledInstances& Renderer::culledInstances(Camera* camera,
        GraphNode* cameraNode, Layer* layer)
    {
        return resolveCulledInstances(camera, cameraNode, layer);
    }

    Renderer::CulledInstances& Renderer::resolveCulledInstances(Camera* camera, GraphNode* cameraNode, Layer* layer)
    {
        const auto key = std::make_pair(camera, layer);
        CulledInstances& entry = _culledInstances[key];

        // A set culled or reused this frame answers while it was culled against the
        // frustum this camera has NOW: on the first frame the aspect ratio can change
        // between the batch and the pass, so the set would be the answer for a
        // differently shaped view. The CullVersion is not compared here: the frame draws
        // what its build culled, as it did when every set was culled per frame.
        const bool hasCameraFrustum = camera && cameraNode;
        const Frustum current = hasCameraFrustum
            ? buildCameraFrustum(camera, cameraNode) : Frustum{};
        if (entry.used && frustumsEqual(entry.frustum, current)) {
            return entry;
        }

        // The frame graph never asked for this pair — an app-appended pass reaches here
        // with a camera the composition never saw, and rendering nothing would be the
        // wrong answer — or the camera has changed since it did. A set kept from an
        // earlier frame still stands if nothing it reads changed.
        if (cullSetCurrent(entry, camera, current)) {
            entry.used = true;
            ++_device->frameCounters().cullSetsReused;
            return entry;
        }
        cullMeshInstancesInto(camera, cameraNode, layer, entry);
        return entry;
    }

    void Renderer::cullMeshInstancesInto(Camera* camera, GraphNode* cameraNode, Layer* layer,
        CulledInstances& out)
    {
        CulledInstances* outs[1] = {&out};
        cullMeshInstances(camera, cameraNode, &layer, outs, 1);
    }

    void Renderer::cullMeshInstances(Camera* camera, GraphNode* cameraNode, Layer* const* layers,
        CulledInstances* const* outs, const size_t layerCount)
    {
        // A component's layers are matched against the requested ones through a bit per
        // request, so the sweep below handles at most this many at once; a camera asking
        // for more (none does) is served in slices.
        constexpr size_t kMaxLayersPerSweep = 64;
        if (layerCount > kMaxLayersPerSweep) {
            cullMeshInstances(camera, cameraNode, layers, outs, kMaxLayersPerSweep);
            cullMeshInstances(camera, cameraNode, layers + kMaxLayersPerSweep, outs + kMaxLayersPerSweep,
                layerCount - kMaxLayersPerSweep);
            return;
        }

        const ScopedMilliseconds cullTimer(_device->frameCounters().cullTime);

        const bool hasCameraFrustum = camera && cameraNode;
        const Frustum cameraFrustum = hasCameraFrustum
            ? buildCameraFrustum(camera, cameraNode) : Frustum{};
        const Vector3 cameraPosition = hasCameraFrustum ? cameraNode->position() : Vector3();
        // A precompile culls nothing by view, unless asked to (setPrecompileFrame).
        const bool frustumTest = hasCameraFrustum && (!_precompileFrame || _precompileFrustumCulling);
        const uint32_t cullingMask = camera ? camera->cullingMask() : 0xFFFFFFFFu;

        // The stamps this set is culled under, read before the sweep: the sweep itself
        // moves nothing.
        const uint64_t cullVersion = CullVersion::current();
        const bool skyVisible = camera ? camera->skyVisible() : true;

        std::array<int, kMaxLayersPerSweep> layerIds{};
        uint64_t requested = 0;
        for (size_t i = 0; i < layerCount; ++i) {
            CulledInstances& out = *outs[i];
            out.opaque.clear();
            out.transparent.clear();
            out.opaqueOrder.valid = false;
            out.transparentOrder.valid = false;
            if (!layers[i]) {
                continue;
            }
            out.frustum = cameraFrustum;
            out.cullVersion = cullVersion;
            out.cullingMask = cullingMask;
            out.skyVisible = skyVisible;
            out.valid = true;
            out.used = true;
            layerIds[i] = layers[i]->id();
            requested |= uint64_t{1} << i;
        }
        if (requested == 0) {
            return;
        }

        // The same for every instance of the sweep.
        const auto fallbackMaterial = getDefaultMaterial(_device);

        enum class Bucket { None, Opaque, Transparent };
        // Whether the camera draws this instance, and in which sublayer. Decided ONCE per
        // instance: an instance on several requested layers goes into each of their
        // buckets on this one answer.
        const auto classify = [&](MeshInstance* meshInstance) {
            if (!meshInstance || !meshInstance->visible() || !meshInstance->mesh()) {
                return Bucket::None;
            }
            if (!meshInstance->mesh()->hasVertexBuffer()) {
                return Bucket::None;
            }
            // Camera::cullingMask against MeshInstance::mask: a camera that
            // wants a subset of the scene says so here rather than by juggling layers.
            if ((meshInstance->mask() & cullingMask) == 0u) {
                return Bucket::None;
            }
            if (meshInstance->onlyCamera() && meshInstance->onlyCamera() != camera) {
                return Bucket::None;
            }
            if (meshInstance->replacesSky() && (!camera || camera->skyVisible())) {
                return Bucket::None;
            }

            const Material* material = meshInstance->material()
                ? meshInstance->material() : fallbackMaterial.get();
            if (!material) {
                return Bucket::None;
            }

            // A camera may leave the scene's sky to an effect of its frame.
            if (material->isSkybox() && camera && !camera->skyVisible()) {
                return Bucket::None;
            }

            // A skybox is drawn around the camera and has no meaningful bounds, so it
            // is never frustum culled.
            if (!material->isSkybox() && frustumTest && meshInstance->cull() &&
                !isVisibleInFrustum(cameraFrustum, meshInstance->aabb())) {
                return Bucket::None;
            }
            // Outside its visibility range, as outside the frustum. A set reused while the
            // frustum is unchanged keeps this answer: the camera has not moved.
            if (frustumTest && !material->isSkybox() && meshInstance->visibilityFrom(cameraPosition).hidden()) {
                return Bucket::None;
            }

            // Split here, ONCE, so each sublayer reads only its own bucket.
            return material->transparent() ? Bucket::Transparent : Bucket::Opaque;
        };
        const auto push = [&](const size_t i, const Bucket bucket, MeshInstance* meshInstance) {
            (bucket == Bucket::Transparent ? outs[i]->transparent : outs[i]->opaque).push_back(meshInstance);
        };

        // ONE sweep of the components for all of the camera's layers, rather than one
        // per layer — five for a default camera (World, Depth, Skybox, UI, Immediate),
        // four of which find nearly nothing. The order inside each bucket is what a
        // per-layer sweep would produce: components in creation order, then the layer's
        // own instances.
        for (auto* renderComponent : instancesOf<RenderComponent>(componentRegistry())) {
            // active() covers both halves: the component's own flag and the owning
            // entity's hierarchy state.
            if (!renderComponent || !renderComponent->active()) {
                continue;
            }
            uint64_t onLayers = 0;
            for (const int layerId : renderComponent->layers()) {
                for (size_t i = 0; i < layerCount; ++i) {
                    if (layerIds[i] == layerId) {
                        onLayers |= uint64_t{1} << i;
                    }
                }
            }
            onLayers &= requested;
            if (onLayers == 0) {
                continue;
            }
            for (auto* meshInstance : renderComponent->meshInstances()) {
                const Bucket bucket = classify(meshInstance);
                if (bucket == Bucket::None) {
                    continue;
                }
                for (uint64_t remaining = onLayers; remaining != 0; remaining &= remaining - 1) {
                    push(static_cast<size_t>(std::countr_zero(remaining)), bucket, meshInstance);
                }
            }
        }

        // Instances added to a layer directly rather than through a component.
        for (size_t i = 0; i < layerCount; ++i) {
            if (!layers[i]) {
                continue;
            }
            for (auto* meshInstance : layers[i]->meshInstances()) {
                if (const Bucket bucket = classify(meshInstance); bucket != Bucket::None) {
                    push(i, bucket, meshInstance);
                }
            }
        }
    }

    void Renderer::dispatchGpuInstanceCulling(const std::vector<Camera*>& cameras)
    {
        if (!_device || MeshInstance::gpuCulledInstanceCount() == 0) {
            return;
        }
        // Each camera's frustum planes, once. The extraction expects a column-major
        // float[16], which is Matrix4's in-memory layout. Occlusion, for a camera whose frame
        // does not cull in the frame itself, against its pyramid of the frame before (this
        // frame's does not exist yet), with the matrix that pyramid's depth was rendered with.
        const int version = _device->renderVersion();
        std::vector<GpuCullView> views;
        views.reserve(cameras.size());
        for (Camera* camera : cameras) {
            // A multi-view camera has no one frustum a culled set could be compacted for: it
            // gets no output, so its draws take the whole instance buffer.
            if (!camera || !camera->node() || camera->multiView()) {
                continue;
            }
            GpuCullView view;
            view.camera = camera;
            const Matrix4 vp = camera->projectionMatrix() * camera->node()->worldTransform().inverse();
            InstanceCuller::extractFrustumPlanes(reinterpret_cast<const float*>(&vp), view.planes);
            const auto& handle = camera->hiZMapHandle();
            if (!camera->gpuOcclusionInFrame() && handle && handle->renderVersion == version - 1 &&
                !handle->texture.expired()) {
                view.hiZ = handle;
                view.occlusionViewProjection = handle->viewProjection;
            }
            views.push_back(std::move(view));
        }
        cullGpuInstances(views);
    }

    void Renderer::cullGpuInstancesInFrame(Camera* camera, const Matrix4& depthViewProjection)
    {
        if (!_device || !camera || !camera->node() || MeshInstance::gpuCulledInstanceCount() == 0) {
            return;
        }
        const auto& handle = camera->hiZMapHandle();
        if (!handle || handle->renderVersion != _device->renderVersion() || handle->texture.expired()) {
            return;
        }
        GpuCullView view;
        view.camera = camera;
        const Matrix4 vp = camera->projectionMatrix() * camera->node()->worldTransform().inverse();
        InstanceCuller::extractFrustumPlanes(reinterpret_cast<const float*>(&vp), view.planes);
        view.hiZ = handle;
        view.occlusionViewProjection = depthViewProjection;
        cullGpuInstances({view});
    }

    void Renderer::cullGpuInstances(const std::vector<GpuCullView>& views)
    {
        if (views.empty()) {
            return;
        }
        const int version = _device->renderVersion();
        // An output no camera has culled for this long belongs to a camera that is gone or
        // no longer draws.
        constexpr int kStaleFrames = 120;

        for (auto* rc : instancesOf<RenderComponent>(componentRegistry())) {
            // active(): a disabled entity's instances are not drawn, so culling them
            // is wasted GPU work.
            if (!rc || !rc->active()) {
                continue;
            }
            for (auto* mi : rc->meshInstances()) {
                if (!mi || !mi->gpuCullingEnabled()) {
                    continue;
                }
                const auto& srcData = mi->instancingData();
                if (!srcData.vertexBuffer || srcData.count <= 0) {
                    continue;
                }
                // The kernel reads and compacts 80-byte records. A buffer of another format, set
                // after culling was enabled (an application re-setting its instances every
                // frame), would be read as garbage: it is drawn whole instead, uncut.
                if (!srcData.vertexBuffer->format() || !srcData.vertexBuffer->format()->hasInstanceColor()) {
                    continue;
                }
                auto* srcMesh = mi->mesh();
                if (!srcMesh) {
                    continue;
                }

                // The instances are in the NODE's space (the vertex stage composes the
                // node's world matrix), so the world planes are carried into it: for
                // p_world = M p_local a plane (n, d) becomes M^T (n, d), whose value at a
                // local point is still the WORLD signed distance — the kernel's test is
                // unchanged — and the local sphere radius is scaled to world by M's
                // largest axis. An identity node leaves both exactly as they were.
                float m[16];
                (mi->node() ? mi->node()->worldTransform() : Matrix4::identity()).store(m);   // column-major
                static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                const bool identity = std::equal(std::begin(m), std::end(m), std::begin(kIdentity));
                const float maxScale = identity ? 1.0f : std::sqrt(std::max({
                    m[0] * m[0] + m[1] * m[1] + m[2] * m[2],
                    m[4] * m[4] + m[5] * m[5] + m[6] * m[6],
                    m[8] * m[8] + m[9] * m[9] + m[10] * m[10]}));

                InstanceCullParams params{};
                params.boundingSphereRadius = mi->instanceCullRadius() * maxScale;
                params.instanceCount = static_cast<uint32_t>(srcData.count);
                const auto prim = srcMesh->getPrimitive();
                params.indexCount  = static_cast<uint32_t>(prim.count);
                params.indexStart  = static_cast<uint32_t>(prim.base);
                params.baseVertex  = static_cast<int32_t>(prim.baseVertex);
                params.baseInstance = 0u;

                for (const GpuCullView& view : views) {
                    auto* output = mi->gpuCullOutputFor(view.camera);
                    if (!output) {
                        continue;
                    }
                    // Occlusion against the view's pyramid (last frame's at graph build, this
                    // frame's in the frame), its depth's view-projection carried into the
                    // instances' space as the planes are; a view without one culls by the
                    // frustum alone.
                    std::shared_ptr<HiZMapHandle> hiZ;
                    params.occlusion[0] = 0.0f;
                    if (mi->gpuOcclusionCulling() && view.hiZ) {
                        hiZ = view.hiZ;
                        const Matrix4 toClip = view.occlusionViewProjection *
                            (mi->node() ? mi->node()->worldTransform() : Matrix4::identity());
                        toClip.store(params.occlusionMatrix);
                        params.occlusion[0] = 1.0f;
                        params.occlusion[1] = mi->instanceCullRadius();
                    }
                    if (identity) {
                        std::memcpy(params.frustumPlanes, view.planes, sizeof(view.planes));
                    } else {
                        for (int p = 0; p < 6; ++p) {
                            for (int c = 0; c < 4; ++c) {
                                params.frustumPlanes[p][c] = m[4 * c] * view.planes[p][0] +
                                    m[4 * c + 1] * view.planes[p][1] + m[4 * c + 2] * view.planes[p][2] +
                                    m[4 * c + 3] * view.planes[p][3];
                            }
                        }
                    }
                    output->culler->cull(srcData.vertexBuffer, params, hiZ);
                    output->culledVersion = version;
                }
                mi->pruneGpuCullOutputs(version - kStaleFrames);
            }
        }
    }
}
