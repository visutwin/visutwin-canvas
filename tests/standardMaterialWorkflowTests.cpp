// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2026
//
// StandardMaterial's workflow and scalar packing, pinned against upstream.
//
// A default StandardMaterial is in the SPECULAR workflow (useMetalness
// false) with a black specular colour, so it renders no specular at all, where a
// metalness-0 default would be a dielectric with reflections. A render cannot say
// which default a scene got — the difference is a faint grazing highlight — so the
// defaults and the rule that decides whether specular renders are checked here.
//
// Also pinned: StandardMaterial's own scalars must apply even when a base-colour
// texture is bound on the base Material, which is how the GLB parser binds it.
// Packed only when a diffuse map is set or no base-colour texture is, setOpacity /
// setMetalness / setGloss / setBumpiness on a loaded model silently do nothing.

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-5f;

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
    quietPasses();
    // Creates nothing; enough for a Texture to exist so a material can hold one.
    StubGraphicsDevice device;

    // The defaults, and what they mean for the packed block.
    {
        StandardMaterial material;
        check(!material.useMetalness(), "useMetalness defaults to false (upstream _defineFlag('useMetalness', false))");
        check(nearStrict(material.metalness(), 1.0f, kTolerance), "metalness defaults to 1 (upstream)");
        check(nearStrict(material.gloss(), 0.25f, kTolerance), "gloss defaults to 0.25 (upstream)");
        check(nearStrict(material.heightMapFactor(), 1.0f, kTolerance), "heightMapFactor defaults to 1 (upstream)");
        check(nearStrict(material.iridescenceThicknessMax(), 0.0f, kTolerance),
            "iridescenceThicknessMax defaults to 0 (upstream)");
        check(material.usesSpecularWorkflow(), "a default material is in the specular workflow");
        check(!material.rendersSpecular(),
            "a default material renders NO specular: specular workflow, black specular, no map, no clearcoat");

        const MaterialUniforms& u = material.packedUniforms();
        check(nearStrict(u.metallicFactor, 0.0f, kTolerance),
            "the specular workflow packs metallic 0 even though metalness is 1");
        check(nearStrict(u.specGlossParams[0], 0.0f, kTolerance) &&
              nearStrict(u.specGlossParams[1], 0.0f, kTolerance) && nearStrict(u.specGlossParams[2], 0.0f, kTolerance),
            "a black specular packs a black F0");
        check(nearStrict(u.specGlossParams[3], 0.25f, kTolerance),
            "the specular workflow's gloss is the material gloss");
        check(nearStrict(u.roughnessFactor, 0.75f, kTolerance), "roughness is 1 - gloss");
        check((u.flags & (kSkyboxOffBit | kOpacityMapBit)) == 0u,
            "a default material keeps the scene environment and has no opacity map bit");
    }

    // Each of the useSpecular conditions turns specular on.
    {
        StandardMaterial metal;
        metal.setUseMetalness(true);
        check(metal.rendersSpecular() && !metal.usesSpecularWorkflow(), "useMetalness renders specular");
        check(nearStrict(metal.packedUniforms().metallicFactor, 1.0f, kTolerance),
            "useMetalness packs the default metalness of 1");
        metal.setMetalness(0.3f);
        check(nearStrict(metal.packedUniforms().metallicFactor, 0.3f, kTolerance),
            "useMetalness packs the authored metalness");

        StandardMaterial tinted;
        tinted.setSpecular(Color(0.5f, 0.0f, 0.0f, 1.0f));
        check(tinted.rendersSpecular(), "a non-black specular colour renders specular");
        check(nearStrict(tinted.packedUniforms().specGlossParams[0], std::pow(0.5f, 2.2f), kTolerance),
            "specular is authored in sRGB and packed linear, as upstream's _defineColor uniforms are");

        StandardMaterial coated;
        coated.setClearCoat(0.5f);
        check(coated.rendersSpecular(), "clearcoat renders specular (upstream's useSpecular includes clearCoat > 0)");
        check(nearStrict(coated.packedUniforms().clearCoatMapChannels[0], 1.0f, kTolerance) &&
                nearStrict(coated.packedUniforms().clearCoatMapChannels[1], 1.0f, kTolerance),
            "the clearcoat maps read G by default (upstream clearCoatMapChannel / clearCoatGlossMapChannel)");
        coated.setClearCoatMapChannel(MapChannel::MAP_CHANNEL_R);
        coated.setClearCoatGlossMapChannel(MapChannel::MAP_CHANNEL_A);
        check(nearStrict(coated.packedUniforms().clearCoatMapChannels[0], 0.0f, kTolerance) &&
                nearStrict(coated.packedUniforms().clearCoatMapChannels[1], 3.0f, kTolerance),
            "a clearcoat map channel set on the material reaches the packed block");

        const auto specGlossTexture = makeTexture(&device);
        StandardMaterial specGlossMapped;
        specGlossMapped.setSpecGlossMap(specGlossTexture.get());
        check(specGlossMapped.rendersSpecular(), "a spec-gloss map renders specular");

        StandardMaterial inverted;
        inverted.setGlossInvert(true);
        inverted.setGloss(0.2f);
        check(nearStrict(inverted.packedUniforms().specGlossParams[3], 0.8f, kTolerance),
            "glossInvert applies to the specular workflow's gloss too");
    }

    // A gloss map multiplies the AUTHORED factor and the product is inverted after:
    // under glossInvert, roughness = factor x texel. The shaders compute
    // gloss = factor x texel, then 1 - gloss when the flag is set; this mirrors them
    // so the packed values are checked against the result they produce. The no-map
    // packing (roughnessFactor, clearCoatRoughness) is unchanged.
    {
        const auto shadedRoughness = [](const float factor, const float invert, const float texel) {
            float gloss = factor * texel;
            if (invert > 0.5f) gloss = 1.0f - gloss;
            return 1.0f - gloss;
        };
        const auto glossTexture = makeTexture(&device);

        StandardMaterial base;
        base.setGlossMap(glossTexture.get());
        base.setGlossInvert(true);
        base.setGloss(1.0f);
        const MaterialUniforms& u = base.packedUniforms();
        check(u.mapChannelParams[0] == 1.0f, "a gloss map's factor is packed as authored, not inverted");
        check(u.glossMapParams[0] == 1.0f, "glossInvert packs the base invert flag");
        check(nearStrict(shadedRoughness(u.mapChannelParams[0], u.glossMapParams[0], 0.3f), 0.3f, kTolerance),
            "an inverted gloss map with factor 1 and texel 0.3 gives roughness 0.3");
        check(u.roughnessFactor == 1.0f, "the inverted factor still packs roughness = factor with no map read");

        base.setGlossInvert(false);
        base.setGloss(0.5f);
        const MaterialUniforms& v = base.packedUniforms();
        check(v.glossMapParams[0] == 0.0f, "no glossInvert, no invert flag");
        check(nearStrict(shadedRoughness(v.mapChannelParams[0], v.glossMapParams[0], 0.6f), 0.7f, kTolerance),
            "a plain gloss map gives roughness 1 - factor x texel");

        // The glTF clearcoat route: coat roughness as the gloss factor, inverted.
        StandardMaterial coat;
        coat.setClearCoat(1.0f);
        coat.setClearCoatGloss(1.0f);
        coat.setClearCoatGlossInvert(true);
        coat.setClearCoatGlossMap(glossTexture.get());
        const MaterialUniforms& c = coat.packedUniforms();
        check(c.glossMapParams[1] == 1.0f && c.glossMapParams[2] == 1.0f,
            "an inverted clearcoat gloss packs the invert flag and the authored factor");
        check(nearStrict(shadedRoughness(c.glossMapParams[2], c.glossMapParams[1], 0.3f), 0.3f, kTolerance),
            "an inverted clearcoat gloss map with factor 1 and texel 0.3 gives coat roughness 0.3");
        check(c.clearCoatRoughness == 1.0f, "the no-map coat roughness is still the inverted factor");
        check((c.flags & (1u << 15)) != 0u, "a clearcoat gloss map sets flags bit 15");
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
        check(nearStrict(u.baseColor[3], 0.25f, kTolerance), "setOpacity applies with a base-colour texture bound");
        check(nearStrict(u.metallicFactor, 0.6f, kTolerance), "setMetalness applies with a base-colour texture bound");
        check(nearStrict(u.roughnessFactor, 0.1f, kTolerance), "setGloss applies with a base-colour texture bound");
        check(nearStrict(u.normalScale, 0.4f, kTolerance), "setBumpiness applies with a base-colour texture bound");
    }

    // The ambient tint: white by default, so every
    // material that never sets it packs exactly 1 and renders as before; authored sRGB,
    // uploaded linear, as emissive is.
    {
        StandardMaterial material;
        const MaterialUniforms& u = material.packedUniforms();
        check(u.ambientTint[0] == 1.0f && u.ambientTint[1] == 1.0f && u.ambientTint[2] == 1.0f,
            "the ambient tint defaults to exactly white");
        material.setAmbient(Color(0.5f, 0.25f, 1.0f, 1.0f));
        const MaterialUniforms& t = material.packedUniforms();
        check(nearStrict(t.ambientTint[0], std::pow(0.5f, 2.2f), kTolerance) &&
              nearStrict(t.ambientTint[1], std::pow(0.25f, 2.2f), kTolerance) &&
              nearStrict(t.ambientTint[2], 1.0f, kTolerance), "setAmbient packs the colour linearised");
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

    // A mesh instance's own lightmap (what a lightmapper bakes) goes over the
    // material's lightmap slot, and leaves the material itself untouched. A baker that
    // writes into the shared material makes meshes sharing one show a single bake.
    {
        const auto assigned = makeTexture(&device);
        const auto baked = makeTexture(&device);
        const auto slotTexture = [](const std::vector<TextureSlot>& slots) -> Texture* {
            for (const auto& slot : slots) {
                if (slot.slot == kLightMapTextureSlot) return slot.texture;
            }
            return nullptr;
        };

        StandardMaterial material;
        std::vector<TextureSlot> slots;
        material.getTextureSlots(slots);
        applyInstanceLightMap(slots, baked.get());
        check(slotTexture(slots) == baked.get(),
            "an instance lightmap is bound at the lightmap slot when the material has none");

        material.setLightMap(assigned.get());
        slots.clear();
        material.getTextureSlots(slots);
        check(slotTexture(slots) == assigned.get(), "the material's own lightmap binds at the lightmap slot");
        applyInstanceLightMap(slots, baked.get());
        check(slotTexture(slots) == baked.get(), "the instance lightmap WINS over the material's");
        check(material.lightMap() == assigned.get(), "and the material's lightmap is left alone");

        slots.clear();
        material.getTextureSlots(slots);
        applyInstanceLightMap(slots, nullptr);
        check(slotTexture(slots) == assigned.get(), "no instance lightmap leaves the material's in place");
    }

    return finish("standard material workflow");
}
