// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.07.2026.
//
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Engine;
    class Entity;
    class RenderTarget;
    class Texture;

    /**
     * @brief Runtime scene-capture reflection probe (dynamic cubemap bake).
     * @ingroup group_framework
     *
     * Captures the live scene into a cubemap by rendering it six times — once per
     * cube face, from the probe position, with a 90° camera — into the six faces
     * of a mipmapped color cubemap, then regenerates the cube mip chain and
     * installs it as the scene reflection probe (`Scene::setReflectionProbe`).
     * This replaces the hand-authored / static cubemap the probe otherwise needs.
     *
     * The six face cameras are ordinary `CameraComponent`s pointed along
     * ±X/±Y/±Z (reusing `LightCamera::pointLightRotations`). Because the layer
     * composition renders cameras in construction order, **construct the probe
     * BEFORE the main camera** so its faces are captured before the main camera
     * samples the probe.
     *
     * The captured faces hold tonemapped/gamma-encoded LDR (the normal forward
     * output), which the probe shader sRGB-decodes — matching the existing
     * static-cubemap path. DEVIATIONS: roughness uses hardware trilinear cube
     * mips (no GGX per-level prefilter — same approximation the probe shader
     * already relies on); a single probe; directional-shadow cascades are fit
     * only for the presentation camera, so probe faces may miss directional
     * shadows.
     *
     * Usage per frame:
     *   // (construct probe, then the main camera)
     *   engine->render();
     *   probe->update();     // after render: regenerate mips + (re)install probe
     */
    class ReflectionProbe
    {
    public:
        explicit ReflectionProbe(Engine* engine, int faceSize = 128);
        ~ReflectionProbe();

        /// World-space capture origin (probe center). Also the box-projection center.
        void setPosition(const Vector3& worldPosition);

        /// Parallax-correction box in world space and whether to box-project.
        void setBox(const Vector3& boxMin, const Vector3& boxMax, bool boxProjection = true);

        void setIntensity(float intensity);

        /// Near/far clip of the six capture cameras (default 0.1 / 500).
        void setNearFar(float nearClip, float farClip);

        /// Which layers the probe captures (default WORLD + SKYBOX).
        void setLayers(const std::vector<int>& layers);

        /// true (default): recapture every frame (reflections track the scene).
        /// false: capture once, then disable the face cameras (static bake).
        void setDynamic(bool dynamic);

        /// Force a (re)capture on the next update, even in one-shot mode.
        void requestBake();

        /// Call once per frame AFTER engine->render(): regenerate the cube mip
        /// chain from the freshly-rendered faces and install the probe.
        void update();

        Texture* cubemap() const { return _cube.get(); }

    private:
        void applyProbe();
        void setCapturingEnabled(bool enabled);

        Engine* _engine = nullptr;
        int _faceSize = 128;

        std::shared_ptr<Texture> _cube;
        std::vector<std::shared_ptr<RenderTarget>> _faceTargets;

        Entity* _node = nullptr;
        std::array<Entity*, 6> _faceEntities{};
        std::array<CameraComponent*, 6> _faceCameras{};

        Vector3 _position{0.0f, 0.0f, 0.0f};
        Vector3 _boxMin{-1.0f, -1.0f, -1.0f};
        Vector3 _boxMax{1.0f, 1.0f, 1.0f};
        bool _boxProjection = true;
        float _intensity = 1.0f;

        bool _dynamic = true;
        bool _capturing = true;   // face cameras currently enabled
        bool _installed = false;  // probe installed on the scene
    };
}
