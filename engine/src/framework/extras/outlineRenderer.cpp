// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.07.2026
//
#include "outlineRenderer.h"

#include <algorithm>

#include "scene/shader-lib/slangShaders.h"
#include "framework/engine.h"
#include "scene/renderer/forwardRenderer.h"
#include "framework/entity.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/layer.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "scene/composition/layerComposition.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {

    }

    OutlineRenderer::OutlineRenderer(Engine* engine, const int layerId) : _engine(engine)
    {
        const auto device = engine->graphicsDevice();
        const auto scene = engine->scene();

        // Dedicated outline layer, rendered only by the outline camera (the main
        // camera's default layer list does not include this id).
        _layer = std::make_shared<Layer>("Outline", layerId);
        if (scene && scene->layers()) {
            scene->layers()->pushOpaque(_layer);
        }

        // Offscreen silhouette target (RGBA8 + depth) and ping-pong intermediate.
        resizeTargets(4, 4);

        // Camera rendering the outline layer to the offscreen target.
        _cameraEntity = new Entity();
        _cameraEntity->setEngine(engine);
        _cameraComponent = static_cast<CameraComponent*>(_cameraEntity->addComponent<CameraComponent>());
        if (_cameraComponent) {
            _cameraComponent->setLayers({layerId});
            _cameraComponent->camera()->setRenderTarget(_renderTarget);
            _cameraComponent->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 0.0f));
        }
        engine->root()->addChild(_cameraEntity);

        // Post passes: horizontal extend (rt -> temp), vertical extend (temp -> rt),
        // alpha blend over the back buffer.
        _extendHorizontalPass = std::make_shared<RenderPassShaderQuad>(device);
        _extendHorizontalPass->setShader(getOrCreateSlangShader(device.get(), "outline-extend", "h"));
        _extendHorizontalPass->init(_tempRenderTarget);
        _extendHorizontalPass->setRequiresCubemaps(false);

        _extendVerticalPass = std::make_shared<RenderPassShaderQuad>(device);
        _extendVerticalPass->setShader(getOrCreateSlangShader(device.get(), "outline-extend", "v"));
        _extendVerticalPass->init(_renderTarget);
        _extendVerticalPass->setRequiresCubemaps(false);

        _blendPass = std::make_shared<RenderPassShaderQuad>(device);
        _blendPass->setShader(getOrCreateSlangShader(device.get(), "outline-blend"));
        _blendPass->init(nullptr);
        _blendPass->setRequiresCubemaps(false);
        _blendPass->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
    }

    OutlineRenderer::~OutlineRenderer()
    {
        removeAllEntities();
        setPassesRegistered(false);
        if (_cameraEntity) {
            if (_cameraEntity->parent()) {
                auto ownedCamera = _cameraEntity->remove();
                ownedCamera.reset();
            } else {
                delete _cameraEntity;
            }
            _cameraEntity = nullptr;
            _cameraComponent = nullptr;
        }
    }

    void OutlineRenderer::collectClones(Entity* entity, const Color& color, const bool recursive,
        std::vector<Entity*>& sources, std::vector<std::unique_ptr<MeshInstance>>& clones,
        std::vector<std::shared_ptr<StandardMaterial>>& materials)
    {
        if (!entity) {
            return;
        }

        // Only a component that is actually rendered: a disabled
        // component or entity keeps its mesh instances, still visible, but out of the
        // scene's layers — and this renderer has its own layer, so cloning them outlined
        // an object that is not drawn. Checked when the entity is added;
        // removal goes by the record and needs no check.
        if (auto* render = entity->findComponent<RenderComponent>(); render && render->active()) {
            // Flat unlit silhouette material in the outline color.
            auto material = std::make_shared<StandardMaterial>();
            material->setName("outline-silhouette");
            material->setUnlit(true);
            material->setDiffuse(color);
            materials.push_back(material);
            sources.push_back(entity);

            for (auto* meshInstance : render->meshInstances()) {
                if (!meshInstance || !meshInstance->mesh()) {
                    continue;
                }
                // Clone shares mesh and node — transforms follow the source entity.
                auto clone = std::make_unique<MeshInstance>(
                    meshInstance->mesh(), material.get(), meshInstance->node());
                clone->setCastShadow(false);
                clone->setReceiveShadow(false);
                clones.push_back(std::move(clone));
            }
        }

        if (recursive) {
            for (const auto& child : entity->children()) {
                // dynamic_cast: a plain GraphNode child is not an Entity, and a static_cast
                // would be undefined for one.
                if (auto* childEntity = dynamic_cast<Entity*>(child.get())) {
                    collectClones(childEntity, color, recursive, sources, clones, materials);
                }
            }
        }
    }

    void OutlineRenderer::setPassesRegistered(const bool value)
    {
        if (value == _passesRegistered || !_engine || !_engine->renderer()) {
            return;
        }
        const auto& renderer = _engine->renderer();
        if (value) {
            renderer->addAppendPass(_extendHorizontalPass);
            renderer->addAppendPass(_extendVerticalPass);
            renderer->addAppendPass(_blendPass);
        } else {
            renderer->removeAppendPass(_extendHorizontalPass);
            renderer->removeAppendPass(_extendVerticalPass);
            renderer->removeAppendPass(_blendPass);
        }
        _passesRegistered = value;
    }

    void OutlineRenderer::addEntity(Entity* entity, const Color& color, const bool recursive)
    {
        if (!entity || !_layer) {
            return;
        }

        OutlinedEntity record;
        record.entity = entity;
        std::vector<Entity*> sources;
        collectClones(entity, color, recursive, sources, record.clones, record.materials);
        // The clones point at these entities' nodes: destroying any of them drops the
        // whole record before the node is freed, so no pointer is left dangling. The root
        // is watched even when it contributed no clone of its own.
        if (std::find(sources.begin(), sources.end(), entity) == sources.end()) {
            sources.push_back(entity);
        }
        for (auto* source : sources) {
            record.destroyHandles.push_back(source->on("destroy", [this, entity](const EventArgs&) {
                removeEntity(entity);
            }));
        }

        std::vector<MeshInstance*> instances;
        instances.reserve(record.clones.size());
        for (const auto& clone : record.clones) {
            instances.push_back(clone.get());
        }
        _layer->addMeshInstances(instances);
        _outlined.push_back(std::move(record));
        setPassesRegistered(!_outlined.empty());
    }

    void OutlineRenderer::removeEntity(Entity* entity)
    {
        for (auto it = _outlined.begin(); it != _outlined.end();) {
            if (it->entity == entity) {
                std::vector<MeshInstance*> instances;
                instances.reserve(it->clones.size());
                for (const auto& clone : it->clones) {
                    instances.push_back(clone.get());
                }
                _layer->removeMeshInstances(instances);
                for (const auto& handle : it->destroyHandles) {
                    handle->off();
                }
                it = _outlined.erase(it);
            } else {
                ++it;
            }
        }
        setPassesRegistered(!_outlined.empty());
    }

    void OutlineRenderer::removeAllEntities()
    {
        for (const auto& record : _outlined) {
            std::vector<MeshInstance*> instances;
            instances.reserve(record.clones.size());
            for (const auto& clone : record.clones) {
                instances.push_back(clone.get());
            }
            _layer->removeMeshInstances(instances);
            for (const auto& handle : record.destroyHandles) {
                handle->off();
            }
        }
        _outlined.clear();
        setPassesRegistered(false);
    }

    void OutlineRenderer::resizeTargets(const uint32_t width, const uint32_t height)
    {
        const auto device = _engine->graphicsDevice();

        const auto makeTexture = [&](const char* name) {
            TextureOptions options;
            options.name = name;
            options.width = width;
            options.height = height;
            options.format = PixelFormat::PIXELFORMAT_RGBA8;
            options.mipmaps = false;
            options.minFilter = FilterMode::FILTER_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;
            auto texture = std::make_shared<Texture>(device.get(), options);
            texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
            texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
            return texture;
        };

        if (!_renderTarget) {
            _colorTexture = makeTexture("OutlineTexture");
            RenderTargetOptions rtOptions;
            rtOptions.graphicsDevice = device.get();
            rtOptions.colorBuffer = _colorTexture.get();
            rtOptions.depth = true;
            rtOptions.name = "OutlineRT";
            _renderTarget = device->createRenderTarget(rtOptions);

            _tempTexture = makeTexture("OutlineTempTexture");
            RenderTargetOptions tempOptions;
            tempOptions.graphicsDevice = device.get();
            tempOptions.colorBuffer = _tempTexture.get();
            tempOptions.depth = false;
            tempOptions.name = "OutlineTempRT";
            _tempRenderTarget = device->createRenderTarget(tempOptions);
        } else if (_renderTarget->width() != static_cast<int>(width) ||
                   _renderTarget->height() != static_cast<int>(height)) {
            _renderTarget->resize(width, height);
            _tempRenderTarget->resize(width, height);
        }
    }

    void OutlineRenderer::frameUpdate(Entity* sceneCameraEntity)
    {
        if (!sceneCameraEntity || !_cameraComponent) {
            return;
        }

        const auto [width, height] = _engine->graphicsDevice()->size();
        resizeTargets(static_cast<uint32_t>(std::max(width, 1)), static_cast<uint32_t>(std::max(height, 1)));

        // Mirror the scene camera.
        const auto* sceneCameraComponent = sceneCameraEntity->findComponent<CameraComponent>();
        if (sceneCameraComponent && sceneCameraComponent->camera()) {
            const auto* sceneCamera = sceneCameraComponent->camera();
            auto* outlineCamera = _cameraComponent->camera();
            outlineCamera->setFov(sceneCamera->fov());
            outlineCamera->setNearClip(sceneCamera->nearClip());
            outlineCamera->setFarClip(sceneCamera->farClip());
        }
        const auto& pos = sceneCameraEntity->localPosition();
        _cameraEntity->setLocalPosition(pos);
        _cameraEntity->setLocalRotation(sceneCameraEntity->localRotation());

        // Refresh quad inputs (textures can be recreated on resize).
        _extendHorizontalPass->setQuadTextureBinding(0, _colorTexture.get());
        _extendVerticalPass->setQuadTextureBinding(0, _tempTexture.get());
        _blendPass->setQuadTextureBinding(0, _colorTexture.get());
    }

}
