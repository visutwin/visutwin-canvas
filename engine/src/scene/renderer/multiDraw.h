// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Issues a mesh instance's draw, or its sub-draws when it has a multi-draw list for the camera
// (MeshInstance::setMultiDraw), through GraphicsDevice::multiDraw: the live sub-draws that draw
// something, with the state bound once and the vertex buffers kept until the last (with nothing
// drawn the instance slot is unbound). A skinned or morphed draw ignores the list: its palette
// and morph buffers are handed to the first draw only.
//
#pragma once

#include <memory>
#include <vector>

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
        // The sub-draws that draw something; reused between calls, as the forward pass draws many.
        static thread_local std::vector<MultiDrawCommand> live;
        live.clear();
        for (int i = 0; i < commands->count(); ++i) {
            if (commands->command(i).count > 0 && commands->command(i).instanceCount > 0) {
                live.push_back(commands->command(i));
            }
        }
        if (live.empty()) {
            // Nothing drawn: an instance buffer left bound would follow the next draw.
            device.setVertexBuffer(nullptr, 5);
            return;
        }
        device.multiDraw(primitive, indexBuffer, live);
    }
}
