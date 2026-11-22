// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// MetalGraphicsDevice: per-draw state, uniforms and bindings, and the draw calls.
#include <cmath>
#include "metalGraphicsDevice.h"
#include "platform/graphics/bindlessTable.h"

#include "platform/graphics/textureSamplerState.h"
#include "platform/graphics/runLoopWait.h"
#include <typeinfo>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ranges>
#include "metalTexture.h"
#include "core/scopedTimer.h"
#include "platform/graphics/screenshot.h"
#include "metalComputePipeline.h"
#include "metalIndexBuffer.h"
#include "metalShader.h"
#include "metalRenderPipeline.h"
#include "metalRenderTarget.h"
#include "metalUtils.h"
#include "metalVertexBuffer.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/stencilParameters.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    using metal::toMetalPrimitiveType;
    using metal::toMetalCullMode;

    namespace
    {
        MTL::CompareFunction toMetalCompare(const CompareFunction function)
        {
            switch (function) {
            case CompareFunction::Never:        return MTL::CompareFunctionNever;
            case CompareFunction::Less:         return MTL::CompareFunctionLess;
            case CompareFunction::Equal:        return MTL::CompareFunctionEqual;
            case CompareFunction::LessEqual:    return MTL::CompareFunctionLessEqual;
            case CompareFunction::Greater:      return MTL::CompareFunctionGreater;
            case CompareFunction::NotEqual:     return MTL::CompareFunctionNotEqual;
            case CompareFunction::GreaterEqual: return MTL::CompareFunctionGreaterEqual;
            case CompareFunction::Always:       return MTL::CompareFunctionAlways;
            }
            return MTL::CompareFunctionAlways;
        }

        MTL::StencilOperation toMetalStencilOperation(const StencilOperation operation)
        {
            switch (operation) {
            case StencilOperation::Keep:           return MTL::StencilOperationKeep;
            case StencilOperation::Zero:           return MTL::StencilOperationZero;
            case StencilOperation::Replace:        return MTL::StencilOperationReplace;
            case StencilOperation::IncrementClamp: return MTL::StencilOperationIncrementClamp;
            case StencilOperation::DecrementClamp: return MTL::StencilOperationDecrementClamp;
            case StencilOperation::Invert:         return MTL::StencilOperationInvert;
            case StencilOperation::IncrementWrap:  return MTL::StencilOperationIncrementWrap;
            case StencilOperation::DecrementWrap:  return MTL::StencilOperationDecrementWrap;
            }
            return MTL::StencilOperationKeep;
        }

        void configureStencilDescriptor(MTL::StencilDescriptor* descriptor,
            const StencilParameters* parameters)
        {
            descriptor->setStencilCompareFunction(toMetalCompare(parameters->compareFunction()));
            descriptor->setStencilFailureOperation(toMetalStencilOperation(parameters->failOperation()));
            descriptor->setDepthFailureOperation(toMetalStencilOperation(parameters->depthFailOperation()));
            descriptor->setDepthStencilPassOperation(toMetalStencilOperation(parameters->passOperation()));
            descriptor->setReadMask(parameters->readMask());
            descriptor->setWriteMask(parameters->writeMask());
        }
    }

    constexpr int INDIRECT_ENTRY_BYTE_SIZE = 5 * 4; // 5 x 32bit


    int MetalGraphicsDevice::submitVertexBuffer(const std::shared_ptr<VertexBuffer>& vertexBuffer, int slot)
    {
        if (!vertexBuffer || !_renderPassEncoder) {
            return 0;
        }
        auto* vbBuffer = static_cast<MTL::Buffer*>(vertexBuffer->nativeBuffer());
        if (!vbBuffer) {
            spdlog::warn("Vertex buffer has no native Metal buffer bound");
            return 0;
        }

        // Slots 0 (mesh) and 5 (instancing) are written only here, so a buffer the
        // encoder already holds there needs no rebind: consecutive draws of one mesh,
        // and every draw sharing an instancing buffer. The command buffer retains what
        // was bound, so a pointer cannot come back as a different buffer within a pass.
        // Slot 1 is not cached — the scene block is set there with setVertexBytes.
        MTL::Buffer** cached = slot == 0 ? &_encoderVertexBuffer0
            : (slot == 5 ? &_encoderInstancingBuffer : nullptr);
        if (cached) {
            if (*cached == vbBuffer) {
                return 1;
            }
            *cached = vbBuffer;
        }

        _renderPassEncoder->setVertexBuffer(vbBuffer, 0, static_cast<NS::UInteger>(slot));
        return 1;
    }

    void MetalGraphicsDevice::resetEncoderStateCache()
    {
        _encoderBindlessKey = nullptr;
        _encoderBindlessResidentVersion = 0;
        _encoderLanesVersion = 0;
        _pipelineState = nullptr;
        _encoderVertexBuffer0 = nullptr;
        _encoderInstancingBuffer = nullptr;
        _encoderDepthStencilState = nullptr;
        _encoderCullMode = -1;
        _encoderStencilReferenceValid = false;
    }


    MTL::DepthStencilState* MetalGraphicsDevice::resolveDepthStencilState(const DepthState* depthState,
        StencilParameters* stencilFront, StencilParameters* stencilBack)
    {
        const bool depthTest = !depthState || depthState->depthTest();
        const bool depthWrite = !depthState || depthState->depthWrite();
        const CompareFunction depthFunc = depthState ? depthState->func() : CompareFunction::LessEqual;
        if (!stencilFront && !stencilBack) {
            // The four LessEqual combinations are prebuilt; anything else (a material
            // with its own depthFunc, e.g. the Greater test an x-ray pass wants) falls
            // through to the cache below, which handles stencil-free states too.
            if (depthFunc == CompareFunction::LessEqual) {
                if (depthTest) {
                    return depthWrite ? _defaultDepthStencilState : _noWriteDepthStencilState;
                }
                return depthWrite ? _noTestDepthStencilState : _noTestNoWriteDepthStencilState;
            }
        }

        // A single face descriptor applies to both windings, matching the common
        // front-only API convention. Supplying both enables independent behavior.
        StencilParameters* effectiveFront = stencilFront ? stencilFront : stencilBack;
        StencilParameters* effectiveBack = stencilBack ? stencilBack : stencilFront;

        const DepthStencilCacheKey cacheKey{
            effectiveFront ? effectiveFront->stateKey() : 0u,
            effectiveBack ? effectiveBack->stateKey() : 0u,
            depthTest, depthWrite, depthFunc};
        if (const auto it = _stencilStateCache.find(cacheKey); it != _stencilStateCache.end()) {
            return it->second;
        }

        auto* descriptor = MTL::DepthStencilDescriptor::alloc()->init();
        descriptor->setDepthCompareFunction(depthTest
            ? toMetalCompare(depthFunc) : MTL::CompareFunctionAlways);
        descriptor->setDepthWriteEnabled(depthWrite);

        MTL::StencilDescriptor* frontDescriptor = nullptr;
        MTL::StencilDescriptor* backDescriptor = nullptr;
        if (effectiveFront) {
            frontDescriptor = MTL::StencilDescriptor::alloc()->init();
            configureStencilDescriptor(frontDescriptor, effectiveFront);
            descriptor->setFrontFaceStencil(frontDescriptor);
        }
        if (effectiveBack) {
            backDescriptor = MTL::StencilDescriptor::alloc()->init();
            configureStencilDescriptor(backDescriptor, effectiveBack);
            descriptor->setBackFaceStencil(backDescriptor);
        }

        auto* state = _device->newDepthStencilState(descriptor);
        if (backDescriptor) { backDescriptor->release(); }
        if (frontDescriptor) { frontDescriptor->release(); }
        descriptor->release();
        if (!state) {
            spdlog::error("Failed to create Metal depth/stencil state");
            return nullptr;
        }
        _stencilStateCache.emplace(cacheKey, state);
        return state;
    }






    void MetalGraphicsDevice::setDepthBias(const float depthBias, const float slopeScale, const float clamp)
    {
        if (!_renderPassEncoder) return;
        _renderPassEncoder->setDepthBias(depthBias, slopeScale, clamp);
    }

    void MetalGraphicsDevice::setIndirectDrawBuffer(void* nativeBuffer)
    {
        _indirectDrawBuffer = static_cast<MTL::Buffer*>(nativeBuffer);
    }

    void MetalGraphicsDevice::setDynamicBatchPalette(const void* data, const size_t size,
        const uint64_t contentVersion)
    {
        // No device frame is open in a precompile pass: nothing may take frame memory.
        if (_precompilePass) {
            return;
        }
        // From the palette ring (variable-size bump allocator, which grows): one copy a
        // frame per content version, however many passes draw the skin.
        _pendingPaletteOffset = _paletteRing->allocate(data, size, contentVersion);
    }

    void MetalGraphicsDevice::setParticleState(const std::shared_ptr<VertexBuffer>& particles,
        const std::shared_ptr<VertexBuffer>& order, const std::shared_ptr<VertexBuffer>& meshVertices,
        const void* params, const size_t paramsSize)
    {
        if (!particles || !params || paramsSize == 0 || paramsSize > _pendingParticleParams.size()) {
            _pendingParticleBuffer = nullptr;
            _pendingParticleParamsSize = 0;
            return;
        }
        _pendingParticleBuffer = static_cast<MTL::Buffer*>(particles->nativeBuffer());
        _pendingParticleOrderBuffer = order ? static_cast<MTL::Buffer*>(order->nativeBuffer()) : nullptr;
        _pendingParticleMeshBuffer = meshVertices ? static_cast<MTL::Buffer*>(meshVertices->nativeBuffer()) : nullptr;
        std::memcpy(_pendingParticleParams.data(), params, paramsSize);
        _pendingParticleParamsSize = paramsSize;
    }

    void MetalGraphicsDevice::setGSplatState(const std::shared_ptr<VertexBuffer>& splats,
        const std::shared_ptr<VertexBuffer>& order, const std::shared_ptr<VertexBuffer>& sh,
        const void* params, const size_t paramsSize)
    {
        if (!splats || !order || !params || paramsSize == 0 || paramsSize > _pendingGSplatParams.size()) {
            _pendingGSplatBuffer = nullptr;
            _pendingGSplatOrderBuffer = nullptr;
            _pendingGSplatShBuffer = nullptr;
            _pendingGSplatParamsSize = 0;
            return;
        }
        _pendingGSplatBuffer = static_cast<MTL::Buffer*>(splats->nativeBuffer());
        _pendingGSplatOrderBuffer = static_cast<MTL::Buffer*>(order->nativeBuffer());
        _pendingGSplatShBuffer = sh ? static_cast<MTL::Buffer*>(sh->nativeBuffer()) : nullptr;
        std::memcpy(_pendingGSplatParams.data(), params, paramsSize);
        _pendingGSplatParamsSize = paramsSize;
    }

    void MetalGraphicsDevice::setMorphState(const std::shared_ptr<VertexBuffer>& deltaBuffer,
        const void* params, const size_t paramsSize)
    {
        if (!deltaBuffer || !params || paramsSize == 0 || paramsSize > _pendingMorphParams.size()) {
            _pendingMorphDeltaBuffer = nullptr;
            _pendingMorphParamsSize = 0;
            return;
        }
        _pendingMorphDeltaBuffer = static_cast<MTL::Buffer*>(deltaBuffer->nativeBuffer());
        std::memcpy(_pendingMorphParams.data(), params, paramsSize);
        _pendingMorphParamsSize = paramsSize;
    }

    size_t MetalGraphicsDevice::clusterBufferVram() const
    {
        size_t total = 0;
        for (const auto& slot : _clusterBufferSets) {
            for (const auto& set : slot) {
                total += set.lightCapacity + set.cellCapacity;
            }
        }
        return total;
    }

    void MetalGraphicsDevice::setClusterBuffers(const void* lightData, const size_t lightSize,
        const void* cellData, const size_t cellSize)
    {
        // No device frame is open in a precompile pass: the per-frame slots are not this one's.
        if (_precompilePass) {
            return;
        }
        if (!lightData || lightSize == 0 || !cellData || cellSize == 0) {
            // No grid for this layer: the stand-ins replace whatever an earlier layer bound.
            // The pair and the last-data pointers stay, so the same grid bound again later in
            // the frame still reuses its pair.
            _clusterBuffersSet = false;
            bindClusterBuffersOnEncoder();
            return;
        }

        // The renderer binds a grid per layer, and two layers of a frame may see different
        // grids. A shared-storage buffer is read by the GPU when the frame executes, not
        // when it is encoded, so ONE buffer rewritten per layer would light every layer of
        // the frame with the last grid written — and, with frames in flight, with a LATER
        // frame's grid. Each grid a frame binds therefore gets its own pair from this
        // frame's slot. The same grid bound again in the frame (the renderer pools them,
        // and two layers seeing the same lights share one) reuses its pair.
        if (lightData != _clusterLastLightData || cellData != _clusterLastCellData) {
            auto& slot = _clusterBufferSets[_clusterFrameSlot];
            if (_clusterSetsUsed >= slot.size()) {
                slot.emplace_back();
            }
            ClusterBufferSet& set = slot[_clusterSetsUsed++];
            if (!set.light || set.lightCapacity < lightSize) {
                if (set.light) {
                    set.light->release();   // a command buffer still using it retains it
                }
                set.lightCapacity = lightSize * 2;   // over-allocate to reduce reallocations
                set.light = _device->newBuffer(set.lightCapacity, MTL::ResourceStorageModeShared);
            }
            if (!set.cell || set.cellCapacity < cellSize) {
                if (set.cell) {
                    set.cell->release();
                }
                set.cellCapacity = cellSize * 2;
                set.cell = _device->newBuffer(set.cellCapacity, MTL::ResourceStorageModeShared);
            }
            std::memcpy(set.light->contents(), lightData, lightSize);
            std::memcpy(set.cell->contents(), cellData, cellSize);
            _clusterLightBuffer = set.light;
            _clusterCellBuffer = set.cell;
            _clusterLastLightData = lightData;
            _clusterLastCellData = cellData;
        }
        _clusterBuffersSet = true;

        // A layer binds its grid after its render pass has begun, and startRenderPass binds
        // only what was set before it: bound only there, the first layer to use a new pair
        // would draw with no cluster buffers at all.
        bindClusterBuffersOnEncoder();
    }

    void MetalGraphicsDevice::bindClusterBuffersOnEncoder()
    {
        if (!_renderPassEncoder) {
            return;
        }
        const bool grid = _clusterBuffersSet && _clusterLightBuffer && _clusterCellBuffer;
        _renderPassEncoder->setFragmentBuffer(grid ? _clusterLightBuffer : _clusterEmptyLightBuffer, 0, 7);
        _renderPassEncoder->setFragmentBuffer(grid ? _clusterCellBuffer : _clusterEmptyCellBuffer, 0, 8);
    }

    void MetalGraphicsDevice::setClusterGridParams(const float* boundsMin, const float* boundsRange,
        const float* cellsCountByBoundsSize,
        const int cellsX, const int cellsY, const int cellsZ, const int maxLightsPerCell,
        const int numClusteredLights)
    {
        _uniformBinder.setClusterParams(boundsMin, boundsRange, cellsCountByBoundsSize,
            cellsX, cellsY, cellsZ, maxLightsPerCell, numClusteredLights);
    }

    void MetalGraphicsDevice::setTransformUniforms(const Matrix4& viewProjection, const Matrix4& model,
        const float ditherId)
    {
        if (!_renderPassEncoder) return;
        _uniformBinder.setTransformUniforms(_renderPassEncoder, _transformRing.get(), viewProjection, model,
            ditherId);
    }

    void MetalGraphicsDevice::setLightingUniforms(const Color& ambientColor, const std::vector<GpuLightData>& lights,
        const Vector3& cameraPosition, const bool enableNormalMaps, const float exposure,
        const FogParams& fogParams, const ShadowParams& shadowParams, const int toneMapping,
        const Vector3* ambientSH, const Matrix4* viewProjection, const uint32_t meshLightMask)
    {
        _uniformBinder.setLightingUniforms(ambientColor, lights, cameraPosition,
            enableNormalMaps, exposure, fogParams, shadowParams, toneMapping, ambientSH,
            viewProjection, meshLightMask);
    }

    void MetalGraphicsDevice::setReflectionProbeUniforms(Texture* cubemap, const Vector3& boxMin,
        const Vector3& boxMax, const bool boxProjection, const float intensity, const float maxLod)
    {
        _uniformBinder.setReflectionProbeUniforms(cubemap, boxMin, boxMax, boxProjection, intensity, maxLod);
    }

    void MetalGraphicsDevice::setCameraClipPlanes(const float nearClip, const float farClip)
    {
        _uniformBinder.setCameraClipPlanes(nearClip, farClip);
    }

    void MetalGraphicsDevice::setDebugShaderPass(const uint32_t mode)
    {
        _uniformBinder.setDebugShaderPass(mode);
    }

    void MetalGraphicsDevice::setEnvironmentUniforms(Texture* envAtlas, const float skyboxIntensity,
        const float skyboxMip, const Vector3& skyDomeCenter, const bool isDome,
        Texture* skyboxCubeMap)
    {
        _uniformBinder.setEnvironmentUniforms(envAtlas, skyboxIntensity, skyboxMip,
            skyDomeCenter, isDome, skyboxCubeMap);
    }

    void MetalGraphicsDevice::setAtmosphereUniforms(const void* data, const size_t size)
    {
        _uniformBinder.setAtmosphereUniforms(data, size);
    }


    uint32_t MetalGraphicsDevice::renderTargetFormatKey()
    {
        const RenderTarget* target = _renderTarget.get();
        if (!_psoFormatKeyValid || target != _psoFormatKeyTarget) {
            // dynamic_cast, NOT static_cast: the back buffer is a plain RenderTarget,
            // and reading past the base object crashed intermittently in the compose
            // pass. Key 0 mirrors create()'s fixed BGRA8+D32F back-buffer path.
            const auto* metalTarget = dynamic_cast<const MetalRenderTarget*>(target);
            _psoFormatKey = metalTarget ? metalTarget->formatKey() : 0u;
            _psoFormatKeyTarget = target;
            _psoFormatKeyValid = true;
        }
        return _psoFormatKey;
    }

    std::optional<MetalGraphicsDevice::IndexBinding> MetalGraphicsDevice::prepareDraw(const Primitive& primitive,
        const std::shared_ptr<IndexBuffer>& indexBuffer, const bool first)
    {
        if (!_shader || !_renderPassEncoder) {
            spdlog::warn("Draw skipped: shader or render encoder is not set");
            return std::nullopt;
        }

        MTL::RenderCommandEncoder* passEncoder = _renderPassEncoder;
        assert(passEncoder != nullptr);

        if (first && !bindDrawPipeline(passEncoder, primitive, indexBuffer)) {
            return std::nullopt;
        }
        auto indexBinding = resolveIndexBinding(indexBuffer);
        if (!indexBinding) {
            return std::nullopt;
        }
        if (_cullMode == CullMode::CULLFACE_FRONTANDBACK) {
            return std::nullopt;
        }
        applyEncoderCullMode(passEncoder);

        const Material* boundMaterial = material();
        UniformBlock uniforms = selectMaterialUniforms(boundMaterial);
        // A quad pass has no material, so its own uniform block takes the material
        // slot. Set after the material branch so it cannot disturb that path.
        if (quadRenderActive() && !quadUniformData().empty()) {
            uniforms = {quadUniformData().data(), quadUniformData().size()};
        }
        bindPassTextures(passEncoder);
        updateReflectionUniforms();
        submitDrawUniforms(passEncoder, boundMaterial, uniforms);
        bindDrawSampler(passEncoder);
        applyDepthStencilState(passEncoder);
        bindPendingDrawResources(passEncoder);
        bindBindlessTable(passEncoder);
        bindMapLanes(passEncoder);
        return indexBinding;
    }

    void MetalGraphicsDevice::finishDraw(const Primitive& primitive, const int numInstances)
    {
        recordDraw(primitive, numInstances);
        // What the open command buffer holds, for endRenderPass's early commit.
        ++_openBufferDraws;
        _openBufferVertices += static_cast<uint64_t>(primitive.count) *
            static_cast<uint64_t>(std::max(numInstances, 1));
    }

    void MetalGraphicsDevice::draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer,
                int numInstances, const int indirectSlot, const bool first, const bool last)
    {
        if (_precompilePass) {
            requestDrawPipeline(primitive, indexBuffer);
            if (last) {
                clearVertexBuffer();
            }
            return;
        }
        const auto indexBinding = prepareDraw(primitive, indexBuffer, first);
        if (!indexBinding) {
            return;
        }
        encodeDraw(_renderPassEncoder, primitive, *indexBinding, numInstances, indirectSlot);
        finishDraw(primitive, numInstances);

        if (last) {
            // Clear the vertex buffer array. What the ENCODER holds stays as it is: the
            // next draw compares against it (_pipelineState, _encoderVertexBuffer0 ...).
            clearVertexBuffer();
        }
    }

    void MetalGraphicsDevice::multiDraw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer,
        const std::span<const MultiDrawCommand> commands)
    {
        if (commands.empty()) {
            return;
        }
        if (_precompilePass) {
            requestDrawPipeline(primitive, indexBuffer);
            clearVertexBuffer();
            return;
        }
        // Metal has no multi-draw call: the state is bound and checked ONCE for the list (every
        // sub-draw shares it), then each sub-draw is encoded.
        const auto indexBinding = prepareDraw(primitive, indexBuffer, true);
        if (!indexBinding) {
            return;
        }
        for (const MultiDrawCommand& command : commands) {
            const Primitive narrowed = narrowedPrimitive(primitive, command);
            encodeDraw(_renderPassEncoder, narrowed, *indexBinding, command.instanceCount, -1);
            finishDraw(narrowed, command.instanceCount);
        }
        clearVertexBuffer();
    }

    const std::shared_ptr<VertexBuffer>* MetalGraphicsDevice::instancingVertexBuffer() const
    {
        for (size_t i = 2; i < _vertexBuffers.size(); ++i) {
            if (_vertexBuffers[i] && _vertexBuffers[i]->format() &&
                _vertexBuffers[i]->format()->isInstancing()) {
                return &_vertexBuffers[i];
            }
        }
        return nullptr;
    }

    void MetalGraphicsDevice::requestDrawPipeline(const Primitive& primitive,
        const std::shared_ptr<IndexBuffer>& indexBuffer)
    {
        if (!_shader || !_blendState || !_renderPipeline) {
            return;
        }
        // The inputs bindDrawPipeline gives get(), so the precompiled pipeline is the one the
        // draw will look up.
        const auto& vb0 = _vertexBuffers.size() > 0 ? _vertexBuffers[0] : _nullVertexBuffer;
        const auto& vb1 = _vertexBuffers.size() > 1 ? _vertexBuffers[1] : _nullVertexBuffer;
        const std::shared_ptr<VertexBuffer>* instancingVBPtr = instancingVertexBuffer();
        const int ibFormat = indexBuffer ? indexBuffer->format() : -1;
        _renderPipeline->request(primitive,
            vb0 != nullptr ? vb0->format() : nullptr,
            vb1 != nullptr ? vb1->format() : nullptr,
            ibFormat, _shader, _renderTarget, _bindGroupFormats, _blendState,
            instancingVBPtr ? (*instancingVBPtr)->format() : nullptr,
            renderTargetFormatKey());
    }

    void MetalGraphicsDevice::onBeginPrecompilePass(RenderPass* renderPass)
    {
        // The target the pass would begin with, as startRenderPass takes it, so the pipelines
        // are built for its attachment formats.
        _psoFormatKeyValid = false;
        const std::shared_ptr<RenderTarget> activeTarget =
            (renderPass && renderPass->renderTarget()) ? renderPass->renderTarget() : backBuffer();
        setRenderTarget(activeTarget);
        if (const auto offscreenTarget = std::dynamic_pointer_cast<MetalRenderTarget>(activeTarget)) {
            offscreenTarget->ensureAttachments();
        }
    }

    void MetalGraphicsDevice::onEndPrecompilePass()
    {
        _psoFormatKeyValid = false;
    }

    int MetalGraphicsDevice::precompilePendingCount() const
    {
        return _renderPipeline ? _renderPipeline->pendingCount() : 0;
    }

    int MetalGraphicsDevice::precompilePipelineRequests() const
    {
        return _renderPipeline ? _renderPipeline->requestCount() : 0;
    }

    void MetalGraphicsDevice::waitPrecompileIdle()
    {
        if (_renderPipeline) {
            _renderPipeline->waitIdle();
        }
    }

    bool MetalGraphicsDevice::bindDrawPipeline(MTL::RenderCommandEncoder* passEncoder, const Primitive& primitive,
        const std::shared_ptr<IndexBuffer>& indexBuffer)
    {
        // Get vertex buffers — use const references to avoid shared_ptr refcount churn.
        const auto& vb0 = _vertexBuffers.size() > 0 ? _vertexBuffers[0] : _nullVertexBuffer;
        const auto& vb1 = _vertexBuffers.size() > 1 ? _vertexBuffers[1] : _nullVertexBuffer;

        // Hardware instancing: find VB with isInstancing() format (set at slot 5 by renderer).
        // Use raw pointer — the shared_ptr in _vertexBuffers keeps the object alive.
        const std::shared_ptr<VertexBuffer>* instancingVBPtr = instancingVertexBuffer();

        if (vb0) {
            const int vbSlot = submitVertexBuffer(vb0, 0);
            if (vb1) {
                submitVertexBuffer(vb1, vbSlot);
            }
        }

        // Submit instancing vertex buffer at slot 5 for vertex descriptor layout(5).
        if (instancingVBPtr) {
            submitVertexBuffer(*instancingVBPtr, 5);
        }

        // Get or create a render pipeline (includes instancing format for perInstance step function)
        const int ibFormat = indexBuffer ? indexBuffer->format() : -1;
        const auto instFmt = instancingVBPtr ? (*instancingVBPtr)->format() : nullptr;
        MTL::RenderPipelineState* pipelineState = _renderPipeline->get(primitive,
            vb0 != nullptr ? vb0->format() : nullptr,
            vb1 != nullptr ? vb1->format() : nullptr,
            ibFormat, _shader, _renderTarget, _bindGroupFormats, _blendState, _depthState,
            _cullMode, _stencilEnabled, _stencilFront, _stencilBack, instFmt,
            renderTargetFormatKey());
        if (!pipelineState) {
            spdlog::error("Draw skipped: failed to create/render pipeline state");
            return false;
        }

        // Set the pipeline state if changed. _pipelineState is what the encoder
        // holds and lives until the pass ends (resetEncoderStateCache); clearing it per
        // draw would make this test always true.
        // NOTE: _pipelineState is a non-owning (borrowing) pointer — the render
        // pipeline cache (_renderPipeline) owns pipeline states and releases them
        // in its destructor.  Do NOT call release() here: the cache destructor
        // releases it too, so that would be a double release.
        if (_pipelineState != pipelineState) {
            _pipelineState = pipelineState;
            passEncoder->setRenderPipelineState(pipelineState);
        }
        return true;
    }

    std::optional<MetalGraphicsDevice::IndexBinding> MetalGraphicsDevice::resolveIndexBinding(
        const std::shared_ptr<IndexBuffer>& indexBuffer)
    {
        IndexBinding binding;
        if (!indexBuffer) {
            return binding;
        }
        binding.buffer = static_cast<MTL::Buffer*>(indexBuffer->nativeBuffer());
        if (!binding.buffer) {
            spdlog::warn("Draw skipped: index buffer has no native Metal buffer");
            return std::nullopt;
        }
        switch (indexBuffer->format()) {
        case INDEXFORMAT_UINT16:
            binding.type = MTL::IndexTypeUInt16;
            break;
        case INDEXFORMAT_UINT32:
            binding.type = MTL::IndexTypeUInt32;
            break;
        case INDEXFORMAT_UINT8:
            // Metal does not support uint8 indices directly.
            spdlog::warn("Draw skipped: uint8 index buffers are not supported on Metal");
            return std::nullopt;
        default:
            binding.type = MTL::IndexTypeUInt16;
            break;
        }
        return binding;
    }

    void MetalGraphicsDevice::applyEncoderCullMode(MTL::RenderCommandEncoder* passEncoder)
    {
        // The winding is set once per pass (startRenderPass); the cull mode when it changes.
        if (const int cullMode = static_cast<int>(_cullMode); cullMode != _encoderCullMode) {
            passEncoder->setCullMode(toMetalCullMode(_cullMode));
            _encoderCullMode = cullMode;
        }
    }

    MetalGraphicsDevice::UniformBlock MetalGraphicsDevice::selectMaterialUniforms(const Material* boundMaterial)
    {
        // The block a draw with no material carries: every such draw's is this one, so
        // it is built once and the binder uploads it once per pass.
        static const MaterialUniforms defaultMaterialUniforms{};

        // Uniform data: use customUniformData() if available (e.g., globe tiles
        // with extended uniforms), otherwise the material's packed block. A material's
        // maps are not bound here: every draw reads them from the bindless table through
        // its map lanes (bindMapLanes).
        UniformBlock uniforms{&defaultMaterialUniforms, sizeof(MaterialUniforms)};
        if (!boundMaterial) {
            return uniforms;
        }

        // isMaterialChanged mirrors submitPerDrawUniforms' dedup condition:
        // for an unchanged material the ring offset is reused and the packed
        // data ignored, so skip the packing too — updateUniforms costs ~35
        // string-map lookups plus transcendentals per call, which dominated
        // the draw loop when run for every draw.
        const bool materialChanged = _uniformBinder.isMaterialChanged(boundMaterial);

        size_t customSize = 0;
        const void* customData = boundMaterial->customUniformData(customSize);
        if (customData && customSize > 0) {
            uniforms = {customData, customSize};
        } else if (instanceMaterialUniforms()) {
            // The mesh instance's overrides over the material's block: always handed over,
            // since the ring reuse is keyed on this block (submitDrawUniforms), not on the
            // material that materialChanged compares.
            uniforms.data = instanceMaterialUniforms();
        } else if (materialChanged) {
            // Packed once per edit and reused across binds — see packedUniforms().
            uniforms.data = &boundMaterial->packedUniforms();
        }
        return uniforms;
    }


    void MetalGraphicsDevice::bindMapLanes(MTL::RenderCommandEncoder* passEncoder)
    {
        // The draw's map lanes (materialMapLanes.h), 112 bytes at buffer 14 on both stages;
        // issued only when the draw's lanes differ from what the encoder holds.
        const MaterialMapLanes& lanes = drawMapLanes();
        if (drawMapLanesVersion() == _encoderLanesVersion) {
            return;
        }
        passEncoder->setVertexBytes(&lanes, sizeof(lanes), kBindlessLanesMetalBufferSlot);
        passEncoder->setFragmentBytes(&lanes, sizeof(lanes), kBindlessLanesMetalBufferSlot);
        _encoderLanesVersion = drawMapLanesVersion();
    }

    void MetalGraphicsDevice::bindPassTextures(MTL::RenderCommandEncoder* passEncoder)
    {
        if (quadRenderActive()) {
            _textureBinder.bindQuadTextures(passEncoder, quadTextureBindings());
            return;
        }
        _textureBinder.bindSceneTextures(passEncoder,
            _uniformBinder.envAtlasTexture(), _uniformBinder.shadowTexture(),
            _uniformBinder.skyboxCubeMapTexture(),
            reflectionMap(), reflectionDepthMap(), ssaoForwardTexture(),
            _areaLightLut1, _areaLightLut2, sceneColorMap(),
            _uniformBinder.reflectionProbeCubeTexture(), sceneDepthGrabMap());
        // The second directional shadow slot's map. The shader gates on the
        // slot's flag, not on the texture: an unbound Metal texture still reports
        // a width and samples zero.
        _textureBinder.bindCached(passEncoder, 35, _uniformBinder.shadowTexture1());
        // The same two maps at the directional VSM moments slots. One specialised forward
        // library cannot give a slot two types, so a VSM light's moments (a colour texture)
        // have slots of their own; the variant reads one of the pair, by the VSM feature.
        _textureBinder.bindCached(passEncoder, 39, _uniformBinder.shadowTexture());
        _textureBinder.bindCached(passEncoder, 40, _uniformBinder.shadowTexture1());
        _textureBinder.bindLocalShadowTextures(passEncoder,
            _uniformBinder.localShadowTexture0(), _uniformBinder.localShadowTexture1());
        // VSM spot maps (EVSM moments, a colour format) beside the depth slots.
        _textureBinder.bindCached(passEncoder, 37, _uniformBinder.localVsmTexture0());
        _textureBinder.bindCached(passEncoder, 38, _uniformBinder.localVsmTexture1());
        _textureBinder.bindOmniShadowTextures(passEncoder,
            _uniformBinder.omniShadowCube0(), _uniformBinder.omniShadowCube1());
        _textureBinder.bindCookieTextures(passEncoder,
            _uniformBinder.cookieTexture2D0(), _uniformBinder.cookieTexture2D1(),
            _uniformBinder.cookieTextureCube0(), _uniformBinder.cookieTextureCube1());
        // Clustered spot-shadow atlas (depth texture2d_array) at slot 26.
        if (_clusterShadowAtlas) {
            _textureBinder.bindCached(passEncoder, 26, _clusterShadowAtlas);
        }
        // Clustered cookie atlas at slot 36. Bound for the cookie pass too, which draws
        // into it; its blit shader declares no slot 36, so nothing reads it there.
        if (_clusterCookieAtlas) {
            _textureBinder.bindCached(passEncoder, 36, _clusterCookieAtlas);
        }
        // The application's texture for its chunk overrides (UserShaderData).
        _textureBinder.bindCached(passEncoder, 46, _userShaderTexture);
    }

    void MetalGraphicsDevice::updateReflectionUniforms()
    {
        // Screen inverse resolution for planar reflection screen-space UV.
        _uniformBinder.setScreenResolution(vw(), vh());

        // Blurred planar reflection parameters.
        const auto& rbp = reflectionBlurParams();
        _uniformBinder.setReflectionBlurParams(
            rbp.intensity, rbp.blurAmount, rbp.fadeStrength, rbp.angleFade,
            rbp.fadeColor.r, rbp.fadeColor.g, rbp.fadeColor.b);
        _uniformBinder.setReflectionDepthParams(rbp.planeDistance, rbp.heightRange, reflectionMap() != nullptr,
            reflectionDepthMap() != nullptr);
        _uniformBinder.setGrabFlags(sceneColorMap() != nullptr, sceneDepthGrabMap() != nullptr);
    }

    void MetalGraphicsDevice::submitDrawUniforms(MTL::RenderCommandEncoder* passEncoder,
        const Material* boundMaterial, const UniformBlock& uniforms)
    {
        // A quad draw supplies its own block in the material slot, so it must not
        // be keyed on the material for the ring's reuse check: nothing clears the
        // bound material for an offline bake, so consecutive quad draws would all
        // look like "same material as last time" and share ONE allocation. That is
        // invisible while a pass draws a single quad — which every effect did until
        // the environment bakes started drawing a rect list in one pass, where the
        // convolve draws then read the reproject block and wrote nothing.
        // A draw with neither a material nor a quad block carries the default block,
        // which one upload per pass serves (every opaque shadow caster and prepass draw).
        // A mesh instance's override block is keyed on itself and its own version.
        const void* uniformKey = nullptr;
        uint64_t uniformVersion = boundMaterial ? boundMaterial->uniformsVersion() : 0;
        if (!(quadRenderActive() && !quadUniformData().empty())) {
            uniformKey = boundMaterial ? static_cast<const void*>(boundMaterial)
                : MetalUniformBinder::sharedDefaultBlockKey();
            if (boundMaterial && uniforms.data == instanceMaterialUniforms()) {
                uniformKey = instanceMaterialUniforms();
                uniformVersion = instanceMaterialUniformsVersion();
            }
        }
        _uniformBinder.submitPerDrawUniforms(passEncoder, _uniformRing.get(),
            uniformKey, uniformVersion, uniforms.data, uniforms.size, hdrPass());
    }

    void MetalGraphicsDevice::bindDrawSampler(MTL::RenderCommandEncoder* passEncoder)
    {
        // A quad pass is a screen-space post pass and needs the post sampler:
        // linear (mips included), clamp-to-edge, no aniso. The scene sampler REPEATS, so a
        // kernel that taps past [0,1] wraps to the opposite edge — visible as
        // wrong pixels along the frame border (CAS in compose does exactly this).
        // So every quad pass (bloom downsample and outline included) gets it at slot 0, the
        // one slot a hand-written quad shader declares, and the Slang programs' slots below.
        //
        // Every other draw gets the default sampler at slot 0; its material's maps come with
        // their samplers from the bindless table.
        const bool quad = quadRenderActive();
        _textureBinder.bindDrawSamplers(passEncoder, quad ? _postSampler : _defaultSampler);
        if (!quad) {
            // The forward program's scene samplers (forward-fragment-head.slang): linear clamp
            // with linear mips at 7 and for the env atlas at 10, nearest clamp at 8, the
            // shadow comparison at 9 — the states Vulkan binds at set 3, 12 / 13.
            _textureBinder.bindSamplerCached(passEncoder, 7, _postSampler);
            _textureBinder.bindSamplerCached(passEncoder, 8, _quadPointSampler);
            _textureBinder.bindSamplerCached(passEncoder, 9, _quadCompareSampler);
            _textureBinder.bindSamplerCached(passEncoder, 10, _postSampler);
        }
        if (quad) {
            // Slang quad programs read these (bindings.slang): the nearest and comparison
            // samplers at 1 and 2, and each input's own slot in the two per-input ranges.
            _textureBinder.bindSamplerCached(passEncoder, 1, _quadPointSampler);
            _textureBinder.bindSamplerCached(passEncoder, 2, _quadCompareSampler);
            for (int n = 0; n < MetalTextureBinder::kQuadInputCount; ++n) {
                Texture* input = quadTextureBinding(static_cast<size_t>(n));
                MTL::SamplerState* sampler = input && quadTextureOwnSampler(static_cast<size_t>(n))
                    ? _samplerCache.get(textureSamplerState(*input, maxAnisotropy())) : _postSampler;
                _textureBinder.bindSamplerCached(passEncoder, MetalTextureBinder::kQuadLinearSamplerBase + n,
                    sampler ? sampler : _postSampler);
            }
            for (int n = 0; n < MetalTextureBinder::kQuadPointInputs; ++n) {
                _textureBinder.bindSamplerCached(passEncoder, MetalTextureBinder::kQuadPointSamplerBase + n, _quadPointSampler);
            }
        }

        // After the first draw in a pass has established all texture/sampler state,
        // subsequent draws can rely on the cache for deduplication.
        _textureBinder.markClean();
    }

    void MetalGraphicsDevice::applyDepthStencilState(MTL::RenderCommandEncoder* passEncoder)
    {
        // Depth/stencil state is dynamic Metal encoder state. The exact
        // depth-test/depth-write combination is RESOLVED on every draw, so state from a
        // prior draw or a specialized pass cannot leak into this one, and issued when it
        // differs from what the encoder holds.
        MTL::DepthStencilState* drawDepthState = _passHasDepth
            ? resolveDepthStencilState(_depthState.get(), _stencilEnabled ? _stencilFront.get() : nullptr,
                  _stencilEnabled ? _stencilBack.get() : nullptr)
            : _noTestNoWriteDepthStencilState;
        if (drawDepthState && drawDepthState != _encoderDepthStencilState) {
            passEncoder->setDepthStencilState(drawDepthState);
            _encoderDepthStencilState = drawDepthState;
        }
        if (_stencilEnabled && (_stencilFront || _stencilBack)) {
            const auto* front = _stencilFront ? _stencilFront.get() : _stencilBack.get();
            const auto* back = _stencilBack ? _stencilBack.get() : _stencilFront.get();
            const uint32_t frontReference = front->reference();
            const uint32_t backReference = back->reference();
            if (!_encoderStencilReferenceValid || frontReference != _encoderStencilReferenceFront ||
                backReference != _encoderStencilReferenceBack) {
                passEncoder->setStencilReferenceValues(frontReference, backReference);
                _encoderStencilReferenceFront = frontReference;
                _encoderStencilReferenceBack = backReference;
                _encoderStencilReferenceValid = true;
            }
        }
    }

    void MetalGraphicsDevice::bindPendingDrawResources(MTL::RenderCommandEncoder* passEncoder)
    {
        // Each binding is one-shot: consumed by this draw and cleared.

        // Dynamic batch / skinning palette (slot 6). The buffer itself is bound once
        // per render pass in startRenderPass(); here only the offset changes.
        if (_pendingPaletteOffset != SIZE_MAX) {
            passEncoder->setVertexBufferOffset(_pendingPaletteOffset, 6);
            _pendingPaletteOffset = SIZE_MAX;
        }

        // Morph targets (slots 9/10). Delta buffer is static per Morph; params are
        // small per-draw bytes.
        if (_pendingMorphDeltaBuffer) {
            passEncoder->setVertexBuffer(_pendingMorphDeltaBuffer, 0, 9);
            passEncoder->setVertexBytes(_pendingMorphParams.data(), _pendingMorphParamsSize, 10);
            _pendingMorphDeltaBuffer = nullptr;
            _pendingMorphParamsSize = 0;
        }

        // Particle emitter (slots 7/8/9/11). The order and mesh slots always carry a buffer
        // (the pool when the emitter has none): the shader declares them, and reads them
        // only when its params say so. The params reach the fragment stage too (lighting,
        // softening).
        if (_pendingParticleBuffer) {
            passEncoder->setVertexBuffer(_pendingParticleBuffer, 0, 7);
            passEncoder->setVertexBuffer(_pendingParticleOrderBuffer ? _pendingParticleOrderBuffer
                                                                     : _pendingParticleBuffer, 0, 8);
            passEncoder->setVertexBuffer(_pendingParticleMeshBuffer ? _pendingParticleMeshBuffer
                                                                    : _pendingParticleBuffer, 0, 9);
            passEncoder->setVertexBytes(_pendingParticleParams.data(), _pendingParticleParamsSize, 11);
            passEncoder->setFragmentBytes(_pendingParticleParams.data(), _pendingParticleParamsSize, 11);
            _pendingParticleBuffer = nullptr;
            _pendingParticleOrderBuffer = nullptr;
            _pendingParticleMeshBuffer = nullptr;
            _pendingParticleParamsSize = 0;
        }

        // Gaussian splats (slots 7/8/12/11).
        if (_pendingGSplatBuffer && _pendingGSplatOrderBuffer) {
            passEncoder->setVertexBuffer(_pendingGSplatBuffer, 0, 7);
            passEncoder->setVertexBuffer(_pendingGSplatOrderBuffer, 0, 8);
            if (_pendingGSplatShBuffer) {
                passEncoder->setVertexBuffer(_pendingGSplatShBuffer, 0, 12);
            }
            passEncoder->setVertexBytes(_pendingGSplatParams.data(), _pendingGSplatParamsSize, 11);
            _pendingGSplatBuffer = nullptr;
            _pendingGSplatOrderBuffer = nullptr;
            _pendingGSplatShBuffer = nullptr;
            _pendingGSplatParamsSize = 0;
        }
    }

    void MetalGraphicsDevice::encodeDraw(MTL::RenderCommandEncoder* passEncoder, const Primitive& primitive,
        const IndexBinding& index, const int numInstances, const int indirectSlot)
    {
        const auto primitiveType = toMetalPrimitiveType(primitive.type);

        if (indirectSlot >= 0 && _indirectDrawBuffer) {
            // GPU-driven indirect draw: instance count comes from the GPU.
            const auto indirectOffset = static_cast<NS::UInteger>(indirectSlot * INDIRECT_ENTRY_BYTE_SIZE);
            if (index.buffer) {
                passEncoder->drawIndexedPrimitives(
                    primitiveType, index.type, index.buffer, 0,
                    _indirectDrawBuffer, indirectOffset);
            } else {
                passEncoder->drawPrimitives(
                    primitiveType, _indirectDrawBuffer, indirectOffset);
            }
            _indirectDrawBuffer = nullptr;  // Consumed
            return;
        }

        if (index.buffer) {
            const auto indexElementSize = (index.type == MTL::IndexTypeUInt32) ? 4 : 2;
            const auto indexBufferOffset = static_cast<NS::UInteger>(primitive.base * indexElementSize);
            passEncoder->drawIndexedPrimitives(
                primitiveType,
                static_cast<NS::UInteger>(primitive.count),
                index.type,
                index.buffer,
                indexBufferOffset,
                static_cast<NS::UInteger>(numInstances),
                static_cast<NS::Integer>(primitive.baseVertex),
                static_cast<NS::UInteger>(primitive.firstInstance)
            );
        } else {
            passEncoder->drawPrimitives(
                primitiveType,
                static_cast<NS::UInteger>(primitive.base),
                static_cast<NS::UInteger>(primitive.count),
                static_cast<NS::UInteger>(numInstances),
                static_cast<NS::UInteger>(primitive.firstInstance)
            );
        }
    }
}
