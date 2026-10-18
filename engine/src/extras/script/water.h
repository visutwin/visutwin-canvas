// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
// A configurable water surface on the entity the script is attached to, whose render component
// holds a roughly horizontal plane-like mesh (tessellated, for the geometry waves to show):
//
// - a planar reflection of the scene, a reflection of the sky alone, or none;
// - a planar refraction of the scene, or plain transparency;
// - oblique clipping of the reflected and refracted scene by the water plane;
// - depth effects from the scene depth grab, which the script requests on the main camera:
//   absorption with depth, a soft shoreline and shoreline foam;
// - Gerstner waves displacing the mesh and its lighting, the ones shorter than a pixel faded
//   out into the roughness of the sun's highlight;
// - the diffuse and specular light of one directional light;
// - seen from under the water, the Snell window and the underwater fog.
//
// Other materials take the underwater effects through applyUnderwaterEffects: the scene's fog
// limited to their fragments below the water, and caustics on them, fading with depth. Those
// read the scene's user shader data, vectors 0-2 and the texture (Scene::setUserShaderData),
// which belong to the script while it lives.
//
// Put the water on a layer of its own drawn after the skybox: the internal reflection and
// refraction cameras render every layer of the main camera except the water's own, the UI and
// the depth layer.
//
// DEVIATIONS:
// - the underwater effects of another material are a chunk override of that material
//   (applyUnderwaterEffects), where upstream installs shader chunks globally and a material
//   opts in with a define.
// - the internal cameras use linear tone mapping while the main camera renders through a camera
//   frame (whose own tone mapping is the camera's here), as upstream's do there.
//
#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "framework/destroyWatch.h"
#include "framework/script/script.h"
#include "framework/script/scriptRegistry.h"

namespace visutwin::canvas
{
    class Entity;
    class Material;
    class MeshInstance;
    class PlanarRenderer;
    class Scene;
    class Texture;

    enum class WaterReflection
    {
        Planar,   // the scene mirrored by the water plane
        Sky,      // the scene's environment atlas only
        None
    };

    class Water : public Script
    {
    public:
        SCRIPT_NAME("Water")

        Water();
        ~Water() override;

        void initialize() override;
        void postUpdate(float dt) override;

        /// The entity of the scene's main camera (required).
        void setCameraEntity(Entity* entity);
        /// The entity of a directional light lighting the water; none leaves it unlit by the sun.
        void setLightEntity(Entity* entity);

        /// Gives `material` the underwater effects: the scene fog only below the water level
        /// (`heightFog`) and the caustics (`caustics`); both false removes the override. The
        /// material keeps them while any Water script provides their values.
        static void applyUnderwaterEffects(Material& material, bool heightFog, bool caustics);

        // ---- The settings, upstream's attributes. Read every frame.

        /// The ripple normal map (two scrolling layers); a flat one when null. Borrowed.
        Texture* normalMap = nullptr;
        /// The caustics pattern; the normal map when null. Borrowed.
        Texture* causticsMap = nullptr;

        WaterReflection reflectionSource = WaterReflection::Planar;
        bool refraction = true;
        bool obliqueClipping = true;
        float clipBias = 0.1f;
        float textureScale = 0.5f;
        bool depthEffects = true;

        Color shallowColor = Color(0.6f, 0.9f, 0.85f, 1.0f);
        Color deepColor = Color(0.03f, 0.16f, 0.24f, 1.0f);
        float depthFade = 3.0f;
        float shoreSoftness = 0.3f;
        float opacity = 0.8f;

        float rippleTiling = 0.1f;
        float rippleSpeed = 0.05f;
        float bumpiness = 0.5f;
        float distortion = 0.03f;
        float fresnelPower = 5.0f;
        float reflectionStrength = 1.0f;
        float skyBlur = 0.0f;
        float specularPower = 256.0f;
        float specularIntensity = 1.0f;
        float diffuseIntensity = 0.35f;

        bool foam = true;
        Color foamColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
        float foamDepth = 0.5f;

        float causticsTiling = 0.09f;
        float causticsSpeed = 0.04f;
        float causticsStrength = 1.5f;
        float causticsDepth = 3.0f;
        float snellWindow = 0.35f;

        bool waves = false;
        float waveAmplitude = 0.15f;
        float waveLength = 8.0f;
        float waveSpeed = 1.0f;
        float waveSteepness = 0.4f;
        float waveFiltering = 1.0f;
        float waveDirection = 0.0f;
        float swellAmplitude = 0.25f;
        float swellLength = 35.0f;
        float swellSpeed = 1.0f;
        float swellDirection = 40.0f;

    private:
        class WaterMaterial;

        struct PlanarCamera
        {
            Entity* entity = nullptr;
            PlanarRenderer* renderer = nullptr;
        };

        bool setUp();
        PlanarCamera createPlanarCamera(const char* name, bool reflection);
        void updatePlanarCamera(PlanarCamera& camera, const Vector3& planePoint, const Vector3& planeNormal);
        void enableCameras(bool value);
        void requestDepthMap(bool value);
        void releaseAll();

        Entity* _cameraEntity = nullptr;
        DestroyWatch _cameraWatch;
        Entity* _lightEntity = nullptr;
        DestroyWatch _lightWatch;

        std::shared_ptr<WaterMaterial> _material;
        std::vector<std::pair<MeshInstance*, Material*>> _originalMaterials;
        std::shared_ptr<Texture> _fallbackNormalMap;
        std::weak_ptr<Scene> _scene;
        PlanarCamera _reflection;
        DestroyWatch _reflectionWatch;
        PlanarCamera _refraction;
        DestroyWatch _refractionWatch;
        bool _depthMapRequested = false;
        bool _setUpReported = false;
        float _time = 0.0f;
    };
}

REGISTER_SCRIPT(visutwin::canvas::Water, "Water")
