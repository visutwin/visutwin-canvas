// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.11.2026
//
#include "gaussianMipmaps.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "core/math/vector4.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    namespace
    {
        struct MipmapUniforms
        {
            float source[4];   // xy: the source level's texel size, z: the source level
            float scale[4];    // xy: the copy's uv scale (the level's size over the scratch's)
        };

        uint32_t levelSize(const uint32_t size, const uint32_t level)
        {
            return std::max(size >> level, 1u);
        }
    }

    // A quad pass the generator drives from outside: the shader setter is a subclass's.
    class GaussianMipmapGenerator::QuadPass final : public RenderPassShaderQuad
    {
    public:
        using RenderPassShaderQuad::RenderPassShaderQuad;
        using RenderPassShaderQuad::useSlangShader;
    };

    GaussianMipmapGenerator::GaussianMipmapGenerator(const std::shared_ptr<GraphicsDevice>& device)
        : _device(device),
          _blurPass(std::make_unique<QuadPass>(device)),
          _copyPass(std::make_unique<QuadPass>(device))
    {
    }

    GaussianMipmapGenerator::~GaussianMipmapGenerator() = default;

    void GaussianMipmapGenerator::prepareShaders()
    {
        if (!_blurPass->shader()) {
            _blurPass->useSlangShader("mipmap-gaussian", "gaussian");
        }
        if (!_copyPass->shader()) {
            _copyPass->useSlangShader("mipmap-gaussian", "copy");
        }
    }

    GaussianMipmapGenerator::Scratch& GaussianMipmapGenerator::scratchFor(const Texture* texture)
    {
        Scratch& scratch = _scratch[texture->format()];
        const uint32_t width = levelSize(texture->width(), 1);
        const uint32_t height = levelSize(texture->height(), 1);
        if (!scratch.texture || scratch.texture->width() < width || scratch.texture->height() < height) {
            // grown to fit the textures it served before as well
            TextureOptions options;
            options.name = "GaussianMipmapScratch";
            options.width = std::max(width, scratch.texture ? scratch.texture->width() : 0u);
            options.height = std::max(height, scratch.texture ? scratch.texture->height() : 0u);
            options.format = texture->format();
            options.mipmaps = false;
            options.minFilter = FilterMode::FILTER_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;
            scratch.targets.clear();
            scratch.texture = std::make_shared<Texture>(_device.get(), options);
            scratch.texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            scratch.texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            scratch.texture->upload();

            RenderTargetOptions targetOptions;
            targetOptions.graphicsDevice = _device.get();
            targetOptions.colorBuffer = scratch.texture.get();
            targetOptions.depth = false;
            targetOptions.samples = 1;
            targetOptions.name = "GaussianMipmapScratchTarget";
            scratch.targets.push_back(_device->createRenderTarget(targetOptions));
        }
        return scratch;
    }

    std::shared_ptr<RenderTarget> GaussianMipmapGenerator::levelTarget(Texture* texture, const uint32_t level)
    {
        LevelTargets* entry = nullptr;
        for (auto& candidate : _levelTargets) {
            if (candidate.texture == texture) {
                entry = &candidate;
                break;
            }
        }
        if (entry && (entry->width != texture->width() || entry->height != texture->height() ||
                entry->targets.size() != texture->getNumLevels())) {
            entry->targets.clear();
            entry->width = texture->width();
            entry->height = texture->height();
            entry->targets.resize(texture->getNumLevels());
        }
        if (!entry) {
            _levelTargets.push_back({texture, texture->width(), texture->height(), {}});
            entry = &_levelTargets.back();
            entry->targets.resize(texture->getNumLevels());
        }
        if (level >= entry->targets.size()) {
            return nullptr;
        }
        auto& target = entry->targets[level];
        if (!target) {
            RenderTargetOptions options;
            options.graphicsDevice = _device.get();
            options.colorBuffer = texture;
            options.mipLevel = static_cast<int>(level);
            options.depth = false;
            options.samples = 1;
            options.name = "GaussianMipmapLevel";
            target = _device->createRenderTarget(options);
        }
        return target;
    }

    bool GaussianMipmapGenerator::prepare(Texture* texture)
    {
        if (!texture || !texture->gaussianMipmaps() || texture->getNumLevels() <= 1) {
            return false;
        }
        // The first render target over a texture created empty recreates its GPU image, so
        // the targets exist before anything is rendered or copied into it.
        for (uint32_t level = 1; level < texture->getNumLevels(); ++level) {
            if (!levelTarget(texture, level)) {
                return false;
            }
        }
        scratchFor(texture);
        return true;
    }

    bool GaussianMipmapGenerator::generate(Texture* texture)
    {
        if (!prepare(texture) || !_device || _device->insideRenderPass()) {
            return false;
        }
        prepareShaders();
        if (!_blurPass->shader() || !_copyPass->shader()) {
            return false;
        }
        Scratch& scratch = scratchFor(texture);
        const auto& scratchTarget = scratch.targets.front();
        if (!scratchTarget) {
            return false;
        }
        const float scratchWidth = static_cast<float>(scratch.texture->width());
        const float scratchHeight = static_cast<float>(scratch.texture->height());

        for (uint32_t level = 1; level < texture->getNumLevels(); ++level) {
            const auto target = levelTarget(texture, level);
            if (!target) {
                return false;
            }
            const float width = static_cast<float>(levelSize(texture->width(), level));
            const float height = static_cast<float>(levelSize(texture->height(), level));
            const Vector4 viewport(0.0f, 0.0f, width, height);

            // the level, blurred from the one above it, into the scratch texture's corner
            MipmapUniforms blur{};
            blur.source[0] = 1.0f / static_cast<float>(levelSize(texture->width(), level - 1));
            blur.source[1] = 1.0f / static_cast<float>(levelSize(texture->height(), level - 1));
            blur.source[2] = static_cast<float>(level - 1);
            blur.scale[0] = blur.scale[1] = 1.0f;
            _blurPass->init(scratchTarget);
            _blurPass->setClearColor(nullptr);
            _blurPass->setViewport(viewport);
            _blurPass->setQuadTextureBinding(0, texture);
            _blurPass->setQuadUniforms(blur);
            _blurPass->render();

            // and from there into the texture's own level
            MipmapUniforms copy{};
            copy.source[0] = 1.0f / scratchWidth;
            copy.source[1] = 1.0f / scratchHeight;
            copy.source[2] = 0.0f;
            copy.scale[0] = width / scratchWidth;
            copy.scale[1] = height / scratchHeight;
            _copyPass->init(target);
            _copyPass->setClearColor(nullptr);
            _copyPass->clearViewport();
            _copyPass->setQuadTextureBinding(0, scratch.texture.get());
            _copyPass->setQuadUniforms(copy);
            _copyPass->render();
        }
        return true;
    }

    RenderPassGaussianMipmaps::RenderPassGaussianMipmaps(const std::shared_ptr<GraphicsDevice>& device,
        const std::weak_ptr<RenderTarget>& target)
        : RenderPass(device), _target(target), _generator(device)
    {
        _name = "GaussianMipmaps";
    }

    Texture* RenderPassGaussianMipmaps::texture() const
    {
        const auto target = _target.lock();
        return target && target->colorBufferCount() > 0 ? target->getColorBuffer(0) : nullptr;
    }

    void RenderPassGaussianMipmaps::prepareShaders()
    {
        _generator.prepareShaders();
    }

    void RenderPassGaussianMipmaps::execute()
    {
        if (Texture* texture = this->texture()) {
            _generator.generate(texture);
        }
    }
}
