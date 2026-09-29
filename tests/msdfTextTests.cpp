// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// MSDF text, below the shader: the font loader's pages, pixel range and intensity, the
// text visual's split into one mesh instance and material per atlas page, and the
// material values the MSDF shader path reads — upstream's editor-unit scaling of the
// outline (x 0.2) and shadow (x 0.005 of the page, y by minus the page's aspect), colours
// uploaded linear, and the page size (textureSize() does not survive MoltenVK).
//
// The two pages are deliberately DIFFERENT sizes, so a per-page value that is taken from
// page 0 for every page is caught. A render cannot show that: every shipped font but
// upstream's roboto is a single page.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <stb_image_write.h>

#include "framework/appOptions.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/handlers/fontResource.h"
#include "framework/input/elementInput.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

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

    bool near(const float a, const float b, const float eps = 1e-5f) { return std::abs(a - b) <= eps; }

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
        void setResolution(const int width, const int height) override { _size = {width, height}; }
        std::pair<int, int> size() const override { return _size; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    private:
        std::pair<int, int> _size{300, 150};
    };

    void writePng(const std::filesystem::path& path, const int width, const int height)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 4), 128);
        stbi_write_png(path.string().c_str(), width, height, 4, pixels.data(), width * 4);
    }

    /// 'A' on page 0 (64x32), 'B' on page 1 (32x32). `msdf` adds the glyph `range`.
    std::filesystem::path writeFont(const std::filesystem::path& dir, const std::string& name, const bool msdf)
    {
        writePng(dir / (name + ".png"), 64, 32);
        writePng(dir / (name + "1.png"), 32, 32);
        const std::string extra = msdf ? R"(,"scale":1.5,"range":4)" : "";
        const auto json = dir / (name + ".json");
        std::ofstream out(json);
        out << R"({"version":2,"intensity":0.25,"info":{"face":"t","maps":[{"width":64,"height":32},{"width":32,"height":32}]},)"
            << R"("chars":{)"
            << R"("65":{"id":65,"x":0,"y":0,"width":16,"height":16,"map":0,"xadvance":10,"xoffset":0,"yoffset":0,"bounds":[0,-5,10,20])" << extra << "},"
            << R"("66":{"id":66,"x":8,"y":8,"width":16,"height":16,"map":1,"xadvance":10,"xoffset":0,"yoffset":0,"bounds":[0,-2,8,24])" << extra << "}"
            << R"(},"kerning":{"65":{"66":-2.5,"65":0.75},"66":{"65":-1}}})";
        return json;
    }

    /// The text visual's mesh instances: the render component on the element's child.
    std::vector<MeshInstance*> visualInstances(Entity* elementEntity)
    {
        for (const auto& child : elementEntity->children()) {
            auto* entity = dynamic_cast<Entity*>(child.get());
            if (auto* render = entity ? entity->findComponent<RenderComponent>() : nullptr) {
                return render->meshInstances();
            }
        }
        return {};
    }
}

int main()
{
    const auto dir = std::filesystem::temp_directory_path() / "visutwin-msdf-text-tests";
    std::filesystem::create_directories(dir);

    auto device = std::make_shared<StubDevice>();
    auto engine = std::make_shared<Engine>(nullptr);
    auto elementInput = std::make_shared<ElementInput>();
    AppOptions options;
    options.graphicsDevice = device;
    options.elementInput = elementInput;
    // The visuals are render components: without the system they cannot exist.
    options.registerComponentSystem<RenderComponentSystem>();
    options.registerComponentSystem<ScreenComponentSystem>();
    options.registerComponentSystem<ElementComponentSystem>();
    engine->init(options);

    std::cout << "loader\n";
    const auto msdfFont = loadBitmapFontResource(writeFont(dir, "msdf", true).string(), device);
    const auto bitmapFont = loadBitmapFontResource(writeFont(dir, "bitmap", false).string(), device);
    check(msdfFont.has_value() && *msdfFont, "the MSDF font loads");
    check(bitmapFont.has_value() && *bitmapFont, "the bitmap font loads");
    if (!msdfFont || !*msdfFont || !bitmapFont || !*bitmapFont) {
        return 1;
    }
    FontResource* msdf = *msdfFont;
    FontResource* bitmap = *bitmapFont;
    check(msdf->msdf && !bitmap->msdf, "a glyph range marks the font MSDF, its absence a bitmap font");
    check(msdf->pages.size() == 2, "both pages load (<name>.png and <name>1.png)");
    check(msdf->pages.size() == 2 && msdf->pages[0]->width() == 64 && msdf->pages[1]->width() == 32,
          "each page keeps its own size");
    check(near(msdf->pxRange, 6.0f), "pxrange is scale x range (1.5 x 4)");
    check(near(msdf->intensity, 0.25f), "intensity is read");
    check(msdf->glyphs[65].page == 0 && msdf->glyphs[66].page == 1, "each glyph keeps its page");
    // Every pair of a row, not just its first: the value lookup searched from the key's
    // closing quote and missed the key itself, so no font's kerning ever loaded.
    check(near(msdf->kerningValue(65, 66), -2.5f) && near(msdf->kerningValue(65, 65), 0.75f) &&
          near(msdf->kerningValue(66, 65), -1.0f), "every kerning pair loads");
    check(near(msdf->minY, -5.0f) && near(msdf->maxY, 24.0f),
          "the font's vertical extent is the union of the glyph bounds (upstream _fontMinY / _fontMaxY)");

    {
        // Format version 3 (upstream's roboto) keys glyphs by the LETTER, escaped where JSON
        // needs it; the code point is the glyph's `id`. Reading the key as the code point
        // lost every letter and put the digits on codes 0-9.
        writePng(dir / "v3.png", 64, 32);
        const auto json = dir / "v3.json";
        {
            std::ofstream out(json);
            out << R"({"version":3,"type":"msdf","intensity":0,"info":{"face":"t","maps":[{"width":64,"height":32}]},"chars":{)"
                << R"("A":{"id":65,"letter":"A","x":0,"y":0,"width":8,"height":8,"map":0,"xadvance":5,"xoffset":0,"yoffset":0,"scale":1,"range":8},)"
                << R"("\"":{"id":34,"letter":"\"","x":8,"y":0,"width":8,"height":8,"map":0,"xadvance":5,"xoffset":0,"yoffset":0,"scale":1,"range":8},)"
                << R"("\\":{"id":92,"letter":"\\","x":16,"y":0,"width":8,"height":8,"map":0,"xadvance":5,"xoffset":0,"yoffset":0,"scale":1,"range":8},)"
                << R"("7":{"id":55,"letter":"7","x":24,"y":0,"width":8,"height":8,"map":0,"xadvance":5,"xoffset":0,"yoffset":0,"scale":1,"range":8})"
                << R"(},"kerning":{}})";
        }
        const auto v3 = loadBitmapFontResource(json.string(), device);
        check(v3.has_value() && *v3, "a version 3 font loads");
        if (v3 && *v3) {
            FontResource* font = *v3;
            check(font->glyphs.size() == 4, "all four letter-keyed glyphs are read");
            check(font->glyphs.count(65) && font->glyphs.count(34) && font->glyphs.count(92) && font->glyphs.count(55),
                  "each under its code point from `id`, escaped keys included");
            check(!font->glyphs.count(7), "the key \"7\" is not taken as code point 7");
            check(font->glyphs.count(92) && font->glyphs[92].x == 16.0f, "the backslash glyph's fields are its own");
            delete font;
        }
    }

    std::cout << "text visual\n";
    auto* screenEntity = new Entity();
    screenEntity->setEngine(engine.get());
    engine->root()->addChild(std::unique_ptr<GraphNode>(screenEntity));
    static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>())->setScreenSpace(true);

    const auto addText = [&](FontResource* font) {
        auto* entity = new Entity();
        entity->setEngine(engine.get());
        screenEntity->addChild(std::unique_ptr<GraphNode>(entity));
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        element->setup({.type = ElementType::Text, .width = 100.0f, .height = 20.0f});
        element->setFontResource(font);
        element->setFontSize(16);
        element->setText("AB");
        return element;
    };
    ElementComponent* msdfText = addText(msdf);
    msdfText->setColor(Color(0.5f, 0.5f, 0.5f, 1.0f));
    msdfText->setOutlineColor(Color(0.5f, 0.25f, 1.0f, 0.75f));
    msdfText->setOutlineThickness(0.75f);
    msdfText->setShadowColor(Color(1.0f, 0.0f, 0.0f, 0.5f));
    msdfText->setShadowOffset(Vector2(1.0f, -2.0f));
    ElementComponent* bitmapText = addText(bitmap);

    elementInput->syncElements();

    const auto instances = visualInstances(msdfText->entity());
    check(instances.size() == 2, "text over two pages draws as two mesh instances");
    for (size_t i = 0; i < instances.size() && i < 2; ++i) {
        auto* material = dynamic_cast<StandardMaterial*>(instances[i]->material());
        const std::string page = "page " + std::to_string(i) + ": ";
        check(material && material->msdfMap() == msdf->pages[i], page + "its material samples its own page as MSDF");
        if (!material) {
            continue;
        }
        const MaterialUniforms& u = material->packedUniforms();
        const float w = static_cast<float>(msdf->pages[i]->width());
        const float h = static_cast<float>(msdf->pages[i]->height());
        check(near(u.msdfParams[0], 6.0f) && near(u.msdfParams[1], 0.25f), page + "pxrange and intensity");
        check(near(u.msdfParams[2], w) && near(u.msdfParams[3], h), page + "the page's own size (textureSize stand-in)");
        check(near(u.msdfOutlineShadow[0], 0.15f), page + "outline thickness x 0.2");
        check(near(u.msdfOutlineShadow[1], 0.005f), page + "shadow x offset x 0.005");
        check(near(u.msdfOutlineShadow[2], -(w / h) * 0.005f * -2.0f),
              page + "shadow y offset x 0.005 x -(page aspect), upstream's value as is");
        check(near(u.msdfOutlineColor[0], std::pow(0.5f, 2.2f)) && near(u.msdfOutlineColor[1], std::pow(0.25f, 2.2f)) &&
              near(u.msdfOutlineColor[3], 0.75f), page + "outline colour linear, alpha straight");
        check(near(u.msdfShadowColor[0], 1.0f) && near(u.msdfShadowColor[3], 0.5f), page + "shadow colour linear");
    }

    const auto bitmapInstances = visualInstances(bitmapText->entity());
    check(bitmapInstances.size() == 2, "a bitmap font splits by page too");
    if (!bitmapInstances.empty()) {
        auto* material = dynamic_cast<StandardMaterial*>(bitmapInstances[0]->material());
        check(material && !material->msdfMap() && material->diffuseMap() == bitmap->pages[0],
              "a bitmap font's page is a diffuse map (coverage in alpha), not MSDF");
    }

    // Styling changes only the uniforms: the mesh instances survive.
    const MeshInstance* before = instances.empty() ? nullptr : instances[0];
    msdfText->setOutlineThickness(0.5f);
    elementInput->syncElements();
    const auto after = visualInstances(msdfText->entity());
    check(!after.empty() && after[0] == before, "restyling keeps the mesh instances");
    if (!after.empty()) {
        auto* material = dynamic_cast<StandardMaterial*>(after[0]->material());
        check(material && near(material->packedUniforms().msdfOutlineShadow[0], 0.1f), "and updates the thickness");
    }

    elementInput->detach();
    engine.reset();
    delete msdf;
    delete bitmap;
    std::cout << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
