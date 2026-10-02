// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// A text element's auto fit and max lines (the cases of upstream's
// text-element.test.mjs for autoFitWidth / autoFitHeight / minFontSize / maxFontSize and
// maxLines), on the shipped Roboto font.
//
// Those cases' test font gives some of their expectations as literals (a 50-unit box fits 'ab\nab'
// at 24). Here those are held by what makes them right on ANY font: the fitted size is the
// largest one the text fits at — at that size it fits, one size up it does not, measured by the
// layout itself — and the width fit is floor(32 x width / text width).

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/appOptions.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/element/textLayout.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/handlers/fontResource.h"
#include "framework/input/elementInput.h"
#include "platform/graphics/graphicsDevice.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    std::shared_ptr<Engine> engine;
    FontResource* font = nullptr;
    std::vector<std::unique_ptr<Entity>> owned;

    /// A text element on no screen, set up as those cases are: autoWidth and autoHeight off.
    ElementComponent* text()
    {
        owned.push_back(std::make_unique<Entity>());
        Entity* entity = owned.back().get();
        entity->setEngine(engine.get());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        element->setup({.type = ElementType::Text});
        element->setFontResource(font);
        element->setAutoWidth(false);
        element->setAutoHeight(false);
        return element;
    }

    /// The contents of each line, line breaks left out.
    std::vector<std::string> lineContents(const ElementComponent* element)
    {
        std::vector<std::string> lines;
        const TextMeasure measure = element->measureLayout();
        const std::u32string& symbols = element->textCodePoints();
        for (const TextLine& line : measure.lines) {
            std::string content;
            for (size_t i = line.begin; i < line.end; ++i) {
                if (symbols[i] != U'\n' && symbols[i] != U'\r') {
                    content += static_cast<char>(symbols[i]);
                }
            }
            lines.push_back(content);
        }
        return lines;
    }

    std::string join(const std::vector<std::string>& lines)
    {
        std::string out;
        for (const auto& line : lines) {
            out += (out.empty() ? "[" : ", [") + line + "]";
        }
        return out;
    }

    /// The text's measure at `size`, with the line height scaled as auto fit scales it.
    TextMeasure measureAt(const ElementComponent* element, const int size)
    {
        const float step = element->lineHeight() * static_cast<float>(size) / static_cast<float>(element->maxFontSize());
        return measureText(*font, element->textCodePoints(), static_cast<float>(size), step,
                           element->textMaxLineWidth(), element->spacing(),
                           element->wrapLines() ? element->maxLines() : -1);
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.size = {300, 150}, .resizable = true});
    engine = makeTestEngine<RenderComponentSystem, ScreenComponentSystem, ElementComponentSystem>(device,
        [](AppOptions& options) { options.elementInput = std::make_shared<ElementInput>(); });

    const auto loaded = loadBitmapFontResource(std::string(VISUTWIN_ASSETS_DIR) + "/fonts/roboto-regular.json", device);
    if (!loaded || !*loaded) {
        std::cout << "  FAIL the Roboto font loads\n";
        return 1;
    }
    font = *loaded;

    std::cout << "max lines\n";
    {
        ElementComponent* e = text();
        e->setWrapLines(true);
        e->setWidth(100.0f);
        e->setMaxLines(1);
        e->setText("abcde fghij klmno pqrst uvwxyz");
        check(lineContents(e) == std::vector<std::string>{"abcde fghij klmno pqrst uvwxyz"},
              "maxLines 1: long contents stay on one line: " + join(lineContents(e)));
        e->setText("abcde\n\n\nfg\nhij");
        check(lineContents(e) == std::vector<std::string>{"abcdefghij"},
              "maxLines 1: line breaks run on: " + join(lineContents(e)));
        e->setText("abcde\rfghij");
        check(lineContents(e) == std::vector<std::string>{"abcdefghij"}, "maxLines 1: so does '\\r'");
        e->setWidth(1.0f);
        e->setText("abcdef ghijkl");
        check(lineContents(e) == std::vector<std::string>{"abcdef ghijkl"},
              "maxLines 1: no break between characters either");

        e->setWidth(100.0f);
        e->setMaxLines(2);
        e->setText("abcde\n\n\nfg\nhij");
        check(lineContents(e) == std::vector<std::string>{"abcde", "fghij"},
              "maxLines 2: the rest runs on in the second line: " + join(lineContents(e)));
        e->setText("abcde\rfghij");
        check(lineContents(e) == std::vector<std::string>{"abcde", "fghij"}, "maxLines 2: '\\r' breaks as '\\n'");
        e->setText("abcde fghij klmno pqrst uvwxyz");
        const auto wrapped = lineContents(e);
        check(wrapped.size() == 2 && wrapped[0] + wrapped[1] == "abcde fghij klmno pqrst uvwxyz" &&
                  wrapped[0].back() == ' ',
              "maxLines 2: broken once at a word, the rest on the second line: " + join(wrapped));
        e->setWidth(1.0f);
        e->setText("abcdef ghijkl");
        check(lineContents(e) == std::vector<std::string>{"a", "bcdef ghijkl"},
              "maxLines 2: a character per line until the last: " + join(lineContents(e)));

        ElementComponent* flat = text();
        flat->setMaxLines(1);
        flat->setText("ab\ncd");
        check(lineContents(flat) == std::vector<std::string>{"ab", "cd"},
              "text that does not wrap ignores maxLines, as upstream");

        ElementComponent* unlimited = text();
        unlimited->setText("ab\rcd");
        check(lineContents(unlimited) == std::vector<std::string>{"ab", "cd"}, "'\\r' is a line break");
    }

    std::cout << "auto fit width\n";
    {
        ElementComponent* e = text();
        e->setWidth(10.0f);
        e->setText("ab");
        const float textWidth = e->textWidth();
        e->setAutoFitWidth(true);
        const int expected = std::clamp(static_cast<int>(std::floor(32.0f * 10.0f / textWidth)), 8, 32);
        check(e->fontSize() == expected, "reduces the font to floor(32 x width / text width): " +
              std::to_string(e->fontSize()) + " vs " + std::to_string(expected));
        check(measureAt(e, e->fontSize()).width <= 10.0f && measureAt(e, e->fontSize() + 1).width > 10.0f,
              "... the largest size that fits");
        check(std::abs(e->measureLayout().lineStep - 32.0f * static_cast<float>(e->fontSize()) / 32.0f) < 1e-4f,
              "the line height scales with the fitted size");

        e->setWidth(200.0f);
        check(e->fontSize() == 32, "a wider element fits it again, at maxFontSize (the fit follows the width)");

        ElementComponent* off = text();
        off->setFontSize(20);
        off->setLineHeight(20.0f);
        off->setWidth(10.0f);
        off->setText("ab");
        check(off->fontSize() == 20 && off->measureLayout().lineStep == 20.0f, "autoFitWidth off: no fit");

        ElementComponent* autoWidth = text();
        autoWidth->setAutoWidth(true);
        autoWidth->setAutoFitWidth(true);
        autoWidth->setWidth(10.0f);
        autoWidth->setText("ab");
        check(autoWidth->fontSize() == 32, "autoFitWidth with autoWidth on: no fit");

        ElementComponent* tiny = text();
        tiny->setWidth(1.0f);
        tiny->setText("ab");
        tiny->setAutoFitWidth(true);
        check(tiny->fontSize() == tiny->minFontSize() && tiny->minFontSize() == 8, "not below minFontSize (8)");
        tiny->setText("abcdefghijklmn");
        tiny->setMinFontSize(4);
        check(tiny->fontSize() == 4, "a new minFontSize applies");

        ElementComponent* roomy = text();
        roomy->setMaxFontSize(10);
        roomy->setWidth(1000.0f);
        roomy->setText("ab");
        roomy->setAutoFitWidth(true);
        check(roomy->fontSize() == 10, "not above maxFontSize");
        roomy->setMaxFontSize(11);
        check(roomy->fontSize() == 11, "a new maxFontSize applies");
    }

    std::cout << "auto fit height\n";
    {
        ElementComponent* e = text();
        e->setHeight(50.0f);
        e->setText("ab\nab");
        e->setAutoFitHeight(true);
        const int size = e->fontSize();
        check(size < 32 && measureAt(e, size).height <= 50.0f && measureAt(e, size + 1).height > 50.0f,
              "reduces the font to the largest size whose height fits: " + std::to_string(size));
        check(std::abs(e->measureLayout().lineStep - static_cast<float>(size)) < 1e-4f,
              "the line height scales with it (32 x size / 32)");

        ElementComponent* off = text();
        off->setFontSize(20);
        off->setLineHeight(20.0f);
        off->setHeight(50.0f);
        off->setText("ab\nab");
        check(off->fontSize() == 20, "autoFitHeight off: no fit");

        ElementComponent* autoHeight = text();
        autoHeight->setAutoHeight(true);
        autoHeight->setAutoFitHeight(true);
        autoHeight->setHeight(50.0f);
        autoHeight->setText("ab\nab");
        check(autoHeight->fontSize() == 32, "autoFitHeight with autoHeight on: no fit");

        ElementComponent* tiny = text();
        tiny->setHeight(1.0f);
        tiny->setText("ab\nab");
        tiny->setAutoFitHeight(true);
        check(tiny->fontSize() == 8, "not below minFontSize");

        ElementComponent* roomy = text();
        roomy->setHeight(1000.0f);
        roomy->setMaxFontSize(8);
        roomy->setText("ab\nab");
        roomy->setAutoFitHeight(true);
        check(roomy->fontSize() == 8, "not above maxFontSize");
    }

    std::cout << "restoring the font size\n";
    {
        ElementComponent* e = text();
        e->setFontSize(44);
        e->setWidth(10.0f);
        e->setText("ab");
        e->setAutoFitWidth(true);
        check(e->fontSize() != 44, "fitting width changes the size");
        e->setAutoFitWidth(false);
        check(e->fontSize() == 44, "turning autoFitWidth off restores the size set");

        ElementComponent* a = text();
        a->setFontSize(44);
        a->setAutoWidth(true);
        a->setText("ab");
        a->setAutoFitWidth(true);
        check(a->fontSize() == 44, "autoFitWidth on while autoWidth is on changes nothing");

        ElementComponent* b = text();
        b->setFontSize(44);
        b->setWidth(10.0f);
        b->setHeight(1000.0f);
        b->setText("ab");
        b->setAutoFitWidth(true);
        b->setAutoFitHeight(true);
        b->setAutoFitWidth(false);
        check(b->fontSize() == b->maxFontSize(), "autoFitWidth off with autoFitHeight on: maxFontSize");

        ElementComponent* c = text();
        c->setFontSize(44);
        c->setHeight(50.0f);
        c->setText("ab\nab");
        c->setAutoFitHeight(true);
        check(c->fontSize() != 44, "fitting height changes the size");
        c->setAutoFitHeight(false);
        check(c->fontSize() == 44, "turning autoFitHeight off restores the size set");

        ElementComponent* d = text();
        d->setFontSize(44);
        d->setWidth(10.0f);
        d->setText("ab");
        d->setAutoFitWidth(true);
        d->setAutoWidth(true);
        check(d->fontSize() == 44, "autoWidth turning on restores the size set");

        ElementComponent* f = text();
        f->setFontSize(44);
        f->setHeight(50.0f);
        f->setText("ab\nab");
        f->setAutoFitHeight(true);
        f->setAutoHeight(true);
        check(f->fontSize() == 44, "autoHeight turning on restores the size set");

        ElementComponent* g = text();
        g->setFontSize(44);
        g->setHeight(50.0f);
        g->setWidth(1000.0f);
        g->setText("ab\nab");
        g->setAutoFitWidth(true);
        g->setAutoFitHeight(true);
        g->setAutoHeight(true);
        check(g->fontSize() == g->maxFontSize(), "autoHeight on with both fits on: maxFontSize");
    }

    owned.clear();
    return finish("text fit");
}
