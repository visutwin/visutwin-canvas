// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// StandardMaterial's workflow and scalar packing, pinned against upstream.
//
// A default upstream StandardMaterial is in the SPECULAR workflow (useMetalness
// false) with a black specular colour, so it renders no specular at all; this port
// used to default to metalness 0, a dielectric with reflections. A render cannot say
// which default a scene got — the difference is a faint grazing highlight — so the
// defaults and the rule that decides whether specular renders are checked here.
//
// Also pinned: StandardMaterial's own scalars must apply even when a base-colour
// texture is bound on the base Material, which is how the GLB parser binds it.
// They used to be packed only when a diffuse map was set or no base-colour texture
// was, so setOpacity / setMetalness / setGloss / setBumpiness on a loaded model
// silently did nothing.

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        if (!condition) {
            std::printf("FAIL: %s\n", what.c_str());
            ++failures;
        }
    }

    bool near(const float a, const float b)
    {
        return std::fabs(a - b) < 1e-5f;
    }

    /// Creates nothing; enough for a Texture to exist so a material can hold one.
    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&, int,
            const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    std::unique_ptr<Texture> makeTexture(GraphicsDevice* device)
    {
        TextureOptions options;
        options.width = 4;
        options.height = 4;
        options.mipmaps = false;
        return std::make_unique<Texture>(device, options);
    }

    constexpr uint32_t kSkyboxOffBit = 1u << 18;
    constexpr uint32_t kOpacityMapBit = 1u << 19;
}

int main()
{
    StubDevice device;

    // Upstream's defaults, and what they mean for the packed block.
    {
        StandardMaterial material;
        check(!material.useMetalness(), "useMetalness defaults to false (upstream _defineFlag('useMetalness', false))");
        check(near(material.metalness(), 1.0f), "metalness defaults to 1 (upstream)");
        check(near(material.gloss(), 0.25f), "gloss defaults to 0.25 (upstream)");
        check(near(material.heightMapFactor(), 1.0f), "heightMapFactor defaults to 1 (upstream)");
        check(near(material.iridescenceThicknessMax(), 0.0f), "iridescenceThicknessMax defaults to 0 (upstream)");
        check(material.usesSpecularWorkflow(), "a default material is in the specular workflow");
        check(!material.rendersSpecular(),
            "a default material renders NO specular: specular workflow, black specular, no map, no clearcoat");

        const MaterialUniforms& u = material.packedUniforms();
        check(near(u.metallicFactor, 0.0f), "the specular workflow packs metallic 0 even though metalness is 1");
        check(near(u.specGlossParams[0], 0.0f) && near(u.specGlossParams[1], 0.0f) && near(u.specGlossParams[2], 0.0f),
            "a black specular packs a black F0");
        check(near(u.specGlossParams[3], 0.25f), "the specular workflow's gloss is the material gloss");
        check(near(u.roughnessFactor, 0.75f), "roughness is 1 - gloss");
        check((u.flags & (kSkyboxOffBit | kOpacityMapBit)) == 0u,
            "a default material keeps the scene environment and has no opacity map bit");
    }

    // Each of upstream's useSpecular conditions turns specular on.
    {
        StandardMaterial metal;
        metal.setUseMetalness(true);
        check(metal.rendersSpecular() && !metal.usesSpecularWorkflow(), "useMetalness renders specular");
        check(near(metal.packedUniforms().metallicFactor, 1.0f), "useMetalness packs the default metalness of 1");
        metal.setMetalness(0.3f);
        check(near(metal.packedUniforms().metallicFactor, 0.3f), "useMetalness packs the authored metalness");

        StandardMaterial tinted;
        tinted.setSpecular(Color(0.5f, 0.0f, 0.0f, 1.0f));
        check(tinted.rendersSpecular(), "a non-black specular colour renders specular");
        check(near(tinted.packedUniforms().specGlossParams[0], std::pow(0.5f, 2.2f)),
            "specular is authored in sRGB and packed linear, as upstream's _defineColor uniforms are");

        StandardMaterial coated;
        coated.setClearCoat(0.5f);
        check(coated.rendersSpecular(), "clearcoat renders specular (upstream's useSpecular includes clearCoat > 0)");

        const auto specGlossTexture = makeTexture(&device);
        StandardMaterial specGlossMapped;
        specGlossMapped.setSpecGlossMap(specGlossTexture.get());
        check(specGlossMapped.rendersSpecular(), "a spec-gloss map renders specular");

        StandardMaterial inverted;
        inverted.setGlossInvert(true);
        inverted.setGloss(0.2f);
        check(near(inverted.packedUniforms().specGlossParams[3], 0.8f),
            "glossInvert applies to the specular workflow's gloss too");
    }

    // The scalars apply with a base-colour texture bound on the base Material, the
    // way the GLB parser binds one.
    {
        const auto baseColor = makeTexture(&device);
        StandardMaterial material;
        material.setBaseColorTexture(baseColor.get());
        material.setHasBaseColorTexture(true);
        material.setUseMetalness(true);
        material.setOpacity(0.25f);
        material.setMetalness(0.6f);
        material.setGloss(0.9f);
        material.setBumpiness(0.4f);

        const MaterialUniforms& u = material.packedUniforms();
        check(near(u.baseColor[3], 0.25f), "setOpacity applies with a base-colour texture bound");
        check(near(u.metallicFactor, 0.6f), "setMetalness applies with a base-colour texture bound");
        check(near(u.roughnessFactor, 0.1f), "setGloss applies with a base-colour texture bound");
        check(near(u.normalScale, 0.4f), "setBumpiness applies with a base-colour texture bound");
    }

    // useSkybox and the opacity map: the flag bits the shaders gate on, and slot 34.
    {
        StandardMaterial material;
        material.setUseSkybox(false);
        check((material.packedUniforms().flags & kSkyboxOffBit) != 0u, "useSkybox(false) sets flags bit 18");
        material.setUseSkybox(true);
        check((material.packedUniforms().flags & kSkyboxOffBit) == 0u, "useSkybox(true) clears flags bit 18");

        const auto opacity = makeTexture(&device);
        material.setOpacityMap(opacity.get());
        check((material.packedUniforms().flags & kOpacityMapBit) != 0u, "an opacity map sets flags bit 19");

        std::vector<TextureSlot> slots;
        material.getTextureSlots(slots);
        bool boundAt34 = false;
        for (const auto& slot : slots) {
            if (slot.slot == 34 && slot.texture == opacity.get()) boundAt34 = true;
        }
        check(boundAt34, "the opacity map is bound at texture slot 34");
    }

    if (failures != 0) {
        std::printf("standard material workflow: %d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("standard material workflow: all checks passed\n");
    return 0;
}
