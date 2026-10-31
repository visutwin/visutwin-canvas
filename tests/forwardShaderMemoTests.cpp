// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026
//
// ProgramLibrary::getForwardShader remembers, ON THE MATERIAL, the shader it resolved,
// against everything the resolution reads: the material as of its uniformsVersion(), the
// draw's flags, the library's frame switches and the chunk registry. A repeat is then a
// compare instead of a rebuilt option set, a program name, a variant key and a cache
// lookup on every material switch.
//
// A memo that is too generous — one input missing from its key — hands a material the
// shader of its previous state: a normal map that never appears, a shadow that stays on.
// So what is checked is that each input moves the answer, and that an unchanged input
// set does not build anything.

#include <iostream>
#include <memory>
#include <string>

#include "platform/graphics/blendState.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/programLibrary.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // A device that blends with a second source colour.
    class DualSourceStubDevice final : public StubGraphicsDevice
    {
    public:
        bool supportsDualSourceBlending() const override { return true; }
    };
}

int main()
{
    std::cout << std::unitbuf;

    // The base device's createShader makes a plain Shader per call, which is all the
    // library needs to tell one variant from another.
    auto device = std::make_shared<StubGraphicsDevice>();
    ProgramLibrary library(device);
    Texture texture(device.get());

    StandardMaterial material;
    material.setUseMetalness(true);

    std::cout << "a repeat builds nothing\n";
    const uint64_t frameBits = library.forwardFrameBits();
    check(!library.forwardShaderResolved(&material, frameBits), "a material the library has not drawn is unresolved");
    const auto first = library.getForwardShader(&material, false);
    check(first != nullptr, "the first resolution builds a shader");
    const uint64_t builtAfterFirst = library.forwardVariantsCreated();
    check(builtAfterFirst == 1, "one variant built");
    check(library.forwardShaderResolved(&material, frameBits), "and the material now reads as resolved");
    check(library.getForwardShader(&material, false) == first && library.forwardVariantsCreated() == builtAfterFirst,
        "asking again returns the same shader and builds nothing");

    std::cout << "\nevery input moves the answer\n";
    {
        // The material.
        material.setNormalMap(&texture);
        check(!library.forwardShaderResolved(&material, frameBits), "an edited material is unresolved again");
        const auto withNormalMap = library.getForwardShader(&material, false);
        check(withNormalMap != nullptr && withNormalMap != first, "a normal map selects another shader");
        material.setNormalMap(nullptr);
        check(library.getForwardShader(&material, false) == first, "and taking it away selects the first again");

        // An edit that changes no feature: the variant is the same one, found again.
        const uint64_t built = library.forwardVariantsCreated();
        material.setDiffuse(Color(0.2f, 0.4f, 0.6f, 1.0f));
        check(library.getForwardShader(&material, false) == first && library.forwardVariantsCreated() == built,
            "an edit that changes no feature keeps the shader and builds nothing");

        // The draw.
        const auto skinned = library.getForwardShader(&material, false, false, true);
        check(skinned != nullptr && skinned != first, "a skinned draw of the material selects another shader");
        check(library.getForwardShader(&material, false) == first, "an unskinned one still gets the first");
        check(library.getForwardShader(&material, false, false, true) == skinned,
            "and the skinned one its own: two sets of inputs are both remembered");
        const auto transparentPass = library.getForwardShader(&material, true);
        check(transparentPass != nullptr && transparentPass != first, "the transparent pass selects another shader");

        // A frame switch.
        library.setLocalShadowsEnabled(true);
        check(library.forwardFrameBits() != frameBits, "a frame switch changes the frame bits");
        check(!library.forwardShaderResolved(&material, library.forwardFrameBits()),
            "and the material is unresolved under the new switches");
        const auto withLocalShadows = library.getForwardShader(&material, false);
        check(withLocalShadows != nullptr && withLocalShadows != first, "which select another shader");
        library.setLocalShadowsEnabled(false);
        check(library.getForwardShader(&material, false) == first, "switched back, the first is selected again");
        library.setEnvAtlasEncoding(EnvAtlasEncoding::Rgbp);
        check(library.forwardFrameBits() != frameBits, "the scene atlas's encoding is a frame switch too");
        library.setEnvAtlasEncoding(EnvAtlasEncoding::Srgb);
        library.setSkyboxReflectionEnabled(true);
        check(library.forwardFrameBits() != frameBits, "so is a skybox reflected without an atlas");
        library.setSkyboxReflectionEnabled(false);
        library.setToneMapAgxEnabled(true);
        check(library.forwardFrameBits() != frameBits, "and an AgX tone mapping");
        library.setToneMapAgxEnabled(false);
        library.setColorOutputCount(3);
        check(library.forwardFrameBits() != frameBits, "and the colour attachment count of the pass");
        library.setColorOutputCount(1);

        // The chunk registry.
        // An override is compiled, so it is the chunk's own source with a change that alters
        // nothing but the text.
        const std::string chunk = library.chunks().names().empty() ? std::string() : library.chunks().names().front();
        check(!chunk.empty(), "the forward chunks are found");
        if (!chunk.empty()) {
            library.chunks().set(chunk, *library.chunks().get(chunk) + "\n// overridden\n");
            const auto overridden = library.getForwardShader(&material, false);
            check(overridden != nullptr && overridden != first, "a registry chunk override selects a new shader");
        }
    }

    std::cout << "\nmaterials and libraries do not share memos\n";
    {
        ProgramLibrary fresh(device);
        StandardMaterial a;
        StandardMaterial b;
        b.setNormalMap(&texture);
        const auto shaderA = fresh.getForwardShader(&a, false);
        const auto shaderB = fresh.getForwardShader(&b, false);
        check(shaderA && shaderB && shaderA != shaderB, "two materials with different features get different shaders");
        check(fresh.getForwardShader(&a, false) == shaderA && fresh.getForwardShader(&b, false) == shaderB,
            "each keeps its own on a repeat");

        StandardMaterial same;
        check(fresh.getForwardShader(&same, false) == shaderA,
            "a third material with the first one's features shares its shader");

        // A copy carries the original's memo; it must still answer for its own state.
        StandardMaterial copy(a);
        copy.setNormalMap(&texture);
        check(fresh.getForwardShader(&copy, false) == shaderB, "an edited copy resolves for what it is now");
        check(fresh.getForwardShader(&a, false) == shaderA, "and leaves the original's answer alone");

        ProgramLibrary other(device);
        const auto otherShader = other.getForwardShader(&a, false);
        check(otherShader != nullptr && otherShader != shaderA, "another library builds its own shader for the material");
        check(fresh.getForwardShader(&a, false) == shaderA && other.getForwardShader(&a, false) == otherShader,
            "and the two keep answering with their own");
    }

    std::cout << "\nthe memo does not keep a shader alive\n";
    {
        StandardMaterial lone;
        std::weak_ptr<Shader> held;
        {
            ProgramLibrary scoped(device);
            held = scoped.getForwardShader(&lone, false);
            check(!held.expired(), "the library owns the shader");
        }
        check(held.expired(), "which goes away with the library, memo or no memo");
        ProgramLibrary next(device);
        check(next.getForwardShader(&lone, false) != nullptr, "a later library resolves the material afresh");
    }

    std::cout << "\nthe iridescence maps are features of an iridescent variant\n";
    {
        StandardMaterial iridescent;
        iridescent.setIridescenceIntensity(1.0f);
        const auto plain = library.getForwardShader(&iridescent, false);
        iridescent.setIridescenceMap(&texture);
        const auto intensityMap = library.getForwardShader(&iridescent, false);
        iridescent.setIridescenceThicknessMap(&texture);
        const auto bothMaps = library.getForwardShader(&iridescent, false);
        check(plain && !plain->features().test(ShaderFeature::IridescenceMap) &&
                  !plain->features().test(ShaderFeature::IridescenceThicknessMap),
            "no maps: neither feature");
        check(intensityMap && intensityMap->features().test(ShaderFeature::IridescenceMap) &&
                  !intensityMap->features().test(ShaderFeature::IridescenceThicknessMap),
            "the intensity map");
        check(bothMaps && bothMaps->features().test(ShaderFeature::IridescenceMap) &&
                  bothMaps->features().test(ShaderFeature::IridescenceThicknessMap),
            "both maps");
        iridescent.setIridescenceIntensity(0.0f);
        const auto off = library.getForwardShader(&iridescent, false);
        check(off && !off->features().test(ShaderFeature::IridescenceMap) &&
                  !off->features().test(ShaderFeature::IridescenceThicknessMap),
            "maps on a material without iridescence are not read");
    }

    std::cout << "\ndiffuse transmission is a feature only while its fraction is above zero\n";
    {
        StandardMaterial leaf;
        leaf.setDiffuseTransmissionMap(&texture);
        leaf.setDiffuseTransmissionColorMap(&texture);
        const auto off = library.getForwardShader(&leaf, false);
        leaf.setDiffuseTransmission(0.5f);
        const auto on = library.getForwardShader(&leaf, false);
        leaf.setDiffuseTransmission(0.75f);
        const auto stillOn = library.getForwardShader(&leaf, false);
        check(off && !off->features().test(ShaderFeature::DiffuseTransmission) &&
                  !off->features().test(ShaderFeature::DiffuseTransmissionMap) &&
                  !off->features().test(ShaderFeature::DiffuseTransmissionColorMap),
            "a fraction of 0 keeps it, and its maps, out of the shader");
        check(on && on->features().test(ShaderFeature::DiffuseTransmission) &&
                  on->features().test(ShaderFeature::DiffuseTransmissionMap) &&
                  on->features().test(ShaderFeature::DiffuseTransmissionColorMap),
            "above 0 the transmission and both maps are features");
        check(stillOn == on, "another fraction above 0 is the same variant (only a uniform changes)");
    }

    std::cout << "\nthe fragment entry writes what the target and the blend take\n";
    {
        ProgramLibrary outputs(device);
        StandardMaterial plain;
        const auto single = outputs.getForwardShader(&plain, false);
        check(single && single->fragmentEntry() == "forwardFragment", "one attachment: forwardFragment");
        outputs.setColorOutputCount(3);
        const auto three = outputs.getForwardShader(&plain, false);
        check(three && three->fragmentEntry() == "forwardFragmentMrt3", "three attachments: forwardFragmentMrt3");
        outputs.setColorOutputCount(2);
        const auto two = outputs.getForwardShader(&plain, false);
        check(two && two->fragmentEntry() == "forwardFragmentMrt2", "two: forwardFragmentMrt2");
        outputs.setColorOutputCount(1);
        check(outputs.getForwardShader(&plain, false) == single, "back to one: the first shader again");

        StandardMaterial dual;
        auto blend = std::make_shared<BlendState>();
        blend->setEnabled(true);
        blend->setColorSrcFactor(BLENDMODE_ONE);
        blend->setColorDstFactor(BLENDMODE_SRC1_COLOR);
        dual.setBlendState(blend);
        const auto withoutSupport = outputs.getForwardShader(&dual, false);
        check(withoutSupport && withoutSupport->fragmentEntry() == "forwardFragment",
            "a second-source blend on a device without dual-source blending writes one colour");

        auto dualDevice = std::make_shared<DualSourceStubDevice>();
        ProgramLibrary dualLibrary(dualDevice);
        const auto dualShader = dualLibrary.getForwardShader(&dual, false);
        check(dualShader && dualShader->fragmentEntry() == "forwardFragmentDualSource",
            "with it, the entry writing the second source");
        check(dualLibrary.getForwardShader(&plain, false)->fragmentEntry() == "forwardFragment",
            "a blend without a second source keeps forwardFragment");
    }

    return finish("forward shader memo");
}
