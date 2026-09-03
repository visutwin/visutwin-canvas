// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.07.2026.
//
#include "reflectionProbe.h"

#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/renderer/lightCamera.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    namespace
    {
        int mipLevelsFor(int size)
        {
            int levels = 1;
            while (size > 1) {
                size >>= 1;
                ++levels;
            }
            return levels;
        }
    }

    ReflectionProbe::ReflectionProbe(Engine* engine, const int faceSize)
        : _engine(engine), _faceSize(faceSize)
    {
        auto device = _engine->graphicsDevice();

        // Mipmapped LDR color cubemap. The faces hold the normal forward output
        // (tonemapped + gamma-encoded); the probe shader sRGB-decodes on sample,
        // matching the static-cubemap path. Coarser mips approximate roughness.
        TextureOptions cubeOptions;
        cubeOptions.width = static_cast<uint32_t>(faceSize);
        cubeOptions.height = static_cast<uint32_t>(faceSize);
        cubeOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        cubeOptions.cubemap = true;
        cubeOptions.mipmaps = true;
        cubeOptions.numLevels = static_cast<uint32_t>(mipLevelsFor(faceSize));
        cubeOptions.name = "reflectionProbeCube";
        _cube = std::make_shared<Texture>(device.get(), cubeOptions);

        // Probe root: the six face cameras hang off it, so setPosition() moves
        // all of them at once. Face cameras look along ±X/±Y/±Z (90° FOV).
        _node = new Entity();
        _node->setEngine(_engine);
        _engine->root()->addChild(_node);

        _faceTargets.resize(6);
        for (int face = 0; face < 6; ++face) {
            RenderTargetOptions rtOptions;
            rtOptions.graphicsDevice = device.get();
            rtOptions.colorBuffer = _cube.get();
            rtOptions.face = face;
            rtOptions.depth = true;
            rtOptions.name = "reflectionProbeFace";
            _faceTargets[face] = device->createRenderTarget(rtOptions);

            auto* faceEntity = new Entity();
            faceEntity->setEngine(_engine);
            faceEntity->setLocalRotation(LightCamera::pointLightRotations[face]);
            _node->addChild(faceEntity);
            _faceEntities[face] = faceEntity;

            auto* cameraComponent = static_cast<CameraComponent*>(faceEntity->addComponent<CameraComponent>());
            cameraComponent->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
            Camera* camera = cameraComponent->camera();
            camera->setRenderTarget(_faceTargets[face]);
            camera->setProjection(ProjectionType::Perspective);
            camera->setFov(90.0f);
            camera->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
            camera->setAspectRatio(1.0f);
            camera->setNearClip(0.1f);
            camera->setFarClip(500.0f);
            camera->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
            _faceCameras[face] = cameraComponent;
        }
    }

    ReflectionProbe::~ReflectionProbe()
    {
        if (_installed && _engine && _engine->scene()) {
            _engine->scene()->clearReflectionProbe();
        }
        if (_node) {
            if (_node->parent()) {
                auto ownedNode = _node->remove();
                ownedNode.reset();
            } else {
                delete _node;
            }
            _node = nullptr;
        }
        _faceEntities.fill(nullptr);
        _faceCameras.fill(nullptr);
    }

    void ReflectionProbe::setPosition(const Vector3& worldPosition)
    {
        _position = worldPosition;
        if (_node) {
            _node->setLocalPosition(worldPosition);
        }
        if (_installed) {
            applyProbe();
        }
    }

    void ReflectionProbe::setBox(const Vector3& boxMin, const Vector3& boxMax, const bool boxProjection)
    {
        _boxMin = boxMin;
        _boxMax = boxMax;
        _boxProjection = boxProjection;
        if (_installed) {
            applyProbe();
        }
    }

    void ReflectionProbe::setIntensity(const float intensity)
    {
        _intensity = intensity;
        if (_installed) {
            applyProbe();
        }
    }

    void ReflectionProbe::setNearFar(const float nearClip, const float farClip)
    {
        for (auto* cameraComponent : _faceCameras) {
            if (cameraComponent && cameraComponent->camera()) {
                cameraComponent->camera()->setNearClip(nearClip);
                cameraComponent->camera()->setFarClip(farClip);
            }
        }
    }

    void ReflectionProbe::setLayers(const std::vector<int>& layers)
    {
        for (auto* cameraComponent : _faceCameras) {
            if (cameraComponent) {
                cameraComponent->setLayers(layers);
            }
        }
    }

    void ReflectionProbe::setDynamic(const bool dynamic)
    {
        _dynamic = dynamic;
        if (_dynamic && !_capturing) {
            setCapturingEnabled(true);
        }
    }

    void ReflectionProbe::requestBake()
    {
        if (!_capturing) {
            setCapturingEnabled(true);
        }
    }

    void ReflectionProbe::update()
    {
        if (!_capturing) {
            return;
        }

        // The six face cameras have just rendered the scene into cube level 0;
        // build the coarser roughness mips from them.
        _engine->graphicsDevice()->generateMipmaps(_cube.get());

        if (!_installed) {
            applyProbe();
            _installed = true;
        }

        // One-shot: stop re-rendering the faces after the first successful bake.
        if (!_dynamic) {
            setCapturingEnabled(false);
        }
    }

    void ReflectionProbe::applyProbe()
    {
        if (auto scene = _engine->scene()) {
            scene->setReflectionProbe(_cube.get(), _position, _boxMin, _boxMax, _boxProjection, _intensity);
        }
    }

    void ReflectionProbe::setCapturingEnabled(const bool enabled)
    {
        _capturing = enabled;
        for (auto* cameraComponent : _faceCameras) {
            if (cameraComponent) {
                cameraComponent->setEnabled(enabled);
            }
        }
    }
}
