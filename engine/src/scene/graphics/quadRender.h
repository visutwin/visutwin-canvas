// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <memory>

#include "core/math/vector4.h"
#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    class Shader;

    class QuadRender
    {
    public:
        explicit QuadRender(const std::shared_ptr<Shader>& shader);
        ~QuadRender() = default;

        void render(const Vector4* viewport = nullptr, const Vector4* scissor = nullptr) const;

    private:
        std::shared_ptr<Shader> _shader;
    };
}
