// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
// A procedurally generated sky: an analytic Preetham daylight model with a night sky of
// stars, a twilight band and a moon, drawn per pixel on the scene's sky mesh, and baked
// into the scene's environment atlas for ambient light and reflections. The bake runs only
// when the sun has moved more than a threshold or a sky parameter changed, so a running
// day / night cycle stays affordable. A linked directional light follows the sun (its
// direction, a warm-to-white colour and its intensity), and crossfades to a dim moonlight
// once the sun is below the horizon.
//
// The scene's sky material and environment atlas belong to the script while it lives.
//
#pragma once

#include <array>
#include <memory>

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "framework/destroyWatch.h"
#include "framework/script/script.h"
#include "framework/script/scriptRegistry.h"

namespace visutwin::canvas
{
    class Entity;
    class GraphicsDevice;
    class Material;
    class RenderTarget;
    class Scene;
    class Texture;

    /// The uniform block of the sky programs (vtproceduralsky.slang: ProceduralSkyData).
    struct ProceduralSkyBlock
    {
        float sunDir[4] = {};      // xyz: towards the sun, sky space; w: sun intensity
        float betaR[4] = {};       // xyz: Rayleigh coefficient; w: Mie anisotropy
        float betaM[4] = {};       // xyz: Mie coefficient; w: luminance
        float night[4] = {};       // night blend, night brightness, star brightness, star density
        float nightColor[4] = {};  // xyz: night base colour; w: star size
        float moonDir[4] = {};     // xyz: towards the moon, sky space; w: moon radius (radians)
        float moonColor[4] = {};   // xyz: moon colour; w: moon glow
        float output[4] = {};      // sky intensity, exposure, tone mapping, flags
        float extra[4] = {};       // x: twilight glow, y: cube face (the bake)
    };
    static_assert(sizeof(ProceduralSkyBlock) == 144, "ProceduralSkyBlock mirrors ProceduralSkyData");

    class ProceduralSky : public Script
    {
    public:
        SCRIPT_NAME("ProceduralSky")

        ProceduralSky();
        ~ProceduralSky() override;

        void initialize() override;
        void update(float dt) override;

        /// The sun's compass direction and its elevation above the horizon, in degrees.
        /// A negative elevation puts the sun below the horizon (night).
        void setAzimuth(const float degrees) { _azimuth = degrees; }
        [[nodiscard]] float azimuth() const { return _azimuth; }
        void setElevation(const float degrees) { _elevation = degrees; }
        [[nodiscard]] float elevation() const { return _elevation; }

        /// Turns the whole sky, the sun and the moon with it, around Y, in degrees, without
        /// changing the azimuth and elevation a time of day maps to.
        void setRotation(const float degrees) { _rotation = degrees; }
        [[nodiscard]] float rotation() const { return _rotation; }

        /// Haziness (1-20): a milkier sky and a larger glow around the sun.
        void setTurbidity(const float value) { _turbidity = value; }
        [[nodiscard]] float turbidity() const { return _turbidity; }
        /// Rayleigh scattering (0-4): the blueness of the sky.
        void setRayleigh(const float value) { _rayleigh = value; }
        [[nodiscard]] float rayleigh() const { return _rayleigh; }
        /// Mie scattering (0-0.1): the brightness of the glow around the sun.
        void setMieCoefficient(const float value) { _mieCoefficient = value; }
        [[nodiscard]] float mieCoefficient() const { return _mieCoefficient; }
        /// Mie anisotropy (0-1): how tightly the glow hugs the sun.
        void setMieDirectionalG(const float value) { _mieDirectionalG = value; }
        [[nodiscard]] float mieDirectionalG() const { return _mieDirectionalG; }
        /// The HDR scale of the sky radiance.
        void setLuminance(const float value) { _luminance = value; }
        [[nodiscard]] float luminance() const { return _luminance; }

        /// The face size of the cube the lighting is baked from (the sky is low frequency).
        void setLightingResolution(const int value) { _lightingResolution = value; }
        [[nodiscard]] int lightingResolution() const { return _lightingResolution; }
        /// The size of the environment atlas the scene is lit by.
        void setAtlasSize(const int value) { _atlasSize = value; }
        [[nodiscard]] int atlasSize() const { return _atlasSize; }
        /// Degrees the sun moves before the lighting is baked again.
        void setLightingThreshold(const float degrees) { _lightingThreshold = degrees; }
        [[nodiscard]] float lightingThreshold() const { return _lightingThreshold; }

        /// A directional light kept in step with the sun. Its intensity when it is first
        /// updated is the daytime peak.
        void setSunLight(Entity* light);
        [[nodiscard]] Entity* sunLight() const { return _sunLight; }

        /// The moonlight the sun light fades to below the horizon: its colour, intensity
        /// and the world direction it comes from.
        void setMoonColor(const Color& color) { _moonColor = color; }
        [[nodiscard]] const Color& moonColor() const { return _moonColor; }
        void setMoonIntensity(const float value) { _moonIntensity = value; }
        [[nodiscard]] float moonIntensity() const { return _moonIntensity; }
        void setMoonDirection(const Vector3& direction) { _moonDirection = direction; }
        [[nodiscard]] const Vector3& moonDirection() const { return _moonDirection; }

        /// The night sky: its base colour and brightness, the stars (brightness, the share of
        /// sky cells holding one, a size multiplier), the twilight glow, and the moon disk
        /// (angular radius in degrees, brightness).
        void setNightColor(const Color& color) { _nightColor = color; }
        [[nodiscard]] const Color& nightColor() const { return _nightColor; }
        void setNightBrightness(const float value) { _nightBrightness = value; }
        [[nodiscard]] float nightBrightness() const { return _nightBrightness; }
        void setStarBrightness(const float value) { _starBrightness = value; }
        [[nodiscard]] float starBrightness() const { return _starBrightness; }
        void setStarDensity(const float value) { _starDensity = value; }
        [[nodiscard]] float starDensity() const { return _starDensity; }
        void setStarSize(const float value) { _starSize = value; }
        [[nodiscard]] float starSize() const { return _starSize; }
        void setTwilightGlow(const float value) { _twilightGlow = value; }
        [[nodiscard]] float twilightGlow() const { return _twilightGlow; }
        void setMoonSize(const float degrees) { _moonSize = degrees; }
        [[nodiscard]] float moonSize() const { return _moonSize; }
        void setMoonGlow(const float value) { _moonGlow = value; }
        [[nodiscard]] float moonGlow() const { return _moonGlow; }

        /// The atlas the scene is lit by (null before the first update).
        [[nodiscard]] Texture* envAtlas() const { return _envAtlas.get(); }

    private:
        class SkyMaterial;

        Vector3 computeSunDir() const;
        Vector3 computeMoonDir() const;
        void updateAtmosphere();
        void updateSunLight();
        void bakeLighting();
        void releaseScene();

        float _azimuth = 0.0f;
        float _elevation = 25.0f;
        float _rotation = 0.0f;
        float _turbidity = 2.0f;
        float _rayleigh = 1.5f;
        float _mieCoefficient = 0.005f;
        float _mieDirectionalG = 0.8f;
        float _luminance = 1.0f;
        int _lightingResolution = 64;
        int _atlasSize = 256;
        float _lightingThreshold = 0.5f;

        Entity* _sunLight = nullptr;
        DestroyWatch _sunLightWatch;
        float _baseSunIntensity = -1.0f;   // negative until captured

        Color _moonColor = Color(0.792f, 0.918f, 1.0f, 1.0f);
        float _moonIntensity = 1.0f;
        Vector3 _moonDirection = Vector3(-1.53f, 0.85f, 0.35f);
        Color _nightColor = Color(0.114f, 0.247f, 0.408f, 1.0f);
        float _nightBrightness = 0.052f;
        float _starBrightness = 0.05f;
        float _starDensity = 0.8f;
        float _starSize = 0.5f;
        float _twilightGlow = 0.52f;
        float _moonSize = 0.6f;
        float _moonGlow = 3.0f;

        std::shared_ptr<GraphicsDevice> _device;
        std::weak_ptr<Scene> _scene;
        std::shared_ptr<SkyMaterial> _material;
        std::shared_ptr<Texture> _lightingCube;
        std::array<std::shared_ptr<RenderTarget>, 6> _faceTargets;
        std::unique_ptr<Texture> _envAtlas;

        ProceduralSkyBlock _block;
        Vector3 _sunDir;            // world space, towards the sun
        Vector3 _skyDir;            // sky space (world with X negated)
        Vector3 _bakedDir = Vector3(0.0f, -1.0f, 0.0f);
        std::array<float, 7> _bakedParams = {};
        bool _baked = false;
    };
}

REGISTER_SCRIPT(visutwin::canvas::ProceduralSky, "ProceduralSky")
