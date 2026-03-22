// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.11.2025.
//
#include "metalVertexBufferLayout.h"

#include <sstream>

namespace visutwin::canvas
{
    std::vector<void*> MetalVertexBufferLayout::get(const std::shared_ptr<VertexFormat>& vertexFormat0,
            const std::shared_ptr<VertexFormat>& vertexFormat1)
    {
        std::string key = getKey(vertexFormat0, vertexFormat1);

        auto it = _cache.find(key);
        if (it != _cache.end()) {
            return it->second;
        }

        auto layout = create(vertexFormat0, vertexFormat1);
        _cache[key] = layout;

        return layout;
    }

    std::string MetalVertexBufferLayout::getKey(const std::shared_ptr<VertexFormat>& vertexFormat0,
        const  std::shared_ptr<VertexFormat>& vertexFormat1) const
    {
        std::ostringstream oss;
        if (vertexFormat0) {
            oss << vertexFormat0->renderingHashString();
        } else {
            oss << "null";
        }
        oss << "-";

        if (vertexFormat1) {
            oss << vertexFormat1->renderingHashString();
        } else {
            oss << "null";
        }

        return oss.str();
    }

    std::vector<void*> MetalVertexBufferLayout::create(const std::shared_ptr<VertexFormat>& vertexFormat0,
            const std::shared_ptr<VertexFormat>& vertexFormat1)
    {
        // Vector to hold vertex buffer layouts
        std::vector<void*> layout;

        // Helper lambda to process a single vertex format
        auto addFormat = [&](std::shared_ptr<VertexFormat> format) {
            if (!format)
            {
                return;
            }

            bool interleaved = format->isInterleaved();
            const char* stepMode = format->isInstancing() ? "instance" : "vertex";

            // Note: vertex buffer layout creation is stubbed — uses void* placeholders.
            // Full implementation would create GPUVertexBufferLayout structures.

            /*
            std::vector<GPUVertexAttribute> attributes;
            auto& elements = format->getElements();
            int elementCount = elements.size();

            for (int i = 0; i < elementCount; i++) {
                auto& element = elements[i];
                int location = semanticToLocation[element.getName()];

                std::string formatStr = getGpuVertexFormat(
                    element.getDataType(),
                    element.getNumComponents(),
                    element.getNormalize()
                );

                GPUVertexAttribute attribute;
                attribute.shaderLocation = location;
                attribute.offset = interleaved ? element.getOffset() : 0;
                attribute.format = formatStr;

                attributes.push_back(attribute);

                // If not interleaved, or this is the last element, create a buffer layout
                if (!interleaved || i == elementCount - 1) {
                    GPUVertexBufferLayout bufferLayout;
                    bufferLayout.attributes = attributes;
                    bufferLayout.arrayStride = element.getStride();
                    bufferLayout.stepMode = stepMode;

                    layout.push_back(bufferLayout);
                    attributes.clear();
                }
            }
            */

            // Placeholder: add nullptr for now
            layout.push_back(nullptr);
        };

        // Process both vertex formats
        addFormat(vertexFormat0);
        addFormat(vertexFormat1);

        return layout;
    }
}