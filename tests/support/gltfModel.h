// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Building a glTF model in memory, and loading it through both of the parser's model
// entry points.
//
// addAccessor() appends float data to the model's one buffer behind a view and an
// accessor of its own. forEachLoadPath() loads a model twice — through createFromModel,
// and through prepareFromModel + createFromPrepared (what loadAsync runs) — from a fresh
// copy each time, and hands each container to the caller; the container lives for the
// call only.

#pragma once

#include <tiny_gltf.h>

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas::test
{
    /// The min / max a VEC3 accessor declares (POSITION requires them).
    struct Vec3Bounds
    {
        std::vector<double> min;
        std::vector<double> max;
    };

    /// Appends `values` (`count` elements of `type`) as FLOAT data to `bytes`, the model's
    /// buffer 0, with its own buffer view and accessor; returns the accessor's index.
    /// `vec3Bounds` is written as min / max on every VEC3 accessor when given.
    inline int addAccessor(tinygltf::Model& model, std::vector<unsigned char>& bytes, const std::vector<float>& values,
        const int count, const int type, const std::optional<Vec3Bounds>& vec3Bounds = std::nullopt)
    {
        const size_t at = bytes.size();
        bytes.resize(at + values.size() * sizeof(float));
        std::memcpy(bytes.data() + at, values.data(), values.size() * sizeof(float));

        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = at;
        view.byteLength = values.size() * sizeof(float);
        model.bufferViews.push_back(view);

        tinygltf::Accessor accessor;
        accessor.bufferView = static_cast<int>(model.bufferViews.size()) - 1;
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        accessor.count = static_cast<size_t>(count);
        accessor.type = type;
        if (vec3Bounds && type == TINYGLTF_TYPE_VEC3) {
            accessor.minValues = vec3Bounds->min;
            accessor.maxValues = vec3Bounds->max;
        }
        model.accessors.push_back(accessor);
        return static_cast<int>(model.accessors.size()) - 1;
    }

    enum class LoadPath
    {
        CreateFromModel,
        Prepared,   ///< prepareFromModel + createFromPrepared
    };

    /// Loads `buildModel()` through each LoadPath in turn and calls
    /// `visit(GlbContainerResource* container, LoadPath path)` with the result (null when
    /// the load failed). `visit` may return bool: false stops, and the call returns false.
    /// `ktx2Target` is what prepareFromModel transcodes KHR_texture_basisu images to.
    template <typename BuildModel, typename Visit>
    bool forEachLoadPath(BuildModel&& buildModel, const std::shared_ptr<GraphicsDevice>& device,
        const std::string& debugName, Visit&& visit, const PixelFormat ktx2Target = PixelFormat::PIXELFORMAT_RGBA8)
    {
        for (const LoadPath path : {LoadPath::CreateFromModel, LoadPath::Prepared}) {
            tinygltf::Model model = buildModel();
            std::unique_ptr<GlbContainerResource> container;
            if (path == LoadPath::CreateFromModel) {
                container = GlbParser::createFromModel(model, device, debugName);
            } else {
                auto prepared = GlbParser::prepareFromModel(model, ktx2Target, debugName);
                container = GlbParser::createFromPrepared(model, std::move(prepared), device, debugName);
            }
            if constexpr (std::is_same_v<std::invoke_result_t<Visit&, GlbContainerResource*, LoadPath>, bool>) {
                if (!visit(container.get(), path)) {
                    return false;
                }
            } else {
                visit(container.get(), path);
            }
        }
        return true;
    }
}
