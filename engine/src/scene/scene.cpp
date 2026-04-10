// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.10.2025.
//
#include "scene.h"

#include <cstring>

#include "scene/graphics/envLighting.h"

namespace visutwin::canvas
{
    Scene::Scene(const std::shared_ptr<GraphicsDevice>& graphicsDevice) : _device(graphicsDevice), _lighting()
    {
        _sky = std::make_unique<Sky>(_device, this);
    }

    void Scene::setSkyboxMip(int value)
    {
        if (value != _skyboxMip) {
            _skyboxMip = value;
            resetSkyMesh();
        }
    }

    void Scene::resetSkyMesh()
    {
        if (!_sky) {
            return;
        }
        _sky->resetSkyMesh();
        _sky->updateSkyMesh();
        _updateShaders = true;
    }

    void Scene::setLayers(const std::shared_ptr<LayerComposition>& layers)
    {
        auto prev = _layers;
        _layers = layers;
        if (_sky) {
            _sky->updateSkyMesh();
        }
        fire(EVENT_SETLAYERS, prev, layers);
    }

    void Scene::setSkyboxIntensity(float value) {
        if (value != _skyboxIntensity) {
            _skyboxIntensity = value;
            resetSkyMesh();
        }
    }

    void Scene::setSkyType(const int value)
    {
        if (value != _skyType) {
            _skyType = value;
            if (_sky) {
                _sky->setType(value);
            }
            resetSkyMesh();
        }
    }

    void Scene::setEnvAtlas(Texture* value)
    {
        if (value != _envAtlas) {
            _envAtlas = value;

            // Make sure required options are set up on the texture
            if (value) {
                value->setAddressU(ADDRESS_CLAMP_TO_EDGE);
                value->setAddressV(ADDRESS_CLAMP_TO_EDGE);
                value->setMinFilter(FilterMode::FILTER_LINEAR);
                value->setMagFilter(FilterMode::FILTER_LINEAR);
                value->setMipmaps(false);
            }

            _prefilteredCubemaps.clear();
            if (_internalEnvAtlas) {
                delete _internalEnvAtlas;
                _internalEnvAtlas = nullptr;
            }

            resetSkyMesh();
        }
    }


    void Scene::setSkybox(Texture* value)
    {
        if (value != _skyboxCubeMap) {
            _skyboxCubeMap = value;
            resetSkyMesh();
        }
    }

    void Scene::setAtmosphereUniforms(const void* data, const size_t size)
    {
        if (data && size <= sizeof(_atmosphereUniforms)) {
            std::memcpy(&_atmosphereUniforms, data, size);
        }
    }

    void Scene::setPrefilteredCubemaps(const std::vector<Texture*>& cubemaps)
    {
        _prefilteredCubemaps = cubemaps;

        // DEVIATION: generatePrefilteredAtlas (from pre-filtered cubemaps) is not yet ported.
        // When needed, it will be added to EnvLighting. For now, only generateAtlas (from
        // equirectangular HDR source) is implemented.
        (void)_internalEnvAtlas;
    }
}
