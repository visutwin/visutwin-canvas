// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "core/eventHandler.h"
#include "core/shape/ray.h"
#include "xrTypes.h"

namespace visutwin::canvas
{
    class XrManager;

    /**
     * One source of XR input: a controller, a hand, or the gaze. Fires "selectstart",
     * "selectend" and "select" (a completed press of the primary action), the same three for
     * "squeeze", and "remove" when the source goes away; each with the source itself
     * (XrInputSource*).
     */
    class XrInputSource : public EventHandler
    {
    public:
        XrInputSource(XrManager* manager, const XrInputSourceState& state);

        [[nodiscard]] uint32_t id() const { return _state.id; }
        [[nodiscard]] XrHandedness handedness() const { return _state.handedness; }
        [[nodiscard]] XrTargetRayMode targetRayMode() const { return _state.targetRayMode; }
        [[nodiscard]] const std::vector<std::string>& profiles() const { return _state.profiles; }

        /// Whether the source is something held, with a pose of its own.
        [[nodiscard]] bool grip() const { return _grip; }
        [[nodiscard]] bool selecting() const { return _state.select; }
        [[nodiscard]] bool squeezing() const { return _state.squeeze; }
        [[nodiscard]] float selectValue() const { return _state.selectValue; }
        [[nodiscard]] float squeezeValue() const { return _state.squeezeValue; }
        [[nodiscard]] const std::vector<float>& buttons() const { return _state.buttons; }
        [[nodiscard]] const std::vector<float>& axes() const { return _state.axes; }

        /// The held source's pose in the session's reference space; nothing without a grip.
        [[nodiscard]] std::optional<Vector3> getLocalPosition() const;
        [[nodiscard]] std::optional<Quaternion> getLocalRotation() const;

        /// The same in the world: through the parent of the session's camera.
        [[nodiscard]] std::optional<Vector3> getPosition() const;
        [[nodiscard]] std::optional<Quaternion> getRotation() const;

        /// The held source's velocity in the world, estimated from its positions where the
        /// backend reports none.
        [[nodiscard]] std::optional<Vector3> getLinearVelocity() const;

        /// The pointing ray in the world.
        [[nodiscard]] Vector3 getOrigin() const;
        [[nodiscard]] Vector3 getDirection() const;
        [[nodiscard]] Ray ray() const { return Ray(getOrigin(), getDirection()); }

        /// Vibrates the source, where it can: intensity in [0, 1], for a duration in seconds.
        void pulse(float intensity, float seconds) const;

        /// Takes the frame's state; fires the press and release events. `dt` in seconds.
        void update(const XrInputSourceState& state, float dt);

    private:
        [[nodiscard]] Matrix4 parentTransform() const;

        XrManager* _manager = nullptr;
        XrInputSourceState _state;
        bool _grip = false;
        bool _velocityKnown = false;
        Vector3 _localLinearVelocity{0.0f, 0.0f, 0.0f};
        Vector3 _rayOrigin{0.0f, 0.0f, 0.0f};
        Vector3 _rayDirection{0.0f, 0.0f, -1.0f};
    };

    /**
     * The input sources of the XR session. Fires "add" and "remove" with the source
     * (XrInputSource*), and repeats every source's select and squeeze events.
     */
    class XrInput : public EventHandler
    {
    public:
        explicit XrInput(XrManager* manager) : _manager(manager) {}

        [[nodiscard]] const std::vector<std::shared_ptr<XrInputSource>>& inputSources() const { return _sources; }
        [[nodiscard]] XrInputSource* find(XrHandedness handedness) const;

        void update(const std::vector<XrInputSourceState>& states, float dt);
        void clear();

    private:
        XrManager* _manager = nullptr;
        std::vector<std::shared_ptr<XrInputSource>> _sources;
    };
}
