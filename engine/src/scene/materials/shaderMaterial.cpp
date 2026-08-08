// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.02.2026.
//
#include "shaderMaterial.h"

#include "platform/graphics/graphicsDevice.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        const std::string kNoSource;

        const char* languageName(const ShaderLanguage language)
        {
            return language == ShaderLanguage::Glsl ? "GLSL" : "MSL";
        }
    }

    const std::string& ShaderSourceSet::forLanguage(const ShaderLanguage language) const
    {
        switch (language) {
            case ShaderLanguage::Msl:  return msl;
            case ShaderLanguage::Glsl: return glsl;
        }
        return kNoSource;
    }

    ShaderMaterial::ShaderMaterial(const std::shared_ptr<GraphicsDevice>& device, const std::string& uniqueName,
        const std::string& vertexEntry, const std::string& fragmentEntry, const std::string& sourceCode)
    {
        setName(uniqueName);
        setTransparent(false);

        if (device) {
            createShaderOverride(device, uniqueName, vertexEntry, fragmentEntry, sourceCode);
        }
    }

    ShaderMaterial::ShaderMaterial(const std::shared_ptr<GraphicsDevice>& device, const std::string& uniqueName,
        const std::string& vertexEntry, const std::string& fragmentEntry, const ShaderSourceSet& sources)
    {
        setName(uniqueName);
        setTransparent(false);

        if (!device) {
            return;
        }

        const ShaderLanguage language = device->shaderLanguage();
        const std::string& sourceCode = sources.forLanguage(language);
        if (sourceCode.empty()) {
            // The other language being present is the interesting case: the material was
            // written for one backend and this is the other one. Say which is missing
            // rather than letting the object render untextured with no explanation.
            spdlog::error("ShaderMaterial '{}' has no {} source, which is what this device "
                "requires. Shader override was not created.", uniqueName, languageName(language));
            return;
        }
        createShaderOverride(device, uniqueName, vertexEntry, fragmentEntry, sourceCode);
    }

    void ShaderMaterial::createShaderOverride(const std::shared_ptr<GraphicsDevice>& device,
        const std::string& uniqueName, const std::string& vertexEntry,
        const std::string& fragmentEntry, const std::string& sourceCode)
    {
        if (sourceCode.empty()) {
            spdlog::warn("ShaderMaterial '{}' created without source code. Shader override was not created.", uniqueName);
            return;
        }
        ShaderDefinition definition;
        definition.name = uniqueName;
        definition.vshader = vertexEntry;
        definition.fshader = fragmentEntry;
        setShaderOverride(createShader(device.get(), definition, sourceCode));
    }
}
