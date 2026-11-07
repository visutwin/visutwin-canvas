// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
#include "hiZPyramid.h"

#include <algorithm>
#include <optional>

#include <spdlog/spdlog.h>

#include "core/math/vector4.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/sceneMapHandles.h"
#include "platform/graphics/texture.h"
#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    namespace
    {
        struct HiZUniforms
        {
            int32_t source[4];   // xy: the source's size, z: the source level (reduce)
            int32_t target[4];   // xy: the size of the level written
        };

        bool isDepthFormat(const PixelFormat format)
        {
            return format == PixelFormat::PIXELFORMAT_DEPTH || format == PixelFormat::PIXELFORMAT_DEPTH16 ||
                format == PixelFormat::PIXELFORMAT_DEPTHSTENCIL;
        }

        std::shared_ptr<RenderTarget> createTarget(GraphicsDevice* device, Texture* texture, const int level,
            const char* name)
        {
            RenderTargetOptions options;
            options.graphicsDevice = device;
            options.colorBuffer = texture;
            options.mipLevel = level;
            options.depth = false;
            options.samples = 1;
            options.name = name;
            return device->createRenderTarget(options);
        }
    }

    // A quad pass the generator drives from outside: the shader setter is a subclass's.
    class HiZPyramidGenerator::QuadPass final : public RenderPassShaderQuad
    {
    public:
        // Named for the GPU profiler, which times each device pass by its pass's name.
        QuadPass(const std::shared_ptr<GraphicsDevice>& device, const char* name) : RenderPassShaderQuad(device)
        {
            _name = name;
        }
        using RenderPassShaderQuad::useSlangShader;

        /// Draws one level into `target` (within `viewport`, or all of it), reading `source`.
        void draw(const std::shared_ptr<RenderTarget>& target, const std::optional<Vector4>& viewport,
            Texture* source, const HiZUniforms& uniforms)
        {
            init(target);
            // The chain is this generator's to write: both backends would otherwise regenerate
            // a mipmapped target's whole chain from its top level at the end of the pass.
            if (const auto ops = colorOps()) {
                ops->genMipmaps = false;
            }
            setClearColor(nullptr);
            setViewport(viewport);
            setQuadTextureBinding(0, source);
            setQuadUniforms(uniforms);
            render();
        }
    };

    HiZPyramidGenerator::HiZPyramidGenerator(const std::shared_ptr<GraphicsDevice>& device)
        : _device(device),
          _depthPass(std::make_unique<QuadPass>(device, "HiZLevel0")),
          _floatPass(std::make_unique<QuadPass>(device, "HiZLevel0")),
          _reducePass(std::make_unique<QuadPass>(device, "HiZReduce")),
          _copyPass(std::make_unique<QuadPass>(device, "HiZCopy"))
    {
    }

    HiZPyramidGenerator::~HiZPyramidGenerator() = default;

    uint32_t HiZPyramidGenerator::levelSize(const uint32_t depthSize, const uint32_t level)
    {
        return std::max(depthSize >> (level + 1), 1u);
    }

    void HiZPyramidGenerator::prepareShaders()
    {
        const std::pair<QuadPass*, const char*> variants[] = {
            {_depthPass.get(), "depth"}, {_floatPass.get(), "float"}, {_reducePass.get(), "reduce"},
            {_copyPass.get(), "copy"}};
        for (const auto& [pass, variant] : variants) {
            if (!pass->shader()) {
                pass->useSlangShader("hiz-pyramid", variant);
            }
        }
    }

    bool HiZPyramidGenerator::allocate(const uint32_t depthWidth, const uint32_t depthHeight)
    {
        if (_pyramid && depthWidth == _depthWidth && depthHeight == _depthHeight) {
            return true;
        }
        _levelTargets.clear();
        _scratchTarget.reset();
        _scratch.reset();
        _pyramid.reset();
        _depthWidth = depthWidth;
        _depthHeight = depthHeight;

        TextureOptions options;
        options.name = "HiZPyramid";
        options.width = levelSize(depthWidth, 0);
        options.height = levelSize(depthHeight, 0);
        options.format = PixelFormat::PIXELFORMAT_RG32F;
        options.mipmaps = true;
        options.minFilter = FilterMode::FILTER_NEAREST_MIPMAP_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        _pyramid = std::make_shared<Texture>(_device.get(), options);
        _pyramid->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _pyramid->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _pyramid->upload();

        // The targets exist before anything is rendered into the pyramid: the first render
        // target over a texture created empty recreates its GPU image.
        const uint32_t levels = _pyramid->getNumLevels();
        _levelTargets.resize(levels);
        for (uint32_t level = 0; level < levels; ++level) {
            _levelTargets[level] = createTarget(_device.get(), _pyramid.get(), static_cast<int>(level), "HiZLevel");
            if (!_levelTargets[level]) {
                _pyramid.reset();
                return false;
            }
        }

        // Levels after the first are rendered into the scratch first: the size of level 1,
        // the largest of them.
        if (levels > 1) {
            TextureOptions scratchOptions = options;
            scratchOptions.name = "HiZScratch";
            scratchOptions.width = levelSize(depthWidth, 1);
            scratchOptions.height = levelSize(depthHeight, 1);
            scratchOptions.mipmaps = false;
            scratchOptions.minFilter = FilterMode::FILTER_NEAREST;
            _scratch = std::make_shared<Texture>(_device.get(), scratchOptions);
            _scratch->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            _scratch->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            _scratch->upload();
            _scratchTarget = createTarget(_device.get(), _scratch.get(), 0, "HiZScratchTarget");
            if (!_scratchTarget) {
                _pyramid.reset();
                return false;
            }
        }
        return true;
    }

    std::shared_ptr<Texture> HiZPyramidGenerator::generate(Texture* source)
    {
        if (!source || !_device || _device->insideRenderPass() || source->width() == 0 || source->height() == 0) {
            return nullptr;
        }
        const bool depth = isDepthFormat(source->format());
        if (!depth && source->format() != PixelFormat::PIXELFORMAT_R32F) {
            if (!_warnedSource) {
                _warnedSource = true;
                spdlog::warn("[HiZ] a pyramid is built from a depth texture or an R32F one, not pixel format {}",
                    static_cast<uint32_t>(source->format()));
            }
            return nullptr;
        }
        if (!allocate(source->width(), source->height())) {
            return nullptr;
        }
        prepareShaders();
        QuadPass* first = depth ? _depthPass.get() : _floatPass.get();
        if (!first->shader() || !_reducePass->shader() || !_copyPass->shader()) {
            return nullptr;
        }

        const auto width = [&](const uint32_t level) { return static_cast<int32_t>(levelSize(_depthWidth, level)); };
        const auto height = [&](const uint32_t level) { return static_cast<int32_t>(levelSize(_depthHeight, level)); };

        // level 0, from the depth map
        first->draw(_levelTargets[0], std::nullopt, source,
            {{static_cast<int32_t>(_depthWidth), static_cast<int32_t>(_depthHeight), 0, 0}, {width(0), height(0), 0, 0}});

        const int32_t scratchWidth = _scratch ? static_cast<int32_t>(_scratch->width()) : 0;
        const int32_t scratchHeight = _scratch ? static_cast<int32_t>(_scratch->height()) : 0;
        for (uint32_t level = 1; level < _levelTargets.size(); ++level) {
            // the level, reduced from the one above, into the scratch texture's corner
            const Vector4 viewport(0.0f, 0.0f, static_cast<float>(width(level)), static_cast<float>(height(level)));
            _reducePass->draw(_scratchTarget, viewport, _pyramid.get(),
                {{width(level - 1), height(level - 1), static_cast<int32_t>(level - 1), 0},
                    {width(level), height(level), 0, 0}});
            // and from there into the pyramid's own level
            _copyPass->draw(_levelTargets[level], std::nullopt, _scratch.get(),
                {{scratchWidth, scratchHeight, 0, 0}, {width(level), height(level), 0, 0}});
        }
        return _pyramid;
    }

    RenderPassHiZ::RenderPassHiZ(const std::shared_ptr<GraphicsDevice>& device,
        std::shared_ptr<SceneDepthMapHandle> depth, std::shared_ptr<HiZMapHandle> hiZ)
        : RenderPass(device), _depth(std::move(depth)), _hiZ(std::move(hiZ)), _generator(device)
    {
        _name = "HiZ";
    }

    void RenderPassHiZ::prepareShaders()
    {
        _generator.prepareShaders();
    }

    void RenderPassHiZ::execute()
    {
        if (!_depth || !_hiZ) {
            return;
        }
        // Only a depth rendered in this frame: an older one is the pyramid already built. And
        // once a frame: a camera frame that built it after its prepass has run it already.
        const auto depth = _depth->texture.lock();
        if (!depth || _depth->renderVersion != device()->renderVersion() ||
            (_hiZ->renderVersion == device()->renderVersion() && !_hiZ->texture.expired())) {
            return;
        }
        const auto pyramid = _generator.generate(depth.get());
        if (!pyramid) {
            return;
        }
        HiZMapHandle& handle = *_hiZ;
        handle.texture = pyramid;
        handle.renderVersion = _depth->renderVersion;
        handle.depthWidth = depth->width();
        handle.depthHeight = depth->height();
        handle.levels = pyramid->getNumLevels();
        handle.cameraParams = _depth->cameraParams;
        handle.viewProjection = _depth->viewProjection;
        handle.viewProjectionInverse = _depth->viewProjectionInverse;
        handle.viewport = _depth->viewport;
    }
}
