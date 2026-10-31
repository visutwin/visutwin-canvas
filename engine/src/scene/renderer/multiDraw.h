// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Issues a mesh instance's draw, or its sub-draws when it has a multi-draw list for the camera
// (MeshInstance::setMultiDraw). The sub-draws keep the vertex buffers bound between them (only the
// last clears the device's list; with nothing drawn the instance slot is unbound); each binds its
// own state, which the backends skip where it is unchanged. A skinned or morphed draw ignores the list: its palette and morph buffers are handed
// to the first draw only.
//
#pragma once

#include <memory>

#include "platform/graphics/graphicsDevice.h"
#include "scene/drawCommands.h"

namespace visutwin::canvas
{
    inline void drawWithCommands(GraphicsDevice& device, const Primitive& primitive,
        const std::shared_ptr<IndexBuffer>& indexBuffer, const int numInstances, const DrawCommands* commands)
    {
        if (!commands) {
            device.draw(primitive, indexBuffer, numInstances, -1, true, true);
            return;
        }
        // The last sub-draw that draws anything clears the device's vertex buffer list.
        int last = -1;
        for (int i = 0; i < commands->count(); ++i) {
            if (commands->command(i).count > 0 && commands->command(i).instanceCount > 0) {
                last = i;
            }
        }
        if (last < 0) {
            // Nothing drawn: an instance buffer left bound would follow the next draw.
            device.setVertexBuffer(nullptr, 5);
            return;
        }
        for (int i = 0; i <= last; ++i) {
            const DrawCommands::Command& command = commands->command(i);
            if (command.count > 0 && command.instanceCount > 0) {
                device.draw(commands->primitive(primitive, i), indexBuffer, command.instanceCount, -1, true,
                    i == last);
            }
        }
    }
}
