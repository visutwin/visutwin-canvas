// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 09.10.2025.
//

#include "sky.h"

#include "spdlog/spdlog.h"
#include "platform/graphics/constants.h"
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
        if (!_scene) {
            resetSkyMesh();
            return;
        }

        // When atmosphere scattering is enabled, the sky mesh is needed even
        // without a cubemap/envAtlas — the fragment shader computes sky color
        // procedurally via nishitaScatter(). Create a 1×1 dummy texture and
        // use it as the sky texture. Also install it as the scene's envAtlas
        // so the texture binder properly binds it at fragment slot 2 (Metal
        // requires valid textures at all declared slots even when not sampled).
        if (_scene->atmosphereEnabled() && !_scene->skybox() && !_scene->envAtlas()) {
            if (!_atmosphereDummyTexture) {
                TextureOptions dummyOpts;
                dummyOpts.width = 1;
                dummyOpts.height = 1;
                dummyOpts.mipmaps = false;
                dummyOpts.minFilter = FilterMode::FILTER_NEAREST;
                dummyOpts.magFilter = FilterMode::FILTER_NEAREST;
                dummyOpts.name = "atmosphere-dummy";
                _atmosphereDummyTexture = std::make_shared<Texture>(_device.get(), dummyOpts);
                // Initialize with black pixels so the GPU texture is not reading uninitialized memory.
                const uint8_t black[4] = {0, 0, 0, 255};
                _atmosphereDummyTexture->setLevelData(0, black, sizeof(black));
                _atmosphereDummyTexture->upload();
            }
            if (_atmosphereDummyTexture) {
                // Use SKYTYPE_ATMOSPHERE (sphere mesh + infinite behavior).
                // The box mesh creates visible seams at face edges; a sphere has
                // continuous topology so view directions interpolate smoothly.
                spdlog::debug("Sky::updateSkyMesh: creating SkyMesh for atmosphere (sphere, layers={})",
                    _scene->layers() ? "yes" : "no");
                resetSkyMesh();
                _skyMesh = std::make_unique<SkyMesh>(_device, _scene, &_node, _atmosphereDummyTexture.get(), SKYTYPE_ATMOSPHERE);
                return;
            }
        }

        // need either skybox cubemap or envAtlas to create the sky mesh
        if (!_scene->skybox() && !_scene->envAtlas()) {
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
