// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
#include "cameraFrameEffect.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <utility>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"

namespace visutwin::canvas
{
    namespace
    {
        std::atomic<uint64_t> nextEffectSerial{1};
    }

    CameraFrameEffect::CameraFrameEffect(std::string id, std::vector<FrameResource> requiredResources)
        : _id(std::move(id)), _requiredResources(std::move(requiredResources)), _serial(nextEffectSerial.fetch_add(1))
    {
    }

    CameraFrameEffect::CameraFrameEffect(std::string id, CameraFrameEffectOptions options)
        : _id(std::move(id)), _requiredResources(std::move(options.requiredResources)), _slot(options.slot),
          _slang(std::move(options.slang)), _debugViews(std::move(options.debugViews)),
          _entryPoint(std::move(options.entryPoint)), _serial(nextEffectSerial.fetch_add(1))
    {
    }

    std::string CameraFrameEffect::entryPoint() const
    {
        if (!_entryPoint.empty() || _id.empty()) {
            return _entryPoint;
        }
        std::string name = "apply" + _id;
        name[5] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[5])));
        return name;
    }

    std::string CameraFrameEffect::defineName() const
    {
        std::string out;
        for (size_t i = 0; i < _id.size(); ++i) {
            const auto c = static_cast<unsigned char>(_id[i]);
            if (i > 0 && std::isupper(c) && (std::islower(static_cast<unsigned char>(_id[i - 1])) ||
                    std::isdigit(static_cast<unsigned char>(_id[i - 1])))) {
                out += '_';
            }
            out += static_cast<char>(std::toupper(c));
        }
        return out;
    }

    void CameraFrameEffect::setDefine(const std::string& name, const std::string& value)
    {
        const auto it = std::find_if(_defines.begin(), _defines.end(),
            [&name](const auto& entry) { return entry.first == name; });
        if (value.empty()) {
            if (it != _defines.end()) {
                _defines.erase(it);
            }
        } else if (it == _defines.end()) {
            _defines.emplace_back(name, value);
        } else {
            it->second = value;
        }
    }

    ComposeEffectUniform& CameraFrameEffect::uniformSlot(const std::string& name, const int components)
    {
        auto it = std::find_if(_uniforms.begin(), _uniforms.end(),
            [&name](const auto& uniform) { return uniform.name == name; });
        if (it == _uniforms.end()) {
            _uniforms.push_back({name, components, {}});
            return _uniforms.back();
        }
        it->components = components;
        it->value.fill(0.0f);
        return *it;
    }

    void CameraFrameEffect::setUniform(const std::string& name, const float value)
    {
        uniformSlot(name, 1).value[0] = value;
    }

    void CameraFrameEffect::setTextureUniform(const std::string& name, Texture* texture)
    {
        auto it = std::find_if(_textureUniforms.begin(), _textureUniforms.end(),
            [&name](const auto& uniform) { return uniform.first == name; });
        if (it == _textureUniforms.end()) {
            _textureUniforms.emplace_back(name, texture);
        } else {
            it->second = texture;
        }
    }

    void CameraFrameEffect::setUniform(const std::string& name, const Vector2& value)
    {
        auto& slot = uniformSlot(name, 2);
        slot.value[0] = value.x;
        slot.value[1] = value.y;
    }

    void CameraFrameEffect::setUniform(const std::string& name, const Vector3& value)
    {
        auto& slot = uniformSlot(name, 3);
        slot.value[0] = value.getX();
        slot.value[1] = value.getY();
        slot.value[2] = value.getZ();
    }

    void CameraFrameEffect::setUniform(const std::string& name, const Vector4& value)
    {
        auto& slot = uniformSlot(name, 4);
        value.store(slot.value.data());
    }

    void CameraFrameEffect::setUniform(const std::string& name, const Color& value)
    {
        auto& slot = uniformSlot(name, 4);
        slot.value[0] = value.r;
        slot.value[1] = value.g;
        slot.value[2] = value.b;
        slot.value[3] = value.a;
    }

    void CameraFrameEffect::setUniform(const std::string& name, const Matrix4& value)
    {
        value.store(uniformSlot(name, 16).value.data());
    }
}
