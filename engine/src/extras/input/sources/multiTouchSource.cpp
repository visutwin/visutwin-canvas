// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "multiTouchSource.h"

#include <cmath>

#include "framework/engine.h"
#include "platform/input/touchDevice.h"
#include "sourceEvents.h"

namespace visutwin::canvas
{
    MultiTouchSource::MultiTouchSource()
        : InputSource({{"touch", 2}, {"count", 1}, {"pinch", 1}})
    {
    }

    MultiTouchSource::~MultiTouchSource()
    {
        detach();
    }

    MultiTouchSource::Pointer* MultiTouchSource::find(const int64_t id)
    {
        for (auto& p : _pointers) {
            if (p.id == id) {
                return &p;
            }
        }
        return nullptr;
    }

    std::pair<float, float> MultiTouchSource::midPoint() const
    {
        if (_pointers.size() < 2) {
            return {0.0f, 0.0f};
        }
        const Pointer& a = _pointers[0];
        const Pointer& b = _pointers[1];
        return {b.x + (a.x - b.x) * 0.5f, b.y + (a.y - b.y) * 0.5f};
    }

    float MultiTouchSource::pinchDist() const
    {
        if (_pointers.size() < 2) {
            return 0.0f;
        }
        const float dx = _pointers[0].x - _pointers[1].x;
        const float dy = _pointers[0].y - _pointers[1].y;
        return std::sqrt(dx * dx + dy * dy);
    }

    void MultiTouchSource::onDown(const int64_t id, const float x, const float y)
    {
        if (Pointer* p = find(id)) {
            p->x = x;
            p->y = y;
        } else {
            _pointers.push_back({id, x, y});
        }
        deltas.at("count").append({1.0f});
        if (_pointers.size() > 1) {
            const auto [mx, my] = midPoint();
            _pointerX = mx;
            _pointerY = my;
            _pinchDist = pinchDist();
        }
    }

    void MultiTouchSource::onMove(const int64_t id, const float x, const float y)
    {
        Pointer* p = find(id);
        if (!p) {
            return;
        }
        const float movementX = x - p->x;
        const float movementY = y - p->y;
        p->x = x;
        p->y = y;

        if (_pointers.size() > 1) {
            const auto [mx, my] = midPoint();
            deltas.at("touch").append({mx - _pointerX, my - _pointerY});
            _pointerX = mx;
            _pointerY = my;

            const float dist = pinchDist();
            if (_pinchDist > 0.0f) {
                deltas.at("pinch").append({_pinchDist - dist});
            }
            _pinchDist = dist;
        } else {
            deltas.at("touch").append({movementX, movementY});
        }
    }

    void MultiTouchSource::onUp(const int64_t id)
    {
        if (!find(id)) {
            return;
        }
        std::erase_if(_pointers, [id](const Pointer& p) { return p.id == id; });
        deltas.at("count").append({-1.0f});
        if (_pointers.size() < 2) {
            _pinchDist = -1.0f;
        }
        _pointerX = 0.0f;
        _pointerY = 0.0f;
    }

    void MultiTouchSource::attach(Engine* engine)
    {
        InputSource::attach(engine);
        subscribeTouchPointers(engine, _handles,
            [this](const int64_t id, const float x, const float y) { onDown(id, x, y); },
            [this](const int64_t id, const float x, const float y) { onMove(id, x, y); },
            [this](const int64_t id) { onUp(id); });
    }

    void MultiTouchSource::detach()
    {
        if (!_engine) {
            return;
        }
        releaseHandles(_handles);
        _pointers.clear();
        _pinchDist = -1.0f;
        InputSource::detach();
    }
}
