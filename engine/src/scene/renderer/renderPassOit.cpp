// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
#include "renderPassOit.h"

#include <algorithm>

#include "core/math/color.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/composition/renderAction.h"
#include "scene/layer.h"
#include "scene/graphics/colorTarget.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/renderer/renderPassForward.h"
#include "scene/shader-lib/programLibrary.h"

namespace visutwin::canvas
{
    namespace
    {
        /// The accumulation: a forward pass with the program library's OIT switch on around
        /// its shader preparation and its draws, into a target cleared to zero.
        class RenderPassOitAccumulate final : public RenderPassForward
        {
        public:
            using RenderPassForward::RenderPassForward;

            void before() override
            {
                RenderPassForward::before();
                // After the action clears: both attachments start at zero, nothing else.
                static const Color zero(0.0f, 0.0f, 0.0f, 0.0f);
                setClearColor(&zero);
                setClearDepth(nullptr);
                setClearStencil(nullptr);
            }

            void prepareShaders() override
            {
                const SwitchScope scope(device());
                RenderPassForward::prepareShaders();
            }

            void execute() override
            {
                const SwitchScope scope(device());
                RenderPassForward::execute();
            }

        private:
            struct SwitchScope
            {
                explicit SwitchScope(const std::shared_ptr<GraphicsDevice>& device)
                    : library(getProgramLibrary(device))
                {
                    if (library) {
                        library->setOitAccumulate(true);
                    }
                }
                ~SwitchScope()
                {
                    if (library) {
                        library->setOitAccumulate(false);
                    }
                }
                SwitchScope(const SwitchScope&) = delete;
                SwitchScope& operator=(const SwitchScope&) = delete;
                std::shared_ptr<ProgramLibrary> library;
            };
        };

        /// The composite: the weighted average over the target by the coverage.
        class RenderPassOitComposite final : public RenderPassShaderQuad
        {
        public:
            RenderPassOitComposite(const std::shared_ptr<GraphicsDevice>& device, const RenderPassOit& owner)
                : RenderPassShaderQuad(device), _owner(owner)
            {
                setBlendState(RenderPassOit::compositeBlendState());
            }

            void prepareShaders() override
            {
                if (!shader()) {
                    useSlangShader("oit-composite");
                }
            }

            void execute() override
            {
                prepareShaders();
                if (!shader() || !_owner.accumulationTexture() || !_owner.coverageTexture()) {
                    return;
                }
                setQuadTextureBinding(0, _owner.accumulationTexture());
                setQuadTextureBinding(1, _owner.coverageTexture());
                RenderPassShaderQuad::execute();
            }

        private:
            const RenderPassOit& _owner;
        };
    }

    RenderPassOit::RenderPassOit(const std::shared_ptr<GraphicsDevice>& device, LayerComposition* layerComposition,
        Scene* scene, Renderer* renderer, CameraComponent* camera)
        : RenderPass(device), _layerComposition(layerComposition), _scene(scene), _renderer(renderer), _camera(camera)
    {
    }

    RenderPassOit::~RenderPassOit() = default;

    const std::shared_ptr<BlendState>& RenderPassOit::accumulationBlendState()
    {
        static const auto state = [] {
            auto blend = std::make_shared<BlendState>();
            blend->setEnabled(true);
            blend->setColorOp(BLENDEQUATION_ADD);
            blend->setColorSrcFactor(BLENDMODE_ONE);
            blend->setColorDstFactor(BLENDMODE_ONE);
            blend->setAlphaOp(BLENDEQUATION_ADD);
            blend->setAlphaSrcFactor(BLENDMODE_ONE);
            blend->setAlphaDstFactor(BLENDMODE_ONE);
            BlendState coverage;
            coverage.setEnabled(true);
            coverage.setColorOp(BLENDEQUATION_ADD);
            coverage.setColorSrcFactor(BLENDMODE_ONE);
            coverage.setColorDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
            coverage.setAlphaOp(BLENDEQUATION_ADD);
            coverage.setAlphaSrcFactor(BLENDMODE_ONE);
            coverage.setAlphaDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
            blend->setAttachment(1, coverage);
            return blend;
        }();
        return state;
    }

    const std::shared_ptr<BlendState>& RenderPassOit::compositeBlendState()
    {
        static const auto state = [] {
            auto blend = std::make_shared<BlendState>();
            blend->setEnabled(true);
            blend->setColorOp(BLENDEQUATION_ADD);
            blend->setColorSrcFactor(BLENDMODE_SRC_ALPHA);
            blend->setColorDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
            blend->setAlphaOp(BLENDEQUATION_ADD);
            blend->setAlphaSrcFactor(BLENDMODE_ONE);
            blend->setAlphaDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
            return blend;
        }();
        return state;
    }

    void RenderPassOit::ensureTargets(const int width, const int height)
    {
        if (_target && _accumulation && static_cast<int>(_accumulation->width()) == width &&
            static_cast<int>(_accumulation->height()) == height) {
            return;
        }
        GraphicsDevice* gd = device().get();
        _accumulation = createColorTexture(gd, "OitAccumulation", PixelFormat::PIXELFORMAT_RGBA16F, width, height);
        _coverage = createColorTexture(gd, "OitCoverage", PixelFormat::PIXELFORMAT_RGBA8, width, height);
        RenderTargetOptions options;
        options.graphicsDevice = gd;
        options.colorBuffers = {_accumulation.get(), _coverage.get()};
        options.depth = false;
        options.stencil = false;
        options.name = "OitTarget";
        _target = gd->createRenderTarget(options);
    }

    bool isOrderIndependentAction(const RenderAction& action)
    {
        return action.transparent && !action.useCameraPasses && action.camera && action.layer &&
            action.layer->orderIndependentTransparency();
    }

    RenderPass* RenderPassOit::compositePass() const
    {
        return _compositePass.get();
    }

    void RenderPassOit::setup(const std::vector<RenderAction*>& actions, const std::shared_ptr<RenderTarget>& target,
        const int width, const int height, const bool hdr, const bool cameraEvents)
    {
        clearBeforePasses();
        _ownedActions.clear();
        if (actions.empty() || width <= 0 || height <= 0) {
            return;
        }
        ensureTargets(width, height);

        auto accumulate = std::make_shared<RenderPassOitAccumulate>(device(), _layerComposition, _scene, _renderer,
            _camera);
        accumulate->setHdrPass(hdr);
        accumulate->init(_target);
        bool firstCameraUse = false;
        bool lastCameraUse = false;
        for (const auto* source : actions) {
            if (!source) {
                continue;
            }
            firstCameraUse = firstCameraUse || source->firstCameraUse;
            lastCameraUse = lastCameraUse || source->lastCameraUse;
            auto action = std::make_shared<RenderAction>(*source);
            action->renderTarget = _target;
            action->firstCameraUse = false;
            action->lastCameraUse = false;
            action->clearColor = false;
            action->clearDepth = false;
            action->clearStencil = false;
            accumulate->addRenderAction(action.get());
            _ownedActions.push_back(std::move(action));
        }
        // The camera's events fire from the pass, once, not from its cloned actions.
        accumulate->setCameraUse(cameraEvents && firstCameraUse, cameraEvents && lastCameraUse);
        _accumulatePass = accumulate;

        if (!_compositePass) {
            _compositePass = std::make_shared<RenderPassOitComposite>(device(), *this);
        }
        _compositePass->init(target);
        // Blended over what the target holds: load it, clear nothing.
        _compositePass->setClearColor(nullptr);
        _compositePass->setClearDepth(nullptr);
        _compositePass->setClearStencil(nullptr);

        addBeforePass(_accumulatePass);
        addBeforePass(_compositePass);
    }
}
