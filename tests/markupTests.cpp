// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Text markup (framework/components/element/markup.h), upstream's markup.test.mjs cases
// that are not about JavaScript's prototype chain, plus the grammar the upstream
// text-markup example leans on: attributes, nesting, escapes and the error path.

#include <iostream>
#include <string>

#include "framework/components/element/markup.h"

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

    std::string valueOf(const MarkupResult& r, const size_t i, const std::string& tag)
    {
        if (i >= r.tags.size() || !r.tags[i]) {
            return "<none>";
        }
        const auto it = r.tags[i]->find(tag);
        if (it == r.tags[i]->end()) {
            return "<absent>";
        }
        return it->second.value ? *it->second.value : "<null>";
    }

    std::string attributeOf(const MarkupResult& r, const size_t i, const std::string& tag, const std::string& key)
    {
        if (i >= r.tags.size() || !r.tags[i]) {
            return "<none>";
        }
        const auto it = r.tags[i]->find(tag);
        if (it == r.tags[i]->end()) {
            return "<absent>";
        }
        const auto a = it->second.attributes.find(key);
        return a != it->second.attributes.end() ? a->second : "<absent>";
    }
}

int main()
{
    std::cout << "upstream markup.test.mjs\n";
    {
        const auto r = evaluateMarkup(R"([color="#ff0000"]hi[/color])");
        check(r.error.empty() && r.symbols == "hi", "strips tags and returns the visible symbols");
        check(r.tags.size() == 2 && valueOf(r, 0, "color") == "#ff0000", "each symbol carries its tag");
    }
    {
        const auto r = evaluateMarkup(R"([color="#ff0000"][size="x"]hi[/size][/color])");
        check(valueOf(r, 0, "color") == "#ff0000" && valueOf(r, 0, "size") == "x",
              "a surrounding and an inner tag merge");
    }
    {
        const auto r = evaluateMarkup(R"([color="a"]h[color="b"]i[/color][/color])");
        check(r.symbols == "hi" && valueOf(r, 0, "color") == "a" && valueOf(r, 1, "color") == "b",
              "a nested tag of the same name overrides the outer one");
    }

    std::cout << "grammar\n";
    {
        const auto r = evaluateMarkup(R"(plain text)");
        check(r.error.empty() && r.symbols == "plain text" && r.tags.empty(), "text without tags has no tag list");
    }
    {
        const auto r = evaluateMarkup(R"(for [outline color="#c43c2c" thickness="0.8"]42[/outline] damage)");
        check(r.symbols == "for 42 damage", "attributes are stripped with their tag");
        check(attributeOf(r, 4, "outline", "color") == "#c43c2c" && attributeOf(r, 5, "outline", "thickness") == "0.8",
              "attributes are read");
        check(valueOf(r, 4, "outline") == "<null>", "a tag with attributes and no value has a null value");
        check(!r.tags[0] && !r.tags[7], "symbols outside every tag carry none");
    }
    {
        const auto r = evaluateMarkup(R"([outline color="#111111"]a[outline thickness="0.5"]b[/outline]c[/outline])");
        check(attributeOf(r, 1, "outline", "color") == "#111111" && attributeOf(r, 1, "outline", "thickness") == "0.5",
              "nested attributes merge key by key");
        check(attributeOf(r, 2, "outline", "thickness") == "<absent>", "and the inner ones end with their tag");
    }
    {
        const auto r = evaluateMarkup(R"(back in five, I am \[AFK])");
        check(r.error.empty() && r.symbols == "back in five, I am [AFK]", "a backslash escapes an opening bracket");
    }
    {
        const auto r = evaluateMarkup(R"(a\b)");
        check(r.symbols == R"(a\b)", "any other backslash is kept");
    }
    {
        const auto r = evaluateMarkup(R"([shadow color="#8a3000" offset="0.6"][color="#ffb347"]Crit[/color][/shadow] x)");
        check(r.symbols == "Crit x" && valueOf(r, 0, "color") == "#ffb347" &&
              attributeOf(r, 3, "shadow", "offset") == "0.6", "two tags open over the same symbols");
    }

    std::cout << "errors: the text is drawn as written\n";
    {
        const std::string text = R"([color="#88e088"]Quest complete: Wolves at the Gate)";
        const auto r = evaluateMarkup(text);
        check(!r.error.empty() && r.symbols == text && r.tags.empty(), "an unclosed tag returns the text unchanged");
    }
    {
        const std::string text = R"(a[/color]b)";
        const auto r = evaluateMarkup(text);
        check(!r.error.empty() && r.symbols == text, "a closing tag with no open tag is an error");
    }
    {
        const std::string text = R"([color=#ff0000]x[/color])";
        const auto r = evaluateMarkup(text);
        check(!r.error.empty() && r.symbols == text, "an unquoted value is an error");
    }
    {
        const std::string text = R"([color="#ff0000)";
        const auto r = evaluateMarkup(text);
        check(!r.error.empty() && r.symbols == text, "an unterminated string is an error");
    }

    std::cout << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
