// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "envBake.h"

#include <algorithm>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "core/math/color.h"
#include "envShaders.h"
#include "quadRender.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        // RenderPass takes an owning device handle and keeps its name protected, so
        // the bakes get a tiny subclass. The handle is deliberately NON-OWNING: the
        // pass lives entirely inside one function call and the device outlives it by
        // construction, so a null deleter is correct here and avoids threading a
        // shared_ptr through an API that only ever had a raw one.
        struct EnvBakePass final : RenderPass
        {
            EnvBakePass(GraphicsDevice* device, const char* name)
                : RenderPass(std::shared_ptr<GraphicsDevice>(device, [](GraphicsDevice*) {}))
            {
                _name = name;
            }
        };


        // GLSL needs its #version line first, so the variant switch is spliced in
        // after it; MSL has no such rule and takes a plain prefix.
        std::string composeSource(GraphicsDevice* device, const char* body,
            const bool cubeSource)
        {
            const bool glsl = device->shaderLanguage() == ShaderLanguage::Glsl;
            std::string define = cubeSource ? "#define SRC_CUBE 1\n" : "";
            return glsl ? ("#version 450\n" + define + body) : (define + body);
        }

        std::shared_ptr<Shader> reprojectShader(GraphicsDevice* device, const bool cubeSource)
        {
            const char* cacheKey = cubeSource ? "env-reproject-cube-quad"
                                              : "env-reproject-2d-quad";
            if (auto cached = device->getCachedShader(cacheKey)) {
                return cached;
            }
            ShaderDefinition definition;
            definition.name = cacheKey;
            definition.vshader = "reprojectVertex";
            definition.fshader = "reprojectFragment";
            auto shader = createShader(device, definition,
                composeSource(device,
                    device->shaderLanguage() == ShaderLanguage::Glsl
                        ? env_shaders::REPROJECT_GLSL : env_shaders::REPROJECT_MSL,
                    cubeSource));
            if (shader) {
                device->setCachedShader(cacheKey, shader);
            }
            return shader;
        }


        // The seam expansion both bakes share (see EnvBakeRect::seamPixels).
        void fillUvMod(float (&uvMod)[4], const EnvBakeRect& rect)
        {
            const int seam = std::max(0, rect.seamPixels);
            const int innerWidth = rect.width - seam * 2;
            const int innerHeight = rect.height - seam * 2;
            if (seam > 0 && innerWidth > 0 && innerHeight > 0) {
                uvMod[0] = static_cast<float>(rect.width) / static_cast<float>(innerWidth);
                uvMod[1] = static_cast<float>(rect.height) / static_cast<float>(innerHeight);
                uvMod[2] = -static_cast<float>(seam) / static_cast<float>(innerWidth);
                uvMod[3] = -static_cast<float>(seam) / static_cast<float>(innerHeight);
            }
        }


        // A bake draws outside the frame graph, so nothing else has set the render
        // state the pipeline cache needs. Defaults are right for every bake: opaque
        // writes, no depth test or write, no culling of a fullscreen triangle.
        void setBakeRenderState(GraphicsDevice* device)
        {
            static const auto blend = std::make_shared<BlendState>();
            static const auto depth = [] {
                auto state = std::make_shared<DepthState>();
                state->setDepthTest(false);
                state->setDepthWrite(false);
                return state;
            }();
            device->setBlendState(blend);
            device->setDepthState(depth);
            device->setCullMode(CullMode::CULLFACE_NONE);
            device->setStencilState();
        }


        std::shared_ptr<Shader> convolveShader(GraphicsDevice* device, const bool cubeSource)
        {
            const char* cacheKey = cubeSource ? "env-convolve-cube-quad"
                                              : "env-convolve-2d-quad";
            if (auto cached = device->getCachedShader(cacheKey)) {
                return cached;
            }
            ShaderDefinition definition;
            definition.name = cacheKey;
            definition.vshader = "convolveVertex";
            definition.fshader = "convolveFragment";
            auto shader = createShader(device, definition,
                composeSource(device,
                    device->shaderLanguage() == ShaderLanguage::Glsl
                        ? env_shaders::CONVOLVE_GLSL : env_shaders::CONVOLVE_MSL,
                    cubeSource));
            if (shader) {
                device->setCachedShader(cacheKey, shader);
            }
            return shader;
        }

        // The sample table as a 1-row RGBA32F texture. Built and uploaded BEFORE the
        // offline scope opens: that scope flushes pending uploads on entry, and
        // nothing may be uploaded once it is recording.
        std::shared_ptr<Texture> makeSampleTable(GraphicsDevice* device,
            const float* samples, const int numSamples)
        {
            if (!samples || numSamples <= 0) {
                return nullptr;
            }
            TextureOptions options;
            options.name = "envConvolveSamples";
            options.width = static_cast<uint32_t>(numSamples);
            options.height = 1;
            options.format = PixelFormat::PIXELFORMAT_RGBA32F;
            options.mipmaps = false;
            options.minFilter = FilterMode::FILTER_NEAREST;
            options.magFilter = FilterMode::FILTER_NEAREST;
            auto table = std::make_shared<Texture>(device, options);
            table->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            table->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            table->setLevelData(0, reinterpret_cast<const uint8_t*>(samples),
                static_cast<size_t>(numSamples) * 4u * sizeof(float));
            table->upload();
            return table;
        }

        Vector4 rectViewport(const EnvBakeRect& rect)
        {
            return Vector4(static_cast<float>(rect.x), static_cast<float>(rect.y),
                static_cast<float>(rect.width), static_cast<float>(rect.height));
        }

        // Shared body of the convolve draws, used alone and inside the atlas.
        void drawConvolveRects(QuadRender& quad,
            const std::vector<EnvConvolveBakeRect>& rects,
            const std::vector<std::shared_ptr<Texture>>& tables,
            const bool encodeRgbp, const bool decodeSrgb)
        {
            for (size_t i = 0; i < rects.size(); ++i) {
                const auto& entry = rects[i];
                if (entry.rect.width <= 0 || entry.rect.height <= 0 || !tables[i]) {
                    continue;
                }
                env_shaders::ConvolveUniforms uniforms{};
                fillUvMod(uniforms.uvMod, entry.rect);
                uniforms.encodeRgbp = encodeRgbp ? 1u : 0u;
                uniforms.decodeSrgb = decodeSrgb ? 1u : 0u;
                uniforms.numSamples = static_cast<uint32_t>(entry.numSamples);
                uniforms.weightByNoL = entry.weightByNoL ? 1u : 0u;
                quad.setUniforms(uniforms);
                quad.setTexture(1, tables[i].get());

                const Vector4 viewport = rectViewport(entry.rect);
                quad.render(&viewport);
            }
        }

        // One shader per device, cached like every other quad effect's.
        std::shared_ptr<Shader> equirectToCubeShader(GraphicsDevice* device)
        {
            constexpr const char* cacheKey = "env-equirect-to-cube-quad";
            if (auto cached = device->getCachedShader(cacheKey)) {
                return cached;
            }
            ShaderDefinition definition;
            definition.name = cacheKey;
            definition.vshader = "equirectToCubeVertex";
            definition.fshader = "equirectToCubeFragment";
            auto shader = createShader(device, definition,
                device->shaderLanguage() == ShaderLanguage::Glsl
                    ? env_shaders::EQUIRECT_TO_CUBE_GLSL
                    : env_shaders::EQUIRECT_TO_CUBE_MSL);
            if (shader) {
                device->setCachedShader(cacheKey, shader);
            }
            return shader;
        }
    }

    bool bakeEquirectToCubemap(GraphicsDevice* device, Texture* source,
        Texture* target, const bool decodeSrgb)
    {
        if (!device || !source || !target) {
            spdlog::warn("bakeEquirectToCubemap: source or target is null");
            return false;
        }
        if (!target->isCubemap()) {
            spdlog::error("bakeEquirectToCubemap: target is not a cubemap");
            return false;
        }

        auto shader = equirectToCubeShader(device);
        if (!shader) {
            spdlog::error("bakeEquirectToCubemap: no shader for this device");
            return false;
        }

        source->upload();
        target->upload();

        QuadRender quad(shader);
        quad.setTexture(0, source);

        // All six faces in one offline scope so the backends can batch them into a
        // single command buffer rather than one per face.
        device->beginOfflineWork();
        for (uint32_t face = 0; face < 6u; ++face) {
            RenderTargetOptions targetOptions;
            targetOptions.graphicsDevice = device;
            targetOptions.colorBuffer = target;
            targetOptions.face = static_cast<int>(face);
            targetOptions.depth = false;
            targetOptions.samples = 1;
            targetOptions.flipY = false;
            targetOptions.name = "equirectToCubeFace";
            auto renderTarget = device->createRenderTarget(targetOptions);
            if (!renderTarget) {
                continue;
            }

            // Each face is its own storage slice, so the tile-memory preservation
            // rule that forces the atlas bake into a single pass does not apply.
            EnvBakePass pass(device, "EnvEquirectToCubeFace");
            pass.init(renderTarget);

            env_shaders::EquirectToCubeUniforms uniforms{};
            uniforms.face = face;
            uniforms.decodeSrgb = decodeSrgb ? 1u : 0u;
            quad.setUniforms(uniforms);

            device->startRenderPass(&pass);
            setBakeRenderState(device);
            quad.render();
            device->endRenderPass(&pass);
        }
        if (target->mipmaps() && target->getNumLevels() > 1) {
            device->generateMipmaps(target);
        }
        device->endOfflineWork();
        return true;
    }

    bool bakeReproject(GraphicsDevice* device, const EnvReprojectRequest& request)
    {
        if (!device || !request.source || !request.target || request.rects.empty()) {
            spdlog::warn("bakeReproject: source, target or rects missing");
            return false;
        }
        const bool cubeSource =
            request.sourceProjection == TextureProjection::TEXTUREPROJECTION_CUBE;
        if (cubeSource != request.source->isCubemap()) {
            spdlog::error("bakeReproject: source projection disagrees with the texture");
            return false;
        }

        auto shader = reprojectShader(device, cubeSource);
        if (!shader) {
            spdlog::error("bakeReproject: no shader for this device");
            return false;
        }

        request.source->upload();
        request.target->upload();

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device;
        targetOptions.colorBuffer = request.target;
        targetOptions.depth = false;
        targetOptions.samples = 1;
        targetOptions.flipY = false;
        targetOptions.name = "envReprojectTarget";
        auto renderTarget = device->createRenderTarget(targetOptions);
        if (!renderTarget) {
            return false;
        }

        QuadRender quad(shader);
        quad.setTexture(0, request.source);

        EnvBakePass pass(device, "EnvReproject");
        pass.init(renderTarget);

        device->beginOfflineWork();
        device->startRenderPass(&pass);
        setBakeRenderState(device);
        for (const auto& rect : request.rects) {
            if (rect.width <= 0 || rect.height <= 0) {
                continue;
            }
            env_shaders::ReprojectUniforms uniforms{};
            fillUvMod(uniforms.uvMod, rect);
            uniforms.sourceProjection = static_cast<uint32_t>(request.sourceProjection);
            uniforms.targetProjection = static_cast<uint32_t>(request.targetProjection);
            uniforms.encodeRgbp = request.encodeRgbp ? 1u : 0u;
            uniforms.decodeSrgb = request.decodeSrgb ? 1u : 0u;
            quad.setUniforms(uniforms);

            const Vector4 viewport = rectViewport(rect);
            quad.render(&viewport);
        }
        device->endRenderPass(&pass);
        device->endOfflineWork();
        return true;
    }

    bool bakeConvolve(GraphicsDevice* device, const EnvConvolveRequest& request)
    {
        if (!device || !request.source || !request.target || request.rects.empty()) {
            spdlog::warn("bakeConvolve: source, target or rects missing");
            return false;
        }
        auto shader = convolveShader(device, request.source->isCubemap());
        if (!shader) {
            spdlog::error("bakeConvolve: no shader for this device");
            return false;
        }
        request.source->upload();
        request.target->upload();

        std::vector<std::shared_ptr<Texture>> tables;
        tables.reserve(request.rects.size());
        for (const auto& entry : request.rects) {
            tables.push_back(makeSampleTable(device, entry.samples, entry.numSamples));
        }

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device;
        targetOptions.colorBuffer = request.target;
        targetOptions.depth = false;
        targetOptions.samples = 1;
        targetOptions.flipY = false;
        targetOptions.name = "envConvolveTarget";
        auto renderTarget = device->createRenderTarget(targetOptions);
        if (!renderTarget) {
            return false;
        }

        QuadRender quad(shader);
        quad.setTexture(0, request.source);

        EnvBakePass pass(device, "EnvConvolve");
        pass.init(renderTarget);

        device->beginOfflineWork();
        device->startRenderPass(&pass);
        setBakeRenderState(device);
        drawConvolveRects(quad, request.rects, tables,
            request.encodeRgbp, request.decodeSrgb);
        device->endRenderPass(&pass);
        device->endOfflineWork();
        return true;
    }

    bool bakeEnvAtlas(GraphicsDevice* device, const EnvAtlasRequest& request)
    {
        if (!device || !request.target) {
            spdlog::warn("bakeEnvAtlas: target is null");
            return false;
        }
        const bool wantReproject = request.reprojectSource && !request.reprojectRects.empty();
        const bool wantConvolve = request.convolveSource && !request.convolveRects.empty();
        if (!wantReproject && !wantConvolve) {
            spdlog::warn("bakeEnvAtlas: nothing to bake");
            return false;
        }

        std::shared_ptr<Shader> reproject;
        std::shared_ptr<Shader> convolve;
        if (wantReproject) {
            reproject = reprojectShader(device, request.reprojectSource->isCubemap());
            request.reprojectSource->upload();
        }
        if (wantConvolve) {
            convolve = convolveShader(device, request.convolveSource->isCubemap());
            request.convolveSource->upload();
        }
        if ((wantReproject && !reproject) || (wantConvolve && !convolve)) {
            spdlog::error("bakeEnvAtlas: no shader (reproject={} convolve={})",
                static_cast<const void*>(reproject.get()), static_cast<const void*>(convolve.get()));
            return false;
        }
        request.target->upload();

        std::vector<std::shared_ptr<Texture>> tables;
        tables.reserve(request.convolveRects.size());
        for (const auto& entry : request.convolveRects) {
            tables.push_back(makeSampleTable(device, entry.samples, entry.numSamples));
        }

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device;
        targetOptions.colorBuffer = request.target;
        targetOptions.depth = false;
        targetOptions.samples = 1;
        targetOptions.flipY = false;
        targetOptions.name = "envAtlasTarget";
        auto renderTarget = device->createRenderTarget(targetOptions);
        if (!renderTarget) {
            return false;
        }

        EnvBakePass pass(device, "EnvAtlas");
        pass.init(renderTarget);
        // The rects do not tile the whole atlas, and what they miss is whatever the
        // allocation happened to hold — black on one backend, uninitialised garbage
        // on the other. One bake writes every rect it owns, so clearing first costs
        // nothing and makes the gaps deterministic.
        const Color clearColor(0.0f, 0.0f, 0.0f, 1.0f);
        pass.setClearColor(&clearColor);

        device->beginOfflineWork();
        device->startRenderPass(&pass);
        setBakeRenderState(device);

        if (wantReproject) {
            QuadRender quad(reproject);
            quad.setTexture(0, request.reprojectSource);
            for (const auto& rect : request.reprojectRects) {
                if (rect.width <= 0 || rect.height <= 0) {
                    continue;
                }
                env_shaders::ReprojectUniforms uniforms{};
                fillUvMod(uniforms.uvMod, rect);
                uniforms.sourceProjection = static_cast<uint32_t>(request.reprojectSourceProjection);
                uniforms.targetProjection = static_cast<uint32_t>(request.reprojectTargetProjection);
                uniforms.encodeRgbp = request.encodeRgbp ? 1u : 0u;
                uniforms.decodeSrgb = request.decodeSrgb ? 1u : 0u;
                quad.setUniforms(uniforms);

                const Vector4 viewport = rectViewport(rect);
                quad.render(&viewport);
            }
        }
        if (wantConvolve) {
            QuadRender quad(convolve);
            quad.setTexture(0, request.convolveSource);
            drawConvolveRects(quad, request.convolveRects, tables,
                request.encodeRgbp, request.decodeSrgb);
        }

        device->endRenderPass(&pass);
        device->endOfflineWork();
        return true;
    }
}
