// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
#include "composeEffects.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

#include "scene/graphics/cameraFrameEffect.h"
#include "platform/graphics/shaderBindings.h"
#include "scene/graphics/composeShaders.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr std::array<ComposeSlot, 4> kSlots = {ComposeSlot::Scene, ComposeSlot::Hdr, ComposeSlot::Ldr,
            ComposeSlot::Output};

        const char* slotName(const ComposeSlot slot)
        {
            switch (slot) {
            case ComposeSlot::Scene: return "scene";
            case ComposeSlot::Hdr: return "hdr";
            case ComposeSlot::Ldr: return "ldr";
            case ComposeSlot::Output: return "output";
            default: return "none";
            }
        }

        std::string capitalized(std::string name)
        {
            if (!name.empty()) {
                name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            }
            return name;
        }

        int rowsOf(const ComposeEffectUniform& uniform)
        {
            return uniform.components == 16 ? 4 : 1;
        }

        std::string accessor(const ComposeEffectUniform& uniform, const int row)
        {
            const std::string r = "u.effectData[" + std::to_string(row) + "]";
            switch (uniform.components) {
            case 1: return "(" + r + ".x)";
            case 2: return "(" + r + ".xy)";
            case 3: return "(" + r + ".xyz)";
            case 16:
                return "(float4x4(u.effectData[" + std::to_string(row) + "], u.effectData[" + std::to_string(row + 1) +
                    "], u.effectData[" + std::to_string(row + 2) + "], u.effectData[" + std::to_string(row + 3) + "]))";
            default: return "(" + r + ")";
            }
        }

        bool requiresDepth(const CameraFrameEffect& effect)
        {
            const auto resources = effect.requiredResources();
            return std::any_of(resources.begin(), resources.end(), [](const FrameResource resource) {
                return resource == FrameResource::Depth || resource == FrameResource::PrepassDepth;
            });
        }
    }

    ComposeEffectsBuild buildComposeEffects(const std::vector<std::shared_ptr<CameraFrameEffect>>& effects,
        const std::string& debugView)
    {
        ComposeEffectsBuild build;
        std::string declarations;
        int nextRow = 0;
        int nextInput = kComposeFirstEffectInput;
        for (const auto& owned : effects) {
            const CameraFrameEffect* effect = owned.get();
            if (!effect || !effect->active()) {
                continue;
            }
            // An effect with no slot takes part only to show one of its debug views.
            if (effect->slot() == ComposeSlot::None &&
                (debugView.empty() || effect->slang().empty() ||
                 std::find(effect->debugViews().begin(), effect->debugViews().end(), debugView) ==
                     effect->debugViews().end())) {
                continue;
            }
            if (effect->slang().empty()) {
                build.errors.push_back("compose effect '" + effect->id() + "' has a slot but no chunk; left out");
                continue;
            }
            int rows = 0;
            for (const auto& uniform : effect->uniforms()) {
                rows += rowsOf(uniform);
            }
            if (nextRow + rows > compose_shaders::kComposeEffectDataRows) {
                build.errors.push_back("compose effect '" + effect->id() + "' needs " + std::to_string(rows) +
                    " uniform rows and " + std::to_string(compose_shaders::kComposeEffectDataRows - nextRow) +
                    " are left; left out");
                continue;
            }
            const int textures = static_cast<int>(effect->textureUniforms().size());
            if (nextInput + textures > static_cast<int>(kQuadTextureInputs)) {
                build.errors.push_back("compose effect '" + effect->id() + "' needs " + std::to_string(textures) +
                    " texture inputs and " + std::to_string(static_cast<int>(kQuadTextureInputs) - nextInput) +
                    " are left; left out");
                continue;
            }

            build.effects.push_back(effect);
            build.requiresDepth = build.requiresDepth || requiresDepth(*effect);

            declarations += "\n// ---- effect '" + effect->id() + "' (" + slotName(effect->slot()) + ")\n";
            declarations += "#define " + effect->defineName() + " 1\n";
            for (const auto& [name, value] : effect->defines()) {
                declarations += "#define " + name + " " + value + "\n";
            }
            const auto& uniforms = effect->uniforms();
            for (size_t i = 0; i < uniforms.size(); ++i) {
                build.placements.push_back({effect, i, nextRow});
                declarations += "#define " + uniforms[i].name + " " + accessor(uniforms[i], nextRow) + "\n";
                nextRow += rowsOf(uniforms[i]);
            }
            const auto& textureUniforms = effect->textureUniforms();
            for (size_t i = 0; i < textureUniforms.size(); ++i) {
                build.texturePlacements.push_back({effect, i, nextInput});
                declarations += "VT_DECLARE_QUAD_INPUT" + std::to_string(nextInput) + "(" + textureUniforms[i].first + ")\n";
                ++nextInput;
            }
            declarations += effect->slang();
            declarations += "\n";
            for (const auto& view : effect->debugViews()) {
                build.debugViews.push_back(view);
            }
        }
        if (build.effects.empty()) {
            return build;
        }

        std::string& out = build.source;
        out = "// Generated by buildComposeEffects (scene/graphics/composeEffects.cpp): the compose\n"
              "// effects of a camera frame, in place of compose/compose-effects.slang.\n";
        out += "\n// The pixel a debug view is asked for (composeEffectDebug sets it).\nstatic float2 composeDebugUv;\n";
        out += declarations;
        out += "\n// ---- the slots, each effect in registration order\n";
        for (const ComposeSlot slot : kSlots) {
            const bool scene = slot == ComposeSlot::Scene;
            const char* type = scene ? "float4" : "float3";
            out += std::string(type) + " composeSlot" + capitalized(slotName(slot)) + "(" + type +
                " color, float2 uv)\n{\n";
            for (const CameraFrameEffect* effect : build.effects) {
                if (effect->slot() == slot) {
                    out += "    color = " + effect->entryPoint() + "(color, uv);\n";
                }
            }
            out += "    return color;\n}\n\n";
        }
        out += "float3 composeEffectDebug(uint view, float2 uv, float3 color)\n{\n    composeDebugUv = uv;\n";
        for (size_t i = 0; i < build.debugViews.size(); ++i) {
            out += "    if (view == " + std::to_string(i) + "u) {\n        return debug" +
                capitalized(build.debugViews[i]) + "();\n    }\n";
        }
        out += "    return float3(0.0, 0.0, 0.0);\n}\n";
        return build;
    }

    void packComposeEffectData(const ComposeEffectsBuild& build, float (*rows)[4])
    {
        for (const auto& placement : build.placements) {
            const auto& uniforms = placement.effect->uniforms();
            if (placement.uniform >= uniforms.size()) {
                continue;
            }
            const auto& uniform = uniforms[placement.uniform];
            const int count = uniform.components == 16 ? 16 : uniform.components;
            for (int i = 0; i < count; ++i) {
                rows[placement.row + i / 4][i % 4] = uniform.value[static_cast<size_t>(i)];
            }
        }
    }

    uint32_t composeDebugMode(const std::string& view, const ComposeEffectsBuild& build)
    {
        using namespace compose_shaders;
        if (view.empty()) {
            return kComposeDebugNone;
        }
        static const std::array<std::pair<const char*, uint32_t>, 7> builtIn = {{
            {"scene", kComposeDebugScene}, {"depth", kComposeDebugDepth}, {"bloom", kComposeDebugBloom},
            {"ssao", kComposeDebugSsao}, {"dofcoc", kComposeDebugDofCoc}, {"dofblur", kComposeDebugDofBlur},
            {"vignette", kComposeDebugVignette}}};
        for (const auto& [name, mode] : builtIn) {
            if (view == name) {
                return mode;
            }
        }
        for (size_t i = 0; i < build.debugViews.size(); ++i) {
            if (build.debugViews[i] == view) {
                return kComposeDebugFirstEffect + static_cast<uint32_t>(i);
            }
        }
        return kComposeDebugMissing;
    }
}
