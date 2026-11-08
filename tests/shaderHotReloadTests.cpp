// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
// The shader watcher (VISUTWIN_SHADER_WATCH), with no GPU: a device that keeps the code it is
// handed, over a copy of the shader tree the test edits.
//   - Shader::adoptCode swaps the code and the id in place, and refuses another backend's shader;
//   - ShaderSourceWatcher sees a file added, modified and removed, and nothing else;
//   - reloadChangedSlangShaders recompiles a live shader whose program changed into the SAME
//     object, with a new id and the edited code; a compile failure keeps the previous code; an
//     unchanged tree reloads nothing;
//   - ProgramLibrary::reloadChangedPrograms rebuilds a cached shadow variant in place when a
//     chunk the program includes changes.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "platform/graphics/shader.h"
#include "scene/shader-lib/programLibrary.h"
#include "scene/shader-lib/shaderSourceWatcher.h"
#include "scene/shader-lib/slangShaders.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    /// A shader that keeps the MSL it was created from, as a backend keeps its library.
    class SourceShader final : public Shader
    {
    public:
        SourceShader(GraphicsDevice* device, const ShaderDefinition& definition, std::string source)
            : Shader(device, definition), source(std::move(source)) {}

        std::string source;

    protected:
        void swapBackendCode(Shader& other) override
        {
            std::swap(source, static_cast<SourceShader&>(other).source);
        }
    };

    class SourceDevice final : public StubGraphicsDevice
    {
    public:
        std::shared_ptr<Shader> createShaderFromCode(const ShaderDefinition& definition,
            const ShaderCode& code) override
        {
            return std::make_shared<SourceShader>(this, definition, code.metalSource);
        }
    };

    std::string readFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    /// Writes `text` and moves the file's time stamp on, so a watcher polling at file-system
    /// resolution sees the change however quickly it follows the previous one.
    void writeFile(const std::filesystem::path& path, const std::string& text)
    {
        const bool existed = std::filesystem::exists(path);
        const auto before = existed ? std::filesystem::last_write_time(path) : std::filesystem::file_time_type{};
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << text;
        }
        if (existed) {
            std::filesystem::last_write_time(path, before + std::chrono::seconds(2));
        }
    }

    std::filesystem::path repositoryShaderTree()
    {
        for (const auto& root : shaderSourceRoots()) {
            const auto dir = root / "engine" / "shaders" / "slang";
            if (std::filesystem::is_directory(dir / "programs")) {
                return dir;
            }
        }
        return {};
    }

    void testAdoptCode()
    {
        SourceDevice device;
        ShaderDefinition definition;
        definition.name = "adopt";
        auto held = std::make_shared<SourceShader>(&device, definition, "old");
        auto fresh = std::make_shared<SourceShader>(&device, definition, "new");
        const int heldId = held->id();
        const int freshId = fresh->id();
        check(held->adoptCode(*fresh), "adoptCode accepts a shader of the same type");
        check(held->source == "new" && held->id() == freshId, "the held shader has the fresh code and id");
        check(fresh->source == "old" && fresh->id() == heldId, "the fresh shader is left with the old code and id");

        Shader plain(&device, definition);
        const int idBefore = held->id();
        check(!held->adoptCode(plain), "adoptCode refuses a shader of another type");
        check(held->id() == idBefore && held->source == "new", "a refused adoption changes nothing");
    }

    void testWatcher(const std::filesystem::path& scratch)
    {
        const auto dir = scratch / "watched";
        std::filesystem::create_directories(dir / "nested");
        writeFile(dir / "a.slang", "a");
        ShaderSourceWatcher watcher(dir, std::chrono::milliseconds(0));
        check(!watcher.poll(), "the first poll records the tree");
        check(!watcher.poll(), "an unchanged tree is no change");
        writeFile(dir / "nested" / "b.slang", "b");
        check(watcher.poll(), "a file added in a subdirectory is a change");
        check(!watcher.poll(), "and only once");
        writeFile(dir / "a.slang", "a2");
        check(watcher.poll(), "a modified file is a change");
        writeFile(dir / "notes.txt", "not a shader");
        check(!watcher.poll(), "a file that is not .slang is not");
        std::filesystem::remove(dir / "nested" / "b.slang");
        check(watcher.poll(), "a removed file is a change");

        ShaderSourceWatcher slow(dir, std::chrono::hours(1));
        check(!slow.poll(), "a slow watcher records the tree");
        writeFile(dir / "a.slang", "a3");
        check(!slow.poll(), "and does not look again within its interval");
    }

    void testSlangShaderReload(const std::filesystem::path& tree)
    {
        const auto program = tree / "programs" / "upsample.slang";
        const std::string original = readFile(program);
        const std::string line = "value += (a + c + g + i) * 0.0625;";
        if (!check(original.find(line) != std::string::npos, "the upsample program has the line the test edits")) {
            return;
        }

        auto device = std::make_shared<SourceDevice>();
        const auto shader = std::dynamic_pointer_cast<SourceShader>(getOrCreateSlangShader(device.get(), "upsample"));
        if (!check(shader != nullptr, "the upsample program compiles from the copied tree")) {
            return;
        }
        check(shader->source.find("0.012345") == std::string::npos, "the original code has no marker");
        check(reloadChangedSlangShaders(device.get()) == 0, "an unchanged tree reloads nothing");

        std::string edited = original;
        edited.replace(edited.find(line), line.size(), "value += (a + c + g + i) * 0.0123456;");
        writeFile(program, edited);
        const int idBefore = shader->id();
        check(reloadChangedSlangShaders(device.get()) == 1, "an edited program reloads its shader");
        check(shader->id() != idBefore, "the reloaded shader has a new id");
        check(shader->source.find("0.012345") != std::string::npos, "the reloaded shader has the edited code");
        check(getOrCreateSlangShader(device.get(), "upsample") == shader, "the device cache still names the same object");
        check(reloadChangedSlangShaders(device.get()) == 0, "a second reload with nothing changed reloads nothing");

        writeFile(program, edited + "\nthis is not Slang\n");
        const int idGood = shader->id();
        check(reloadChangedSlangShaders(device.get()) == 0, "a program that fails to compile reloads nothing");
        check(shader->id() == idGood && shader->source.find("0.012345") != std::string::npos,
            "and the shader keeps its previous code");

        writeFile(program, original);
        check(reloadChangedSlangShaders(device.get()) == 1, "the fixed program reloads");
        check(shader->source.find("0.012345") == std::string::npos, "with the original code back");

        // A module every program imports moves every live shader's fingerprint.
        const auto module = tree / "modules" / "vtquad.slang";
        const std::string moduleText = readFile(module);
        writeFile(module, moduleText + "\n// an edit the output does not show\n");
        check(reloadChangedSlangShaders(device.get()) == 1, "an edited module reloads the shader importing it");
        writeFile(module, moduleText);
        (void)reloadChangedSlangShaders(device.get());
    }

    void testProgramLibraryReload(const std::filesystem::path& tree)
    {
        auto device = std::make_shared<SourceDevice>();
        ProgramLibrary library(device);
        const auto shadow = library.getShadowShader(nullptr);
        if (!check(shadow != nullptr, "the shadow program builds a variant from the copied tree")) {
            return;
        }
        check(library.reloadChangedPrograms() == 0, "an unchanged tree reloads no variant");

        const auto chunk = tree / "forward" / "shadow-fragment.slang";
        const std::string text = readFile(chunk);
        writeFile(chunk, text + "\n// an edit to a chunk the shadow program includes\n");
        const int idBefore = shadow->id();
        check(library.reloadChangedPrograms() >= 1, "an edited chunk reloads the shadow variant");
        check(shadow->id() != idBefore, "the variant has a new id");
        check(library.getShadowShader(nullptr) == shadow, "the library still hands out the same object");
        check(library.reloadChangedPrograms() == 0, "nothing more to reload");
        writeFile(chunk, text);
    }
}

int main()
{
    testAdoptCode();

    const auto scratch = std::filesystem::temp_directory_path() /
        ("visutwin-shader-watch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(scratch);
    testWatcher(scratch);

    const auto source = repositoryShaderTree();
    if (check(!source.empty(), "the repository's shader tree is found")) {
        const auto root = scratch / "root";
        const auto tree = root / "engine" / "shaders" / "slang";
        std::filesystem::create_directories(tree.parent_path());
        std::filesystem::copy(source, tree, std::filesystem::copy_options::recursive);
        setenv("VISUTWIN_CANVAS_SHADERS", root.c_str(), 1);
        setenv("VISUTWIN_SHADER_WATCH", "1", 1);
        check(slangShaderWatchEnabled(), "VISUTWIN_SHADER_WATCH turns the watcher on");
        check(std::filesystem::equivalent(slangShaderSourceDir(), tree), "the runtime path compiles from the copy");
        testSlangShaderReload(tree);
        testProgramLibraryReload(tree);
    }

    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    return finish("shader hot reload");
}
