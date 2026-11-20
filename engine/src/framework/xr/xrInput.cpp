// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#include "xrInput.h"

#include <algorithm>

#include "framework/components/camera/cameraComponent.h"
#include "framework/entity.h"
#include "xrBackend.h"
#include "xrManager.h"

namespace visutwin::canvas
{
    XrInputSource::XrInputSource(XrManager* manager, const XrInputSourceState& state)
        : _manager(manager), _state(state)
    {
        // The first state only says the source exists: its presses are edges of update().
        _state.select = false;
        _state.squeeze = false;
    }

    Matrix4 XrInputSource::parentTransform() const
    {
        const CameraComponent* camera = _manager ? _manager->camera() : nullptr;
        Entity* entity = camera ? camera->entity() : nullptr;
        GraphNode* parent = entity ? entity->parent() : nullptr;
        return parent ? parent->worldTransform() : Matrix4::identity();
    }

    std::optional<Vector3> XrInputSource::getLocalPosition() const
    {
        return _grip ? std::optional(_state.grip.position) : std::nullopt;
    }

    std::optional<Quaternion> XrInputSource::getLocalRotation() const
    {
        return _grip ? std::optional(_state.grip.rotation) : std::nullopt;
    }

    std::optional<Vector3> XrInputSource::getPosition() const
    {
        if (!_grip) {
            return std::nullopt;
        }
        return parentTransform().transformPoint(_state.grip.position);
    }

    std::optional<Quaternion> XrInputSource::getRotation() const
    {
        if (!_grip) {
            return std::nullopt;
        }
        const Matrix4 world = parentTransform() * Matrix4::trs(Vector3(0.0f, 0.0f, 0.0f), _state.grip.rotation,
            Vector3(1.0f, 1.0f, 1.0f));
        return Quaternion::fromMatrix4(world);
    }

    std::optional<Vector3> XrInputSource::getLinearVelocity() const
    {
        if (!_grip || !_velocityKnown) {
            return std::nullopt;
        }
        // A direction: the parent's rotation and scale, not its translation.
        const Matrix4 parent = parentTransform();
        return parent.transformPoint(_localLinearVelocity) - parent.transformPoint(Vector3(0.0f, 0.0f, 0.0f));
    }

    Vector3 XrInputSource::getOrigin() const
    {
        return parentTransform().transformPoint(_rayOrigin);
    }

    Vector3 XrInputSource::getDirection() const
    {
        const Matrix4 parent = parentTransform();
        const Vector3 direction = parent.transformPoint(_rayDirection) - parent.transformPoint(Vector3(0.0f, 0.0f, 0.0f));
        return direction.normalized();
    }

    void XrInputSource::pulse(const float intensity, const float seconds) const
    {
        if (_manager && _manager->backend()) {
            _manager->backend()->pulse(_state.id, intensity, seconds);
        }
    }

    void XrInputSource::update(const XrInputSourceState& state, const float dt)
    {
        const bool wasSelecting = _state.select;
        const bool wasSqueezing = _state.squeeze;
        const bool hadGrip = _state.gripValid;
        const Vector3 lastPosition = _state.grip.position;

        _state = state;

        if (state.gripValid) {
            _grip = true;
            if (state.velocityValid) {
                _localLinearVelocity = state.linearVelocity;
                _velocityKnown = true;
            } else if (!hadGrip) {
                // the first pose, and the first once tracking resumes, have no previous one
                _localLinearVelocity = Vector3(0.0f, 0.0f, 0.0f);
                _velocityKnown = true;
            } else if (dt > 0.0f) {
                const Vector3 measured = (state.grip.position - lastPosition) * (1.0f / dt);
                _localLinearVelocity = Vector3::lerp(_localLinearVelocity, measured, 0.15f);
            }
        } else {
            _state.grip.position = lastPosition;
        }

        if (state.rayValid) {
            _rayOrigin = state.ray.position;
            _rayDirection = state.ray.rotation * Vector3(0.0f, 0.0f, -1.0f);
        }

        XrInput* input = _manager ? _manager->input() : nullptr;
        const auto emit = [&](const char* name) {
            fire(name, this);
            if (input) {
                input->fire(name, this);
            }
        };
        if (state.select && !wasSelecting) {
            emit("selectstart");
        } else if (!state.select && wasSelecting) {
            emit("selectend");
            emit("select");
        }
        if (state.squeeze && !wasSqueezing) {
            emit("squeezestart");
        } else if (!state.squeeze && wasSqueezing) {
            emit("squeezeend");
            emit("squeeze");
        }
    }

    XrInputSource* XrInput::find(const XrHandedness handedness) const
    {
        for (const auto& source : _sources) {
            if (source->handedness() == handedness) {
                return source.get();
            }
        }
        return nullptr;
    }

    void XrInput::update(const std::vector<XrInputSourceState>& states, const float dt)
    {
        // Sources the frame no longer reports go first. A handler may hold the pointer for
        // the length of its call, so the source is kept alive until its events have fired.
        for (size_t i = 0; i < _sources.size();) {
            const uint32_t id = _sources[i]->id();
            const bool present = std::ranges::any_of(states, [id](const auto& state) { return state.id == id; });
            if (present) {
                ++i;
                continue;
            }
            const std::shared_ptr<XrInputSource> removed = _sources[i];
            _sources.erase(_sources.begin() + static_cast<std::ptrdiff_t>(i));
            removed->fire("remove", removed.get());
            fire("remove", removed.get());
        }

        for (const auto& state : states) {
            XrInputSource* source = nullptr;
            for (const auto& existing : _sources) {
                if (existing->id() == state.id) {
                    source = existing.get();
                    break;
                }
            }
            if (!source) {
                _sources.push_back(std::make_shared<XrInputSource>(_manager, state));
                source = _sources.back().get();
                fire("add", source);
            }
            source->update(state, dt);
        }
    }

    void XrInput::clear()
    {
        while (!_sources.empty()) {
            const std::shared_ptr<XrInputSource> removed = _sources.back();
            _sources.pop_back();
            removed->fire("remove", removed.get());
            fire("remove", removed.get());
        }
    }
}
