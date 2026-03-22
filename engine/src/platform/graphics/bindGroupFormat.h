// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2025.
//
#pragma once

#include <string>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;

    /**
     * A base class to describe the format of the resource for BindGroupFormat.
     */
    class BindBaseFormat {
    public:
        BindBaseFormat(const std::string& name, uint32_t visibility): _name(name), _visibility(visibility) {}

    protected:
        std::string _name;
        uint32_t _visibility;
    };

    /**
     * BindGroupFormat is a data structure that defines the layout of resources (buffers, textures,
     * samplers) used by rendering or compute shaders. It describes the binding points for each
     * resource type, and the visibility of these resources in the shader stages.
     */
    class BindGroupFormat {
    public:
        BindGroupFormat(GraphicsDevice* graphicsDevice, const std::vector<BindBaseFormat*>& formats);

        uint32_t key() const { return _id; }

    private:
        uint32_t _id;

        GraphicsDevice* _device;
    };
}
