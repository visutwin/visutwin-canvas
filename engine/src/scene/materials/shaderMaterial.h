// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.02.2026.
//
#pragma once

#include <memory>
#include <string>

#include "material.h"
#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    class GraphicsDevice;

    /**
     * @brief Shader source written once per backend language.
     *
     * A backend accepts exactly one language (GraphicsDevice::shaderLanguage()) and
     * rejects the others outright, so a material that must run on both backends
     * carries both strings. Supplying only one is legal — the material simply has no
     * shader on any other backend, and says so.
     *
     * MSL uses the named entry points from the ShaderMaterial constructor. GLSL is
     * compiled once per stage from a single string, with VT_VERTEX_SHADER /
     * VT_FRAGMENT_SHADER defined in turn, so it guards its stages with #ifdef and
     * always names its entry point main().
     */
    struct ShaderSourceSet
    {
        std::string msl;
        std::string glsl;

        /// Source for `language`, or an empty string when this set has none.
        [[nodiscard]] const std::string& forLanguage(ShaderLanguage language) const;
    };

    /**
     * @brief Custom material with user-defined vertex and fragment shader entry points.
     * @ingroup group_scene_materials
     *
     * ShaderMaterial allows injecting arbitrary shader source code, bypassing the
     * standard PBR chunk composition pipeline. Prefer the ShaderSourceSet overload —
     * the single-string form is MSL-only and produces no shader on a Vulkan device.
     */
    class ShaderMaterial : public Material
    {
    public:
        ShaderMaterial(const std::shared_ptr<GraphicsDevice>& device, const std::string& uniqueName,
            const std::string& vertexEntry = "vertexShader", const std::string& fragmentEntry = "fragmentShader",
            const std::string& sourceCode = "");

        ShaderMaterial(const std::shared_ptr<GraphicsDevice>& device, const std::string& uniqueName,
            const std::string& vertexEntry, const std::string& fragmentEntry,
            const ShaderSourceSet& sources);

    private:
        void createShaderOverride(const std::shared_ptr<GraphicsDevice>& device,
            const std::string& uniqueName, const std::string& vertexEntry,
            const std::string& fragmentEntry, const std::string& sourceCode);
    };
}
