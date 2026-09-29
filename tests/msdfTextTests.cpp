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
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <stb_image_write.h>

#include "framework/appOptions.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/element/textLayout.h"
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

    std::cout << "layout (upstream metrics)\n";
    {
        // The MSDF font: A and B advance 10 font units, kerning A->B -2.5 and B->A -1, bounds
        // spanning -5..24, no space glyph. At fontSize 32 a font unit is one element unit.
        const TextMeasure one = measureText(*msdf, "AB", 32.0f, 32.0f);
        check(one.lines.size() == 1 && near(one.width, 17.5f), "width is the kerned advance (10 - 2.5 + 10)");
        check(near(one.height, 29.0f), "height is the glyph-bounds extent (24 - -5)");
        const TextMeasure half = measureText(*msdf, "AB", 16.0f, 16.0f);
        check(near(half.width, 8.75f) && near(half.height, 14.5f), "metrics scale by fontSize / 32");
        const TextMeasure wrapped = measureText(*msdf, "AB AB", 32.0f, 32.0f, 20.0f);
        check(wrapped.lines.size() == 2 && near(wrapped.width, 17.5f), "a line wraps after the whitespace");
        check(near(wrapped.height, 24.0f + 32.0f + 5.0f), "and the block spans both lines");
        const TextMeasure longWord = measureText(*msdf, "ABABAB", 32.0f, 32.0f, 20.0f);
        check(longWord.lines.size() == 3, "a word longer than the line breaks between characters");
        const TextMeasure trailing = measureText(*msdf, "AB\n", 32.0f, 32.0f);
        check(near(trailing.height, 29.0f), "a trailing line break adds no height");
        const TextMeasure spaced = measureText(*msdf, "AB", 32.0f, 32.0f, std::numeric_limits<float>::infinity(), 1.4f);
        check(near(spaced.width, 1.4f * 17.5f), "spacing multiplies every advance, kerning included (1.4 x 17.5)");
        const TextMeasure spacedWrap = measureText(*msdf, "AB AB", 32.0f, 32.0f, 30.0f, 1.4f);
        check(spacedWrap.lines.size() == 2, "and wrapping measures the spread line (24.5 fits 30, the second word does not)");
        const TextMeasure empty = measureText(*msdf, "", 32.0f, 32.0f);
        check(near(empty.width, 0.0f) && near(empty.height, 0.0f), "empty text measures 0 x 0");
        // Symbols are code points: a glyph keyed 8230 (U+2026, three bytes in UTF-8) is one
        // symbol with its own advance, where a byte loop drew three missing glyphs.
        FontGlyph ellipsis = msdf->glyphs[65];
        ellipsis.id = 0x2026;
        msdf->glyphs[0x2026] = ellipsis;
        const TextMeasure dots = measureText(*msdf, "A\u2026", 32.0f, 32.0f);
        check(near(dots.width, 20.0f), "a multi-byte character lays out as one glyph (10 + 10)");
        msdf->glyphs.erase(0x2026);
    }

    std::cout << "UTF-8 decoding\n";
    {
        check(decodeUtf8("A\u00e9\u2026\U0001F600") == std::u32string{U'A', 0xE9, 0x2026, 0x1F600},
            "one, two, three and four byte sequences");
        check(decodeUtf8("\xC3") == std::u32string{0xFFFD}, "a truncated sequence is U+FFFD");
        check(decodeUtf8("\xC0\xAF") == std::u32string{0xFFFD, 0xFFFD}, "an overlong one is U+FFFD per byte");
        check(decodeUtf8("\xED\xA0\x80") == std::u32string{0xFFFD, 0xFFFD, 0xFFFD}, "a surrogate is malformed");
    }

    std::cout << "auto size\n";
    {
        ElementComponent* text = addText(msdf);
        text->setFontSize(32);
        text->setText("AB");
        check(near(text->width(), 17.5f) && near(text->height(), 29.0f), "autoWidth and autoHeight take the text's size");
        text->setAutoWidth(false);
        text->setWrapLines(true);
        text->setWidth(20.0f);
        text->setText("AB AB");
        check(near(text->height(), 61.0f), "wrapped at a set width, the height follows the lines");
        text->setWidth(40.0f);
        check(near(text->height(), 29.0f), "and a wider element re-wraps at once");
        ElementComponent* split = addText(msdf);
        split->setAnchor(Vector4(0.0f, 0.5f, 1.0f, 0.5f));
        const float before = split->width();
        split->setText("AB");
        check(near(split->width(), before), "a split axis keeps its own size");
    }

    std::cout << "justify (upstream justify)\n";
    {
        // No space glyph in the test font, so a space advances nothing: "AB AB" measures 35
        // (17.5 a word) and "AB AB AB" wraps at 40 after the second word.
        const std::u32string text = decodeUtf8("AB AB AB");
        const TextMeasure m = measureText(*msdf, text, 32.0f, 32.0f, 40.0f);
        check(m.lines.size() == 2 && m.lines[0].gaps == 1 && m.lines[1].gaps == 0,
              "a line broken at a word has its gap; the last line has none");
        const auto plain = placeText(*msdf, text, m, 40.0f, 80.0f, Vector2(0.0f, 0.0f), 0.0f, 1.0f, false);
        const auto justified = placeText(*msdf, text, m, 40.0f, 80.0f, Vector2(0.0f, 0.0f), 0.5f, 1.0f, true);
        const auto shift = [&](const size_t symbol) {
            float a = 0.0f, b = 0.0f;
            for (const auto& g : plain) if (g.symbol == symbol) a = g.x0;
            for (const auto& g : justified) if (g.symbol == symbol) b = g.x0;
            return b - a;
        };
        check(near(shift(0), 0.0f) && near(shift(1), 0.0f),
              "the justified line starts flush left, whatever the alignment");
        check(near(shift(3), 5.0f) && near(shift(4), 5.0f),
              "the word after the gap moves by the whole slack (40 - 35), so the line ends flush right");
        check(near(shift(6), 11.25f), "the last line keeps the alignment (centred: (40 - 17.5) / 2)");
        const TextMeasure broken = measureText(*msdf, decodeUtf8("AB AB\nAB"), 32.0f, 32.0f, 40.0f);
        check(broken.lines.size() == 2 && broken.lines[0].gaps == 0, "a line ended by a line break is not justified");
        const TextMeasure longWord = measureText(*msdf, decodeUtf8("ABABAB"), 32.0f, 32.0f, 20.0f);
        check(!longWord.lines.empty() && longWord.lines[0].gaps == 0, "nor is a word broken mid-word");
    }

    std::cout << "draw range (upstream rangeStart / rangeEnd)\n";
    {
        ElementComponent* text = addText(msdf);
        text->setText("ABAB");
        check(text->rangeStart() == 0 && text->rangeEnd() == 4, "setting the text draws all of it: rangeEnd is its length");
        elementInput->syncElements();
        const auto drawn = [&] {
            int indices = 0;
            int visible = 0;
            for (MeshInstance* mi : visualInstances(text->entity())) {
                if (mi->visible()) {
                    ++visible;
                    indices += mi->mesh()->getPrimitive(0).count;
                }
            }
            return std::pair<int, int>{indices, visible};
        };
        check(drawn() == std::pair<int, int>{24, 2}, "all four quads, on both pages");
        text->setRangeEnd(2);
        elementInput->syncElements();
        check(drawn() == std::pair<int, int>{12, 2}, "rangeEnd 2 draws the first A and B");
        text->setRangeStart(1);
        elementInput->syncElements();
        check(drawn() == std::pair<int, int>{6, 1}, "[1, 2) draws the B alone; the A page is hidden, not drawn empty");
        text->setRangeEnd(99);
        check(text->rangeEnd() == 4, "the end clamps to the text");
        text->setRangeEnd(0);
        check(text->rangeEnd() == 1, "and never falls below the start");
        text->setText("AB");
        check(text->rangeStart() == 0 && text->rangeEnd() == 2, "a new text resets the range");
    }

    std::cout << "markup\n";
    {
        ElementComponent* marked = addText(msdf);
        marked->setColor(Color(0.5f, 0.5f, 0.5f, 1.0f));
        marked->setEnableMarkup(true);
        marked->setText(R"([color="#ff0000"]A[/color]A)");
        check(marked->textSymbols() == "AA" && marked->markupTags().size() == 2, "tags are stripped from the drawn text");
        elementInput->syncElements();
        const auto parts = visualInstances(marked->entity());
        check(parts.size() == 2, "two styles on one page draw as two parts");
        bool red = false;
        bool own = false;
        for (auto* instance : parts) {
            auto* material = dynamic_cast<StandardMaterial*>(instance->material());
            if (material && near(material->emissive().r, 1.0f) && near(material->emissive().g, 0.0f)) {
                red = true;
            }
            if (material && near(material->emissive().r, 0.5f)) {
                own = true;
            }
        }
        check(red && own, "the tagged run is red, the rest keeps the element's colour");

        marked->setShadowOffset(Vector2(1.0f, 2.0f));
        elementInput->syncElements();
        const auto shadowed = visualInstances(marked->entity());
        auto* material = shadowed.empty() ? nullptr : dynamic_cast<StandardMaterial*>(shadowed[0]->material());
        check(material && near(material->packedUniforms().msdfOutlineShadow[1], 0.005f) &&
              near(material->packedUniforms().msdfOutlineShadow[2], 0.01f),
              "with tags, the shadow takes upstream's per-vertex convention (0.005 x, 0.005 y)");

        ElementComponent* broken = addText(msdf);
        broken->setEnableMarkup(true);
        broken->setText(R"([color="#ff0000"]AB)");
        check(broken->textSymbols() == R"([color="#ff0000"]AB)" && broken->markupTags().empty(),
              "an unclosed tag draws the text as written");
        ElementComponent* off = addText(msdf);
        off->setText(R"([color="#ff0000"]A[/color])");
        check(off->textSymbols() == R"([color="#ff0000"]A[/color])", "without enableMarkup, brackets are text");
    }

    elementInput->detach();
    engine.reset();
    delete msdf;
    delete bitmap;
    std::cout << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
