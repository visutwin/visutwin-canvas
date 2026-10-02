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

#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/programLibrary.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

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

        // The chunk registry.
        const std::string chunk = library.chunks().names().empty() ? std::string() : library.chunks().names().front();
        if (!chunk.empty()) {
            library.chunks().set(chunk, "// overridden\n");
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

    return finish("forward shader memo");
}
