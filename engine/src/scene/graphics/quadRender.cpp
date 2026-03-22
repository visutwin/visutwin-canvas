// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "quadRender.h"

#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    QuadRender::QuadRender(const std::shared_ptr<Shader>& shader)
        : _shader(shader)
    {
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
        // DEVIATION: UBO / bind-group setup from upstream QuadRender is not implemented in this backend yet.
        rawDevice->setQuadRenderActive(true);

        Primitive quadPrimitive;
        quadPrimitive.type = PRIMITIVE_TRISTRIP;
        quadPrimitive.base = 0;
        quadPrimitive.baseVertex = 0;
        quadPrimitive.count = 4;
        quadPrimitive.indexed = false;

        rawDevice->draw(quadPrimitive, nullptr, 1, -1, true, true);
        rawDevice->setQuadRenderActive(false);
        rawDevice->clearQuadTextureBindings();

        if (viewport) {
            rawDevice->setViewport(oldVx, oldVy, oldVw, oldVh);
            rawDevice->setScissor(oldSx, oldSy, oldSw, oldSh);
        }
    }
}
