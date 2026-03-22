// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 09.10.2025.
//

#include "sky.h"

#include "spdlog/spdlog.h"
#include "platform/graphics/texture.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    Sky::Sky(const std::shared_ptr<GraphicsDevice>& device, Scene* scene) : _device(device), _scene(scene)
    {
    }

    Sky::~Sky()
    {
    }

    void Sky::setType(const int value)
    {
        if (_type != value) {
            _type = value;
            updateSkyMesh();
        }
    }

    void Sky::setDepthWrite(const bool value)
    {
        _depthWrite = value;
    }

    void Sky::updateSkyMesh()
    {
        // need either skybox cubemap or envAtlas to create the sky mesh
        if (!_scene || (!_scene->skybox() && !_scene->envAtlas())) {
            spdlog::debug("Sky::updateSkyMesh: no scene or skybox/envAtlas — resetting");
            resetSkyMesh();
            return;
        }

        // _getSkyboxTex() priority:
        // skyboxMip == 0: prefer skyboxCubeMap > envAtlas
        // skyboxMip > 0: prefer envAtlas (blurred mip levels)
        Texture* skyTex = nullptr;
        if (_scene->skyboxMip() == 0 && _scene->skybox()) {
            skyTex = _scene->skybox();
        } else {
            skyTex = _scene->envAtlas();
        }

        if (!skyTex) {
            // Fallback: use whatever is available
            skyTex = _scene->skybox() ? _scene->skybox() : _scene->envAtlas();
        }

        if (!skyTex) {
            resetSkyMesh();
            return;
        }

        spdlog::debug("Sky::updateSkyMesh: creating SkyMesh (type={}, tex={}x{}, cubemap={}, layers={})",
            _type, skyTex->width(), skyTex->height(),
            skyTex->isCubemap() ? "yes" : "no",
            _scene->layers() ? "yes" : "no");
        resetSkyMesh();
        _skyMesh = std::make_unique<SkyMesh>(_device, _scene, &_node, skyTex, _type);
    }

    void Sky::resetSkyMesh()
    {
        _skyMesh.reset();
    }

    SkyMesh* Sky::skyMesh()
    {
        if (!_skyMesh) {
            updateSkyMesh();
        }
        return _skyMesh.get();
    }

    Vector3 Sky::centerWorldPos() const
    {
        // transform the local center by the sky node's world transform
        return const_cast<GraphNode&>(_node).worldTransform().transformPoint(_center);
    }
}
