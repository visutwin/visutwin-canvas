// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 27.08.2025.
//
#include "metalRenderPipeline.h"

#include "metalRenderTarget.h"
#include "metalShader.h"
#include "core/utils.h"
#include "spdlog/spdlog.h"
#include "Foundation/NSBundle.hpp"

namespace visutwin::canvas
{
    static int _pipelineId = 0;

    // Primitive topology mapping
    const MTL::PrimitiveType MetalRenderPipeline::primitiveTopology[5] = {
        MTL::PrimitiveTypePoint,            // PRIMITIVE_POINTS
        MTL::PrimitiveTypeLine,             // PRIMITIVE_LINES
        MTL::PrimitiveTypeLineStrip,        // PRIMITIVE_LINESTRIP
        MTL::PrimitiveTypeTriangle,         // PRIMITIVE_TRIANGLES
        MTL::PrimitiveTypeTriangleStrip,    // PRIMITIVE_TRISTRIP
    };

    // Blend operation mapping
    const MTL::BlendOperation MetalRenderPipeline::blendOperation[5] = {
        MTL::BlendOperationAdd,              // BLENDEQUATION_ADD
        MTL::BlendOperationSubtract,         // BLENDEQUATION_SUBTRACT
        MTL::BlendOperationReverseSubtract,  // BLENDEQUATION_REVERSE_SUBTRACT
        MTL::BlendOperationMin,              // BLENDEQUATION_MIN
        MTL::BlendOperationMax               // BLENDEQUATION_MAX
    };

    // Blend factor mapping
    const MTL::BlendFactor MetalRenderPipeline::blendFactor[13] = {
        MTL::BlendFactorZero,                       // BLENDMODE_ZERO
        MTL::BlendFactorOne,                        // BLENDMODE_ONE
        MTL::BlendFactorSourceColor,                // BLENDMODE_SRC_COLOR
        MTL::BlendFactorOneMinusSourceColor,        // BLENDMODE_ONE_MINUS_SRC_COLOR
        MTL::BlendFactorDestinationColor,           // BLENDMODE_DST_COLOR
        MTL::BlendFactorOneMinusDestinationColor,   // BLENDMODE_ONE_MINUS_DST_COLOR
        MTL::BlendFactorSourceAlpha,                // BLENDMODE_SRC_ALPHA
        MTL::BlendFactorSourceAlphaSaturated,       // BLENDMODE_SRC_ALPHA_SATURATE
        MTL::BlendFactorOneMinusSourceAlpha,        // BLENDMODE_ONE_MINUS_SRC_ALPHA
        MTL::BlendFactorDestinationAlpha,            // BLENDMODE_DST_ALPHA
        MTL::BlendFactorOneMinusDestinationAlpha,   // BLENDMODE_ONE_MINUS_DST_ALPHA
        MTL::BlendFactorBlendColor,                 // BLENDMODE_CONSTANT
        MTL::BlendFactorOneMinusBlendColor          // BLENDMODE_ONE_MINUS_CONSTANT
    };

    MetalRenderPipeline::MetalRenderPipeline(const MetalGraphicsDevice* device): MetalPipeline(device)
    {
        _lookupHashes.resize(15, 0);
        _vertexBufferLayout = std::make_unique<MetalVertexBufferLayout>();
        _pipeline = nullptr;
    }

    MetalRenderPipeline::~MetalRenderPipeline()
    {
        if (_pipeline) {
            _pipeline->release();
            _pipeline = nullptr;
        }

        for (const auto& [hash, entries] : _cache) {
            for (const auto& entry : entries) {
                if (entry && entry->pipeline) {
                    entry->pipeline->release();
                }
            }
        }
    }

    /*MTL::VertexDescriptor* buildVertexDescriptor()
    {
        MTL::VertexDescriptor* vertexDescriptor = MTL::VertexDescriptor::alloc()->init();

        // Position
        vertexDescriptor->attributes()->object(0)->setFormat(MTL::VertexFormat::VertexFormatFloat3);
        vertexDescriptor->attributes()->object(0)->setOffset(0);
        vertexDescriptor->attributes()->object(0)->setBufferIndex(0);

        // Normal
        vertexDescriptor->attributes()->object(1)->setFormat(MTL::VertexFormat::VertexFormatFloat3);
        vertexDescriptor->attributes()->object(1)->setOffset(3 * sizeof(float));
        vertexDescriptor->attributes()->object(1)->setBufferIndex(0);

        // Texcoord
        vertexDescriptor->attributes()->object(2)->setFormat(MTL::VertexFormat::VertexFormatFloat2);
        vertexDescriptor->attributes()->object(2)->setOffset(6 * sizeof(float));
        vertexDescriptor->attributes()->object(2)->setBufferIndex(0);

        vertexDescriptor->layouts()->object(0)->setStride(8 * sizeof(float));
        //vertexDescriptor->layouts()->object(0)->setStepFunction(MTL::VertexStepFunction::VertexStepFunctionPerVertex);

        return vertexDescriptor;
    }

    MetalRenderPipeline::MetalRenderPipeline(const MetalGraphicsDevice* device)
    {
        const NS::Bundle* bundle = NS::Bundle::mainBundle();
        if (!bundle)
        {
            spdlog::error("Failed to load main bundle");
            assert(false);
        }

        const std::string path = std::string(bundle->resourceURL()->fileSystemRepresentation()) + "/" + descriptor.vertexShader.source;
        spdlog::info("Loading shader library from {}", path);

        auto* mtlDevice = device.raw();

        NS::Error* error = nullptr;
        MTL::Library* library = mtlDevice->newLibrary(NS::String::string(path.c_str(), NS::UTF8StringEncoding), &error);
        if (!library)
        {
            spdlog::error("Failed to load shader library: {}", error->localizedDescription()->utf8String());
            assert(false);
        }

        const MTL::Function* vertexShader = library->newFunction(NS::String::string(descriptor.vertexShader.entryPoint.c_str(), NS::UTF8StringEncoding));
        assert(vertexShader);

        const MTL::Function* fragmentShader = library->newFunction(NS::String::string(descriptor.fragmentShader.entryPoint.c_str(), NS::UTF8StringEncoding));
        assert(fragmentShader);

        const MTL::VertexDescriptor* vertexDescriptor = buildVertexDescriptor();

        MTL::RenderPipelineDescriptor* pipelineDescriptor = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDescriptor->setVertexFunction(vertexShader);
        pipelineDescriptor->setVertexDescriptor(vertexDescriptor);
        pipelineDescriptor->setFragmentFunction(fragmentShader);
        //pipelineDescriptor->setRasterizationEnabled(false);

        const MTL::PixelFormat pixelFormat = device.getSwapChain()->raw()->pixelFormat();
        pipelineDescriptor->colorAttachments()->object(0)->setPixelFormat(pixelFormat);

        pipeline = mtlDevice->newRenderPipelineState(pipelineDescriptor, &error);
        if (!pipeline)
        {
            spdlog::error("Failed to create render pipeline state: {}", error->localizedDescription()->utf8String());
            assert(false);
        }

        pipelineDescriptor->release();
    }*/

    MTL::RenderPipelineState* MetalRenderPipeline::get(const Primitive& primitive, const std::shared_ptr<VertexFormat>& vertexFormat0,
            const std::shared_ptr<VertexFormat>& vertexFormat1, int ibFormat, const std::shared_ptr<Shader>& shader,
            const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            const std::shared_ptr<BlendState>& blendState, const std::shared_ptr<DepthState>& depthState,
            CullMode cullMode, bool stencilEnabled,
            const std::shared_ptr<StencilParameters>& stencilFront, const std::shared_ptr<StencilParameters>& stencilBack,
            const std::shared_ptr<VertexFormat>& instancingFormat) {
        assert(bindGroupFormats.size() <= 3);
        assert(shader != nullptr);
        assert(blendState != nullptr);
        assert(depthState != nullptr);

        // ibFormat is used only for stripped primitives, clear it otherwise to avoid additional render pipelines
        int primitiveType = primitive.type;
        if (primitiveType < 0 || primitiveType >= 5) {
            primitiveType = PRIMITIVE_TRIANGLES;
        }
        if (ibFormat != -1 && primitiveType != PRIMITIVE_LINESTRIP && primitiveType != PRIMITIVE_TRISTRIP) {
            ibFormat = -1;
        }

        // Render pipeline unique hash
        _lookupHashes[0] = primitiveType;
        _lookupHashes[1] = shader->id();
        _lookupHashes[2] = static_cast<int>(cullMode);
        _lookupHashes[3] = depthState->key();
        _lookupHashes[4] = blendState->key();
        _lookupHashes[5] = vertexFormat0 ? vertexFormat0->renderingHash() : 0;
        _lookupHashes[6] = vertexFormat1 ? vertexFormat1->renderingHash() : 0;
        _lookupHashes[7] = renderTarget ? renderTarget->key() : 0;
        _lookupHashes[8] = bindGroupFormats.size() > 0 && bindGroupFormats[0] ? bindGroupFormats[0]->key() : 0;
        _lookupHashes[9] = bindGroupFormats.size() > 1 && bindGroupFormats[1] ? bindGroupFormats[1]->key() : 0;
        _lookupHashes[10] = bindGroupFormats.size() > 2 && bindGroupFormats[2] ? bindGroupFormats[2]->key() : 0;
        _lookupHashes[11] = stencilEnabled ? stencilFront->key() : 0;
        _lookupHashes[12] = stencilEnabled ? stencilBack->key() : 0;
        _lookupHashes[13] = ibFormat != -1 ? ibFormat : 0;
        _lookupHashes[14] = (instancingFormat && instancingFormat->isInstancing())
            ? instancingFormat->renderingHash() : 0;

        uint32_t hash = hash32Fnv1a(_lookupHashes.data(), _lookupHashes.size());

        // Check cached pipeline
        auto it = _cache.find(hash);
        if (it != _cache.end()) {
            auto& cacheEntries = it->second;
            // Find an exact match in case of hash collision
            for (auto& entry : cacheEntries) {
                if (std::equal(entry->hashes.begin(), entry->hashes.end(), _lookupHashes.begin())) {
                    return entry->pipeline;
                }
            }
        }

        // No match or hash collision, create a new pipeline
        const MTL::PrimitiveType primTopology = primitiveTopology[primitiveType];

        // Pipeline layout
        metal::PipelineLayout* pipelineLayout = getPipelineLayout(bindGroupFormats);

        // Vertex buffer layout
        auto vbLayout = _vertexBufferLayout->get(vertexFormat0, vertexFormat1);

        // Derive vertex stride from format (defaults to 56 for standard 14-float layout)
        const int vbStride = vertexFormat0 ? vertexFormat0->size() : 14 * static_cast<int>(sizeof(float));

        // Instancing stride: 0 means no instancing layout in the vertex descriptor.
        const int instStride = (instancingFormat && instancingFormat->isInstancing())
            ? instancingFormat->size() : 0;

        // Create a pipeline
        auto cacheEntry = std::make_shared<CacheEntry>();
        cacheEntry->hashes = _lookupHashes;
        cacheEntry->pipeline = create(
            primTopology, ibFormat, shader, renderTarget, pipelineLayout,
            blendState, depthState, vbLayout, cullMode,
            stencilEnabled, stencilFront, stencilBack,
            vbStride, instStride
        );
        if (!cacheEntry->pipeline) {
            spdlog::error("Render pipeline creation returned null");
            return nullptr;
        }

        // Add to cache
        if (it != _cache.end()) {
            it->second.push_back(cacheEntry);
        } else {
            _cache[hash] = { cacheEntry };
        }

        return cacheEntry->pipeline;
    }

    MTL::RenderPipelineState* MetalRenderPipeline::create(const MTL::PrimitiveType primitiveTopology, int ibFormat,
            const std::shared_ptr<Shader>& shader, const std::shared_ptr<RenderTarget>& renderTarget,
            metal::PipelineLayout* pipelineLayout, std::shared_ptr<BlendState> blendState,
            std::shared_ptr<DepthState> depthState, const std::vector<void*>& vertexBufferLayout,
            CullMode cullMode, bool stencilEnabled, std::shared_ptr<StencilParameters> stencilFront,
            std::shared_ptr<StencilParameters> stencilBack,
            int vertexStride,
            int instancingStride
        )
    {
        auto* mtlDevice = _device->raw();
        spdlog::trace("Creating Metal render pipeline");

        MTL::RenderPipelineDescriptor* pipelineDescriptor = MTL::RenderPipelineDescriptor::alloc()->init();

        NS::Error* error = nullptr;
        MTL::Library* library = nullptr;
        const NS::Bundle* bundle = NS::Bundle::mainBundle();
        if (auto* metalShader = dynamic_cast<MetalShader*>(shader.get())) {
            library = metalShader->getLibrary(mtlDevice, bundle, &error);
            if (library) {
                library->retain();
            }
        } else {
            spdlog::error("Unsupported shader implementation for MetalRenderPipeline. Expected MetalShader.");
            pipelineDescriptor->release();
            return nullptr;
        }

        if (!library) {
            spdlog::error("Failed to load Metal library: {}", error ? error->localizedDescription()->utf8String() : "unknown");
            assert(false);
            pipelineDescriptor->release();
            return nullptr;
        }

        const auto& vertexEntry = shader->vertexEntry().empty() ? std::string("vertexShader") : shader->vertexEntry();
        const auto& fragmentEntry = shader->fragmentEntry().empty() ? std::string("fragmentShader") : shader->fragmentEntry();
        auto* vertexFunction = library->newFunction(NS::String::string(vertexEntry.c_str(), NS::UTF8StringEncoding));
        auto* fragmentFunction = library->newFunction(NS::String::string(fragmentEntry.c_str(), NS::UTF8StringEncoding));
        if (!vertexFunction || !fragmentFunction) {
            spdlog::error("Failed to find required shader entry points ({} / {})", vertexEntry, fragmentEntry);
            if (vertexFunction) vertexFunction->release();
            if (fragmentFunction) fragmentFunction->release();
            library->release();
            pipelineDescriptor->release();
            assert(false);
            return nullptr;
        }

        pipelineDescriptor->setVertexFunction(vertexFunction);
        pipelineDescriptor->setFragmentFunction(fragmentFunction);

        if (const auto metalTarget = std::dynamic_pointer_cast<MetalRenderTarget>(renderTarget))
        {
            const auto colorAttachments = metalTarget->colorAttachments();

            MTL::ColorWriteMask writeMask = MTL::ColorWriteMaskNone;
            if (blendState->redWrite())   writeMask |= MTL::ColorWriteMaskRed;
            if (blendState->greenWrite()) writeMask |= MTL::ColorWriteMaskGreen;
            if (blendState->blueWrite())  writeMask |= MTL::ColorWriteMaskBlue;
            if (blendState->alphaWrite()) writeMask |= MTL::ColorWriteMaskAlpha;

            for (auto i = 0; i < colorAttachments.size(); ++i)
            {
                auto* colorAttachment = pipelineDescriptor->colorAttachments()->object(i);
                colorAttachment->setPixelFormat(colorAttachments[i]->pixelFormat);
                colorAttachment->setWriteMask(writeMask);

                setBlend(colorAttachment, blendState);
            }

            // Set depth/stencil pixel format from the render target's depth attachment
            if (const auto& depthAtt = metalTarget->depthAttachment()) {
                pipelineDescriptor->setDepthAttachmentPixelFormat(depthAtt->pixelFormat);
                if (depthAtt->hasStencil) {
                    pipelineDescriptor->setStencilAttachmentPixelFormat(depthAtt->pixelFormat);
                }
            }
        } else {
            // Back buffer: BGRA8 color + Depth32Float (always attached by startRenderPass)
            auto* colorAttachment = pipelineDescriptor->colorAttachments()->object(0);
            colorAttachment->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
            setBlend(colorAttachment, blendState);
            pipelineDescriptor->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
        }

        _pipelineId++;
        const std::string label = "RenderPipeline-" + std::to_string(_pipelineId);
        pipelineDescriptor->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));

        MTL::VertexDescriptor* vertexDescriptor = MTL::VertexDescriptor::vertexDescriptor();
        auto* attributes = vertexDescriptor->attributes();
        auto* layouts = vertexDescriptor->layouts();

        // Forward shader input contract (interleaved):
        // attribute(0)=position(float3), attribute(1)=normal(float3),
        // attribute(2)=uv0(float2), attribute(3)=tangent(float4), attribute(4)=uv1(float2).
        // When vertex colors are enabled (stride == 72):
        // attribute(5)=color(float4) follows uv1.
        // Point cloud vertices (stride == 28):
        // attribute(0)=position(float3)@0, attribute(5)=color(float4)@12.
        // Unused attributes map to offset 0 (read harmless data from position).
        constexpr int STRIDE_POINT_VERTEX = 7 * static_cast<int>(sizeof(float));   // 28
        constexpr int STRIDE_DYNAMIC_BATCH = 15 * static_cast<int>(sizeof(float)); // 60
        constexpr int STRIDE_WITH_COLOR = 18 * static_cast<int>(sizeof(float));    // 72

        if (vertexStride <= STRIDE_POINT_VERTEX) {
            // Point cloud vertex: position(float3)@0 + color(float4)@12.
            // Unused attributes (normal, uv0, tangent, uv1) point to offset 0,
            // reading harmless data from position. The unlit shader ignores them.
            attributes->object(0)->setFormat(MTL::VertexFormatFloat3);
            attributes->object(0)->setOffset(0);
            attributes->object(0)->setBufferIndex(0);

            attributes->object(1)->setFormat(MTL::VertexFormatFloat3);
            attributes->object(1)->setOffset(0);  // dummy: reads position
            attributes->object(1)->setBufferIndex(0);

            attributes->object(2)->setFormat(MTL::VertexFormatFloat2);
            attributes->object(2)->setOffset(0);  // dummy
            attributes->object(2)->setBufferIndex(0);

            attributes->object(3)->setFormat(MTL::VertexFormatFloat4);
            attributes->object(3)->setOffset(0);  // dummy
            attributes->object(3)->setBufferIndex(0);

            attributes->object(4)->setFormat(MTL::VertexFormatFloat2);
            attributes->object(4)->setOffset(0);  // dummy
            attributes->object(4)->setBufferIndex(0);

            attributes->object(5)->setFormat(MTL::VertexFormatFloat4);
            attributes->object(5)->setOffset(3 * static_cast<NS::UInteger>(sizeof(float)));  // color at offset 12
            attributes->object(5)->setBufferIndex(0);
        } else {
            // Standard vertex layouts (56/60/72 bytes)
            attributes->object(0)->setFormat(MTL::VertexFormatFloat3);
            attributes->object(0)->setOffset(0);
            attributes->object(0)->setBufferIndex(0);

            attributes->object(1)->setFormat(MTL::VertexFormatFloat3);
            attributes->object(1)->setOffset(3 * static_cast<NS::UInteger>(sizeof(float)));
            attributes->object(1)->setBufferIndex(0);

            attributes->object(2)->setFormat(MTL::VertexFormatFloat2);
            attributes->object(2)->setOffset(6 * static_cast<NS::UInteger>(sizeof(float)));
            attributes->object(2)->setBufferIndex(0);

            attributes->object(3)->setFormat(MTL::VertexFormatFloat4);
            attributes->object(3)->setOffset(8 * static_cast<NS::UInteger>(sizeof(float)));
            attributes->object(3)->setBufferIndex(0);

            attributes->object(4)->setFormat(MTL::VertexFormatFloat2);
            attributes->object(4)->setOffset(12 * static_cast<NS::UInteger>(sizeof(float)));
            attributes->object(4)->setBufferIndex(0);

            // Dynamic batching: attribute(5) as Float1 (bone index) at offset 56 when stride is 60 bytes.
            // Vertex colors: attribute(5) as Float4 at offset 56 when stride is 72 bytes.
            // These are mutually exclusive (both use attribute 5).
            if (vertexStride >= STRIDE_WITH_COLOR) {
                attributes->object(5)->setFormat(MTL::VertexFormatFloat4);
                attributes->object(5)->setOffset(14 * static_cast<NS::UInteger>(sizeof(float)));
                attributes->object(5)->setBufferIndex(0);
            } else if (vertexStride >= STRIDE_DYNAMIC_BATCH) {
                attributes->object(5)->setFormat(MTL::VertexFormatFloat);
                attributes->object(5)->setOffset(14 * static_cast<NS::UInteger>(sizeof(float)));
                attributes->object(5)->setBufferIndex(0);
            }
        }

        layouts->object(0)->setStride(static_cast<NS::UInteger>(vertexStride));
        layouts->object(0)->setStepFunction(MTL::VertexStepFunctionPerVertex);
        layouts->object(0)->setStepRate(1);

        // Hardware instancing: set up vertex descriptor layout(5) with perInstance step function.
        // instance_line1..4 (4x float4 for model matrix) + instanceColor (float4).
        // These map to [[attribute(6)]]..[[attribute(10)]] in the shader's VertexData struct.
        if (instancingStride > 0) {
            constexpr NS::UInteger INST_BUFFER_INDEX = 5;
            constexpr NS::UInteger INST_ATTR_BASE = 6;

            for (NS::UInteger i = 0; i < 5; ++i) {
                attributes->object(INST_ATTR_BASE + i)->setFormat(MTL::VertexFormatFloat4);
                attributes->object(INST_ATTR_BASE + i)->setOffset(i * 4 * sizeof(float));
                attributes->object(INST_ATTR_BASE + i)->setBufferIndex(INST_BUFFER_INDEX);
            }

            layouts->object(INST_BUFFER_INDEX)->setStride(static_cast<NS::UInteger>(instancingStride));
            layouts->object(INST_BUFFER_INDEX)->setStepFunction(MTL::VertexStepFunctionPerInstance);
            layouts->object(INST_BUFFER_INDEX)->setStepRate(1);
        }

        pipelineDescriptor->setVertexDescriptor(vertexDescriptor);

        auto* pipeline = mtlDevice->newRenderPipelineState(pipelineDescriptor, &error);
        if (!pipeline)
        {
            spdlog::error("Failed to create render pipeline state: {}", error->localizedDescription()->utf8String());
            assert(false);
        }

        spdlog::trace("RenderPipelineAlloc | Alloc: Id " + std::to_string(_pipelineId));

        vertexFunction->release();
        fragmentFunction->release();
        library->release();
        pipelineDescriptor->release();

        return pipeline;
    }

    void MetalRenderPipeline::setBlend(MTL::RenderPipelineColorAttachmentDescriptor* colorAttachment,
        const std::shared_ptr<BlendState>& blendState)
    {
        if (!blendState->enabled()) {
            return;
        }

        colorAttachment->setBlendingEnabled(true);

        colorAttachment->setRgbBlendOperation(blendOperation[blendState->colorOp()]);
        colorAttachment->setSourceRGBBlendFactor(blendFactor[blendState->colorSrcFactor()]);
        colorAttachment->setDestinationRGBBlendFactor(blendFactor[blendState->colorDstFactor()]);

        colorAttachment->setAlphaBlendOperation(blendOperation[blendState->alphaOp()]);
        colorAttachment->setSourceAlphaBlendFactor(blendFactor[blendState->alphaSrcFactor()]);
        colorAttachment->setDestinationAlphaBlendFactor(blendFactor[blendState->alphaDstFactor()]);
    }
}
