// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Shader composition contracts that only ever broke silently:
//
//  - The Vulkan fragment shader exists twice: forward.frag #includes the chunks at build
//    time, ProgramLibrary composes them at run time when an override is set. The two
//    ORDERS must agree, or an override build differs from the bundled shader in ways no
//    single render reveals.
//  - Every chunk a program registers exists in that language's defaults.
//  - A chunk resolves material override > registry override > default.
//  - A variant's key changes exactly when an override that feeds it changes, so a new
//    override compiles a new variant and an unchanged one reuses the cache.
//
// CPU only: a stub device answers the shader language, and nothing is compiled.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "platform/graphics/graphicsDevice.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/programLibrary.h"

namespace visutwin::canvas
{
    // The seam ProgramLibrary befriends for this test.
    struct ProgramLibraryTestAccess
    {
        static const std::vector<std::string>* chunkOrder(const ProgramLibrary& library, const std::string& program)
        {
            const auto it = library._registeredPrograms.find(program);
            return it == library._registeredPrograms.end() ? nullptr : &it->second;
        }
        static std::string composeGlsl(ProgramLibrary& library, const std::string& program, const Material* material)
        {
            return library.composeProgramVariantGlslSource(program, material);
        }
        static ProgramLibrary::VariantKey key(const ProgramLibrary& library, const Material* material)
        {
            const auto options = library.buildForwardVariantOptions(material, false);
            return library.makeVariantKey(ProgramLibrary::resolveProgramName(options), options, material);
        }
    };
}

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    class StubDevice final : public GraphicsDevice
    {
    public:
        explicit StubDevice(const ShaderLanguage language) : _language(language) {}
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
        ShaderLanguage shaderLanguage() const override { return _language; }
    private:
        ShaderLanguage _language;
    };

    std::vector<std::string> forwardFragIncludes(const std::filesystem::path& file)
    {
        std::ifstream in(file);
        std::vector<std::string> names;
        const std::regex include(R"re(#include\s+"chunks/([A-Za-z0-9_-]+)\.glsl")re");
        std::string line;
        while (std::getline(in, line)) {
            std::smatch m;
            if (std::regex_search(line, m, include)) {
                names.push_back(m[1]);
            }
        }
        return names;
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto glslDevice = std::make_shared<StubDevice>(ShaderLanguage::Glsl);
    ProgramLibrary glsl(glslDevice);
    auto mslDevice = std::make_shared<StubDevice>(ShaderLanguage::Msl);
    ProgramLibrary msl(mslDevice);

    std::cout << "the two Vulkan compositions agree\n";
    const auto forwardFrag = glsl.chunks().rootPath().parent_path() / "forward.frag";
    const auto includes = forwardFragIncludes(forwardFrag);
    const auto* forward = ProgramLibraryTestAccess::chunkOrder(glsl, "forward");
    const auto* skybox = ProgramLibraryTestAccess::chunkOrder(glsl, "skybox");
    check(!includes.empty(), "forward.frag lists its chunks (" + forwardFrag.string() + ")");
    check(forward && *forward == includes, "ProgramLibrary's GLSL 'forward' order is forward.frag's #include order");
    check(skybox && *skybox == includes, "and 'skybox' shares it");

    std::cout << "\nevery registered chunk exists\n";
    for (const auto& [library, language] : {std::pair{&glsl, "GLSL"}, std::pair{&msl, "MSL"}}) {
        for (const char* program : {"forward", "skybox", "shadow"}) {
            const auto* order = ProgramLibraryTestAccess::chunkOrder(*library, program);
            if (!order) {
                continue;   // GLSL registers no "shadow"
            }
            bool all = true;
            for (const auto& name : *order) {
                if (!library->chunks().has(name)) {
                    all = false;
                    std::cout << "        missing " << language << " chunk '" << name << "'\n";
                }
            }
            check(all, std::string(language) + " '" + program + "': every chunk it names is in the defaults");
        }
    }

    std::cout << "\noverride precedence in the composed source\n";
    {
        const std::string defaultSource = ProgramLibraryTestAccess::composeGlsl(glsl, "forward", nullptr);
        glsl.chunks().set("common-dither", "// REGISTRY-OVERRIDE\n");
        StandardMaterial material;
        material.setShaderChunk("common-dither", "// MATERIAL-OVERRIDE\n");
        const std::string withMaterial = ProgramLibraryTestAccess::composeGlsl(glsl, "forward", &material);
        const std::string withRegistry = ProgramLibraryTestAccess::composeGlsl(glsl, "forward", nullptr);
        check(withMaterial.find("MATERIAL-OVERRIDE") != std::string::npos &&
              withMaterial.find("REGISTRY-OVERRIDE") == std::string::npos, "a material override beats the registry's");
        check(withRegistry.find("REGISTRY-OVERRIDE") != std::string::npos, "the registry's beats the default");
        glsl.chunks().remove("common-dither");
        check(ProgramLibraryTestAccess::composeGlsl(glsl, "forward", nullptr) == defaultSource,
            "removing it gives the default back, byte for byte");
    }

    std::cout << "\nvariant keys\n";
    {
        StandardMaterial a;
        StandardMaterial b;
        const auto base = ProgramLibraryTestAccess::key(msl, &a);
        check(base == ProgramLibraryTestAccess::key(msl, &b), "two plain materials share a variant");
        b.setShaderChunk("common-utils", "// one\n");
        const auto one = ProgramLibraryTestAccess::key(msl, &b);
        check(!(one == base), "a material override makes a new variant");
        b.setShaderChunk("common-utils", "// two\n");
        const auto two = ProgramLibraryTestAccess::key(msl, &b);
        check(!(two == one), "and a DIFFERENT override another");
        msl.chunks().set("common-utils", "// registry\n");
        const auto registry = ProgramLibraryTestAccess::key(msl, &a);
        check(!(registry == base), "a registry override re-keys every material");
        msl.chunks().remove("common-utils");
        check(ProgramLibraryTestAccess::key(msl, &a) == base, "and removing it returns to the old key");
    }

    std::cout << (failures == 0 ? "\nAll shader composition tests passed\n" : "\nShader composition tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
