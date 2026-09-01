// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// ShaderChunks registry demo: three spheres rendered with the default chunks, then a
// GLOBAL chunk override (grayscale tonemap — affects the whole scene), then a
// PER-MATERIAL chunk override (emissive glow — affects only the middle sphere), then
// back to defaults. Cache-invalidation hashing recompiles exactly the affected
// variants; restoring defaults reuses the originally compiled programs.
// Auto-cycles phases (Space pauses, 1-4 select phase, Esc quits).
//
#include <memory>

#include "../exampleApp.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/programLibrary.h"

using namespace visutwin::canvas;

// Global override: replaces the tonemap dispatch with a grayscale mapper. Only
// `toneMap()` is referenced by other chunks, so the override can stay minimal.
constexpr const char* GRAYSCALE_TONEMAP_CHUNK = R"(
static inline float3 toneMap(float3 color, float exposure, float mode)
{
    const float3 exposed = color * exposure;
    const float3 mapped = exposed / (exposed + float3(1.0));
    const float gray = dot(mapped, float3(0.299, 0.587, 0.114));
    return float3(gray);
}
)";

// Per-material override: adds a constant green glow on top of the material emissive.
constexpr const char* GREEN_GLOW_EMISSIVE_CHUNK = R"(
    float3 emissiveLinear = max(material.emissiveColor.rgb, float3(0.0)) + float3(0.0, 0.35, 0.05);
#if VT_FEATURE_EMISSIVE_MAP
    if (emissiveTexture.get_width() > 0 && emissiveTexture.get_height() > 0) {
        emissiveLinear *= srgbToLinear(emissiveTexture.sample(defaultSampler, uvEmissive).rgb);
    }
#endif
)";

// The same two overrides in GLSL, for Vulkan. Chunk NAMES are shared across
// backends; the SOURCE has to be in the language the device speaks, which is what
// GraphicsDevice::shaderLanguage() reports. Overriding a chunk with the wrong
// language fails to compile and the engine keeps the default shader.
constexpr const char* GRAYSCALE_TONEMAP_CHUNK_GLSL = R"(
vec3 applyToneMap(vec3 color) {
    vec3 mapped = color / (color + vec3(1.0));
    float gray = dot(mapped, vec3(0.299, 0.587, 0.114));
    return vec3(gray);
}
)";

constexpr const char* GREEN_GLOW_EMISSIVE_CHUNK_GLSL = R"(
    vec3 emissive = material.emissiveColor.rgb + vec3(0.0, 0.35, 0.05);
    if (vtFeatureEnabled(VT_FEATURE_EMISSIVE_MAP_BIT)) {
        emissive *= texture(emissiveMap, uvEmissive).rgb;
    }
    color += emissive;
)";

class ShaderChunksExample final: public ExampleApp
{
public:
    ShaderChunksExample(): ExampleApp({.title = "Shader Chunks"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.25f, 0.25f, 0.28f);

        createCamera(Vector3(0.0f, 2.2f, 7.0f), Vector3(-12.0f, 0.0f, 0.0f));
        createDirectionalLight(Vector3(45.0f, -30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.3f);

        _floorMaterial = std::make_shared<StandardMaterial>();
        _floorMaterial->setDiffuse(Color(0.45f, 0.45f, 0.48f, 1.0f));
        _floorMaterial->setGlossInvert(true);
        _floorMaterial->setGloss(0.85f);
        createPrimitive("plane", _floorMaterial.get(), Vector3(0.0f, -0.6f, 0.0f),
            Vector3(24.0f, 1.0f, 24.0f));

        const Color sphereColors[3] = {
            Color(0.85f, 0.25f, 0.2f, 1.0f), Color(0.85f, 0.7f, 0.2f, 1.0f), Color(0.25f, 0.45f, 0.85f, 1.0f)
        };
        for (int i = 0; i < 3; ++i) {
            _sphereMaterials[i] = std::make_shared<StandardMaterial>();
            _sphereMaterials[i]->setDiffuse(sphereColors[i]);
            _sphereMaterials[i]->setGlossInvert(true);
            _sphereMaterials[i]->setGloss(0.4f);
            createPrimitive("sphere", _sphereMaterials[i].get(),
                Vector3(-2.4f + 2.4f * static_cast<float>(i), 0.4f, 0.0f),
                Vector3(1.8f, 1.8f, 1.8f));
        }

        _programLibrary = getProgramLibrary(device());
        spdlog::info("ShaderChunks registry: {} default chunks", _programLibrary->chunks().names().size());

        applyPhase(0);
        spdlog::info("Keys: 1-4 = phase, Space = auto-cycle, Esc = quit");

        return true;
    }

    void update(const float dt) override
    {
        if (_autoCycle) {
            _cycleTimer += dt;
            if (_cycleTimer >= 3.0f) {
                _cycleTimer = 0.0f;
                _phase = (_phase + 1) % 4;
                applyPhase(_phase);
            }
        }
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        if (event.key.key >= SDLK_1 && event.key.key <= SDLK_4) {
            _autoCycle = false;
            _phase = static_cast<int>(event.key.key - SDLK_1);
            applyPhase(_phase);
            return true;
        }
        if (event.key.key == SDLK_SPACE) {
            _autoCycle = true;
            _cycleTimer = 0.0f;
            return true;
        }
        return false;
    }

private:
    // Phases: 0 defaults, 1 global grayscale tonemap, 2 per-material green glow, 3 defaults again.
    void applyPhase(const int phase) const
    {
        _programLibrary->chunks().clearOverrides();
        _sphereMaterials[1]->clearShaderChunks();
        const bool glsl = _programLibrary->chunks().language() == ShaderLanguage::Glsl;
        switch (phase) {
            case 1:
                _programLibrary->chunks().set("common-tonemap", glsl ? GRAYSCALE_TONEMAP_CHUNK_GLSL
                                                                    : GRAYSCALE_TONEMAP_CHUNK);
                spdlog::info("Phase 1: GLOBAL override 'common-tonemap' (grayscale, {}) — hash {:#x}",
                    glsl ? "GLSL" : "MSL", _programLibrary->chunks().hash());
                break;
            case 2:
                _sphereMaterials[1]->setShaderChunk("forward-fragment-emissive",
                    glsl ? GREEN_GLOW_EMISSIVE_CHUNK_GLSL : GREEN_GLOW_EMISSIVE_CHUNK);
                spdlog::info("Phase 2: PER-MATERIAL override 'forward-fragment-emissive' on middle sphere ({}) — hash {:#x}",
                    glsl ? "GLSL" : "MSL", _sphereMaterials[1]->shaderChunksHash());
                break;
            default:
                spdlog::info("Phase {}: default chunks", phase);
                break;
        }
    }

    std::shared_ptr<ProgramLibrary> _programLibrary;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::shared_ptr<StandardMaterial> _sphereMaterials[3];

    bool _autoCycle = true;
    int _phase = 0;
    float _cycleTimer = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ShaderChunksExample)
