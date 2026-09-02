// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "quadRender.h"

#include "platform/graphics/shader.h"

#include <type_traits>

namespace visutwin::canvas
{
    QuadRender::QuadRender(const std::shared_ptr<Shader>& shader)
        : _shader(shader)
    {
    }

    void QuadRender::setTexture(const size_t slot, Texture* texture)
    {
        if (slot < _textures.size()) {
            _textures[slot] = texture;
        }
    }

    void QuadRender::setUniformData(const void* data, const size_t size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        _uniformData.assign(bytes, bytes + size);
    }

    void QuadRender::render(const Vector4* viewport, const Vector4* scissor) const
    {
        if (!_shader) {
            return;
        }

        auto* const rawDevice = _shader ? _shader->graphicsDevice() : nullptr;
        if (!rawDevice) {
            return;
        }

        const auto* const device = rawDevice;

        float oldVx = 0.0f;
        float oldVy = 0.0f;
        float oldVw = 0.0f;
        float oldVh = 0.0f;
        int oldSx = 0;
        int oldSy = 0;
        int oldSw = 0;
        int oldSh = 0;

        if (viewport) {
            oldVx = device->vx();
            oldVy = device->vy();
            oldVw = device->vw();
            oldVh = device->vh();
            oldSx = device->sx();
            oldSy = device->sy();
            oldSw = device->sw();
            oldSh = device->sh();

            const Vector4 effectiveScissor = scissor ? *scissor : *viewport;
            rawDevice->setViewport(viewport->getX(), viewport->getY(), viewport->getZ(), viewport->getW());
            rawDevice->setScissor(static_cast<int>(effectiveScissor.getX()), static_cast<int>(effectiveScissor.getY()),
                                  static_cast<int>(effectiveScissor.getZ()), static_cast<int>(effectiveScissor.getW()));
        }

        rawDevice->setVertexBuffer(rawDevice->quadVertexBuffer());
        rawDevice->setShader(_shader);
        for (size_t slot = 0; slot < _textures.size(); ++slot) {
            if (_textures[slot]) {
                rawDevice->setQuadTextureBinding(slot, _textures[slot]);
            }
        }
        if (!_uniformData.empty()) {
            rawDevice->setQuadUniformData(_uniformData.data(), _uniformData.size());
        }
        rawDevice->setQuadRenderActive(true);

        // One oversized triangle covering the screen — see quadVertexBuffer().
        Primitive quadPrimitive;
        quadPrimitive.type = PRIMITIVE_TRIANGLES;
        quadPrimitive.base = 0;
        quadPrimitive.baseVertex = 0;
        quadPrimitive.count = 3;
        quadPrimitive.indexed = false;

        rawDevice->draw(quadPrimitive, nullptr, 1, -1, true, true);
        rawDevice->setQuadRenderActive(false);
        rawDevice->clearQuadTextureBindings();
        rawDevice->clearQuadUniformData();

        if (viewport) {
            rawDevice->setViewport(oldVx, oldVy, oldVw, oldVh);
            rawDevice->setScissor(oldSx, oldSy, oldSw, oldSh);
        }
    }
}
