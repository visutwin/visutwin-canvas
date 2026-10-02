// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// Localization: upstream's i18n.test.mjs, case for case (less its asset cases — localization
// assets are not ported), then a text element's `key`: its text
// follows the locale and data added later, setText clears it, a clone keeps it, and the
// text-localization example's data resolves fr-CA through fr-FR with Polish plural forms.

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "framework/appOptions.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/i18n/i18n.h"
#include "framework/input/elementInput.h"
#include "platform/graphics/graphicsDevice.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    void expectEq(const std::string& actual, const std::string& expected, const std::string& what)
    {
        check(actual == expected, what + " -> '" + actual + "' (expected '" + expected + "')");
    }

    // One key's translations as a whole data file. `value` is JSON.
    std::string translation(const std::string& locale, const std::string& key, const std::string& value)
    {
        return R"({"header": {"version": 1}, "data": [{"info": {"locale": ")" + locale +
               R"("}, "messages": {")" + key + R"(": )" + value + "}}]}";
    }

    std::string addText(I18n& i18n, const std::string& locale, const std::string& key, const std::string& value)
    {
        const std::string data = translation(locale, key, value);
        i18n.addData(data);
        return data;
    }

    // getPluralText(key) with no number matches no rule.
    const double kNoNumber = std::numeric_limits<double>::quiet_NaN();

    const std::vector<std::pair<std::string, std::string>> kFallbacks = {
        {"en", "en-US"}, {"es", "es-ES"}, {"zh", "zh-CN"}, {"fr", "fr-FR"},
        {"de", "de-DE"}, {"it", "it-IT"}, {"ru", "ru-RU"}, {"ja", "ja-JP"}};

    void testFindAvailableLocale()
    {
        std::cout << "findAvailableLocale\n";
        {
            I18n i18n;
            addText(i18n, "no-IT", "key", R"("norwegian")");
            expectEq(i18n.findAvailableLocale("no-IT"), "no-IT", "a locale with translations is itself");
            expectEq(i18n.findAvailableLocale("de-DE"), "en-US", "no translations falls back to en-US");
        }
        {
            I18n i18n;
            addText(i18n, "zh-CN", "key", R"("Chinese")");
            expectEq(i18n.findAvailableLocale("zh-SG"), "zh-CN", "zh-SG falls back to zh-CN");
        }
        {
            I18n i18n;
            addText(i18n, "en-GB", "key", R"("British")");
            expectEq(i18n.findAvailableLocale("en-US"), "en-GB", "en-US falls back to en-GB");
        }
        {
            I18n i18n;
            addText(i18n, "es-MX", "key", R"("Mexican Spanish")");
            addText(i18n, "es-ES", "key", R"("Spanish")");
            expectEq(i18n.findAvailableLocale("es-AR"), "es-ES", "es-AR prefers es-ES to es-MX");
        }
        expectEq(I18n::findAvailableLocale("fr-CA", {"en-US", "fr-FR"}), "fr-FR", "static: fr-CA -> fr-FR");
        expectEq(I18n::findAvailableLocale("xx-YY", {"fr-FR"}), "en-US", "static: nothing -> en-US");
    }

    void testGetPluralText()
    {
        std::cout << "getPluralText\n";
        {
            I18n i18n;
            expectEq(i18n.getPluralText("key", kNoNumber), "key", "no translations: key");
            addText(i18n, "no-NO", "key", R"(["translated"])");
            expectEq(i18n.getPluralText("key", kNoNumber), "key", "another locale's translation: key");
        }
        {
            I18n i18n;
            addText(i18n, "no-NO", "key", R"(["norwegian"])");
            expectEq(i18n.getPluralText("key2", kNoNumber, "no-NO"), "key2", "missing key: key");
            i18n.setLocale("no-NO");
            expectEq(i18n.getPluralText("key2", kNoNumber), "key2", "missing key, current locale: key");
        }
        {
            I18n i18n;
            addText(i18n, "en-US", "key", R"(["english one", "english other"])");
            expectEq(i18n.getPluralText("key", 1, "no-NO"), "english one", "no-NO falls back to en-US");
            i18n.setLocale("no-NO");
            expectEq(i18n.getPluralText("key", 1), "english one", "current no-NO falls back to en-US");
        }
        {
            I18n i18n;
            addText(i18n, "en-US", "key", R"(["english one", "english other"])");
            expectEq(i18n.getPluralText("key", 1, "ar"), "english one", "ar falls back to en-US's rule");
            i18n.setLocale("ar");
            expectEq(i18n.getPluralText("key", 1), "english one", "current ar falls back to en-US's rule");
        }
        {
            I18n i18n;
            addText(i18n, "en-US", "key", R"(["", ""])");
            bool allEmpty = true;
            for (const double n : {0.0, 1.0, 2.0}) {
                allEmpty &= i18n.getPluralText("key", n).empty();
            }
            for (const std::string locale : {"no-NO", "ar"}) {
                for (const double n : {0.0, 1.0, 2.0}) {
                    allEmpty &= i18n.getPluralText("key", n, locale).empty();
                }
                i18n.setLocale(locale);
                for (const double n : {0.0, 1.0, 2.0}) {
                    allEmpty &= i18n.getPluralText("key", n).empty();
                }
                addText(i18n, locale, "key", R"(["", "", ""])");
                for (const double n : {0.0, 1.0, 2.0}) {
                    allEmpty &= i18n.getPluralText("key", n).empty();
                }
            }
            check(allEmpty, "an empty string is a valid translation");
        }
        {
            I18n i18n;
            addText(i18n, "en-US", "key", "[null, null]");
            bool allKey = true;
            for (const double n : {0.0, 1.0, 2.0}) {
                allKey &= i18n.getPluralText("key", n) == "key";
            }
            for (const std::string locale : {"no-NO", "ar"}) {
                for (const double n : {0.0, 1.0, 2.0}) {
                    allKey &= i18n.getPluralText("key", n, locale) == "key";
                }
                i18n.setLocale(locale);
                for (const double n : {0.0, 1.0, 2.0}) {
                    allKey &= i18n.getPluralText("key", n) == "key";
                }
                addText(i18n, locale, "key", "[null, null, null]");
                for (const double n : {0.0, 1.0, 2.0}) {
                    allKey &= i18n.getPluralText("key", n) == "key";
                }
            }
            addText(i18n, "es-ES", "key", "null");
            allKey &= i18n.getPluralText("key", 2, "es-ES") == "key";
            check(allKey, "a null translation gives the key");
        }
        for (const bool justLanguage : {false, true}) {
            I18n i18n;
            for (const auto& [lang, locale] : kFallbacks) {
                addText(i18n, locale, "key", "[\"language " + lang + "\"]");
            }
            addText(i18n, "no-NO", "key", R"(["language no"])");
            bool allMatch = true;
            for (const auto& [lang, locale] : kFallbacks) {
                const std::string asked = justLanguage ? lang : lang + "-alt";
                allMatch &= i18n.getPluralText("key", 1, asked) == "language " + lang;
                i18n.setLocale(asked);
                allMatch &= i18n.getPluralText("key", 1) == "language " + lang;
            }
            const std::string no = justLanguage ? "no" : "no-alt";
            allMatch &= i18n.getPluralText("key", 1, no) == "language no";
            i18n.setLocale(no);
            allMatch &= i18n.getPluralText("key", 1) == "language no";
            check(allMatch, justLanguage ? "a bare language falls back to its default locale"
                                         : "a missing locale falls back to its language's default");
        }
        {
            I18n i18n;
            addText(i18n, "no-IT", "key", R"(["norwegian"])");
            expectEq(i18n.getPluralText("key", 1, "no-NO"), "norwegian", "first locale of the language");
            i18n.setLocale("no-NO");
            expectEq(i18n.getPluralText("key", 1), "norwegian", "first locale of the language, current");
        }

        // The CLDR rules, asked with an explicit locale and as the current locale.
        struct Case
        {
            double n;
            const char* form;
        };
        const auto rule = [](const std::vector<std::string>& locales, const std::string& forms,
                             const std::vector<Case>& cases, const std::string& name) {
            I18n i18n;
            for (const auto& locale : locales) {
                addText(i18n, locale, "key", forms);
            }
            bool allMatch = true;
            for (const auto& locale : locales) {
                for (const auto& c : cases) {
                    allMatch &= i18n.getPluralText("key", c.n, locale) == c.form;
                }
                i18n.setLocale(locale);
                for (const auto& c : cases) {
                    const bool match = i18n.getPluralText("key", c.n) == c.form;
                    if (!match) {
                        std::cout << "    " << locale << " n=" << c.n << " -> " << i18n.getPluralText("key", c.n)
                                  << " (expected " << c.form << ")\n";
                    }
                    allMatch &= match;
                }
            }
            check(allMatch, "plural forms for " + name);
        };
        rule({"ja-JP", "ko-KO", "th-TH", "vi-VI", "zh-ZH"}, R"(["other"])", {{0, "other"}, {1, "other"}},
             "ja, ko, th, vi, zh");
        rule({"fa-FA", "hi-HI"}, R"(["one", "other"])",
             {{0, "one"}, {1, "one"}, {0.5, "one"}, {-1, "other"}, {1.1, "other"}, {2, "other"}}, "fa, hi");
        rule({"fr-FR"}, R"(["one", "other"])",
             {{0, "one"}, {1, "one"}, {1.9999, "one"}, {-1, "other"}, {2, "other"}}, "fr");
        rule({"en-US", "en-GB", "de-DE", "it-IT", "el-GR", "es-ES", "tr-TR"}, R"(["one", "other"])",
             {{1, "one"}, {2, "other"}, {0, "other"}, {0.5, "other"}, {1.5, "other"}}, "en, de, it, el, es, tr");
        rule({"ru-RU", "uk-UK"}, R"(["one", "few", "many", "other"])",
             {{1, "one"}, {21, "one"}, {101, "one"}, {1001, "one"}, {2, "few"}, {3, "few"}, {22, "few"},
              {24, "few"}, {1002, "few"}, {0, "many"}, {5, "many"}, {11, "many"}, {14, "many"}, {19, "many"},
              {114, "many"}, {100, "many"}, {10000, "many"}, {1.1, "other"}, {1000.5, "other"}},
             "ru, uk");
        rule({"ar-AR"}, R"(["zero", "one", "two", "few", "many", "other"])",
             {{0, "zero"}, {1, "one"}, {2, "two"}, {3, "few"}, {10, "few"}, {103, "few"}, {110, "few"},
              {11, "many"}, {26, "many"}, {111, "many"}, {1011, "many"}, {100, "other"}, {102, "other"},
              {200, "other"}, {202, "other"}, {500, "other"}, {502, "other"}, {600, "other"}, {1000, "other"},
              {10000, "other"}, {0.1, "other"}, {10.1, "other"}},
             "ar");
        // Polish is not in the ported test; its rule, from the CLDR chart.
        rule({"pl-PL"}, R"(["one", "few", "many", "other"])",
             {{1, "one"}, {2, "few"}, {4, "few"}, {22, "few"}, {0, "many"}, {5, "many"}, {11, "many"},
              {12, "many"}, {14, "many"}, {21, "many"}, {112, "many"}, {1.5, "other"}},
             "pl");

        const auto chinese = [](const std::vector<std::pair<std::string, std::string>>& data,
                                const std::string& locale, const std::string& expected, const std::string& name,
                                const bool plural) {
            I18n i18n;
            for (const auto& [l, v] : data) {
                addText(i18n, l, "key", plural ? "[\"" + v + "\"]" : "\"" + v + "\"");
            }
            i18n.setLocale(locale);
            expectEq(plural ? i18n.getPluralText("key", kNoNumber) : i18n.getText("key"), expected, name);
        };
        for (const bool plural : {true, false}) {
            const std::string how = plural ? " (plural)" : " (text)";
            chinese({{"zh-CN", "cn"}, {"zh-HK", "hk"}, {"zh-TW", "tw"}}, "zh-HK", "hk", "zh-HK uses zh-HK" + how, plural);
            chinese({{"zh-CN", "cn"}, {"zh-TW", "hk"}}, "zh-HK", "hk", "zh-HK falls back to zh-TW" + how, plural);
            chinese({{"zh-CN", "cn"}, {"zh-HK", "tw"}}, "zh-TW", "tw", "zh-TW falls back to zh-HK" + how, plural);
            chinese({{"zh-HK", "hk"}, {"zh-CN", "cn"}, {"zh-TW", "tw"}}, "zh-SG", "cn", "zh-SG falls back to zh-CN" + how, plural);
        }
    }

    void testGetText()
    {
        std::cout << "getText\n";
        {
            I18n i18n;
            expectEq(i18n.getText("key"), "key", "no translations: key");
            addText(i18n, "no-NO", "key", R"("translated")");
            expectEq(i18n.getText("key"), "key", "another locale's translation: key");
        }
        {
            I18n i18n;
            addText(i18n, "no-NO", "key", R"("translated")");
            expectEq(i18n.getText("key", "no-NO"), "translated", "the locale's translation");
            i18n.setLocale("no-NO");
            expectEq(i18n.getText("key"), "translated", "the current locale's translation");
        }
        {
            I18n i18n;
            addText(i18n, "en-US", "key", R"("english")");
            expectEq(i18n.getText("key", "no-NO"), "english", "no-NO falls back to en-US");
            i18n.setLocale("no-NO");
            expectEq(i18n.getText("key"), "english", "current no-NO falls back to en-US");
            addText(i18n, "no-NO", "key", R"("norwegian")");
            expectEq(i18n.getText("key", "no-NO"), "norwegian", "then no-NO's own");
            expectEq(i18n.getText("key"), "norwegian", "then no-NO's own, current");
        }
        {
            I18n i18n;
            addText(i18n, "no-NO", "key", R"("norwegian")");
            expectEq(i18n.getText("key2", "no-NO"), "key2", "missing key: key");
            expectEq(i18n.getText("key", "no-IT"), "norwegian", "no-IT falls back to the language's locale");
            expectEq(i18n.getText("key", "no"), "norwegian", "a bare language falls back to its locale");
            i18n.setLocale("no-IT");
            expectEq(i18n.getText("key"), "norwegian", "current no-IT falls back");
        }
        {
            I18n i18n;
            addText(i18n, "no-IT", "key", R"(["one", "other"])");
            expectEq(i18n.getText("key", "no-NO"), "one", "a plural key's first form");
        }
        {
            I18n i18n;
            addText(i18n, "en-US", "key", R"("")");
            expectEq(i18n.getText("key"), "", "an empty string is a valid translation");
            expectEq(i18n.getText("key", "no-NO"), "", "an empty string, through a fallback");
        }
        {
            I18n i18n;
            addText(i18n, "en-US", "key", "null");
            expectEq(i18n.getText("key"), "key", "a null translation gives the key");
            i18n.setLocale("no-NO");
            expectEq(i18n.getText("key"), "key", "a null translation gives the key, through a fallback");
        }
    }

    void testLocaleAndData()
    {
        std::cout << "locale and data\n";
        {
            I18n i18n;
            i18n.setLocale("id");
            expectEq(i18n.locale(), "id", "id");
            i18n.setLocale("id-ID");
            expectEq(i18n.locale(), "id-ID", "id-ID");
            i18n.setLocale("in");
            expectEq(i18n.locale(), "id", "in becomes id");
            i18n.setLocale("in-ID");
            expectEq(i18n.locale(), "id-ID", "in-ID becomes id-ID");
            i18n.setLocale("en");
            expectEq(i18n.locale(), "en", "en");
        }
        {
            I18n i18n;
            int changes = 0;
            std::string lastNew;
            std::string lastOld;
            i18n.on("change", [&](const std::string& locale, const std::string& old) {
                ++changes;
                lastNew = locale;
                lastOld = old;
            });
            i18n.setLocale("fr-CA");
            i18n.setLocale("fr-CA");
            check(changes == 1 && lastNew == "fr-CA" && lastOld == "en-US",
                  "\"change\" fires once per change, with the new and old locale");
        }
        {
            I18n i18n;
            const std::string data1 = addText(i18n, "en-US", "key", R"("translation")");
            const std::string data2 = addText(i18n, "en-US", "key2", R"("translation2")");
            const std::string data3 = addText(i18n, "no-IT", "key3", R"("translation3")");
            bool all = i18n.getText("key") == "translation" && i18n.getText("key2") == "translation2" &&
                       i18n.getText("key3", "no-IT") == "translation3" && i18n.getText("key3", "no") == "translation3";
            i18n.removeData(data1);
            all &= i18n.getText("key") == "key" && i18n.getText("key2") == "translation2" &&
                   i18n.getText("key3", "no") == "translation3";
            i18n.removeData(data2);
            all &= i18n.getText("key2") == "key2" && i18n.getText("key3", "no-IT") == "translation3";
            i18n.removeData(data3);
            all &= i18n.getText("key3", "no-IT") == "key3" && i18n.getText("key3", "no") == "key3";
            check(all, "removeData removes all data correctly");
        }
        {
            I18n i18n;
            check(!i18n.addData("{\"data\": []}"), "data without a header is refused");
            check(!i18n.addData(R"({"header": {"version": 2}, "data": []})"), "a version other than 1 is refused");
            check(!i18n.addData(R"({"header": {"version": 1}, "data": [{"messages": {}}]})"),
                  "an entry without info is refused");
            check(!i18n.addData("not json"), "text that is not JSON is refused");
        }
    }

    void testTextElementKey()
    {
        std::cout << "text element key\n";
        auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.size = {300, 150}, .resizable = true});
        auto engine = makeTestEngine<RenderComponentSystem, ScreenComponentSystem, ElementComponentSystem>(device,
            [](AppOptions& options) { options.elementInput = std::make_shared<ElementInput>(); });
        I18n* i18n = engine->i18n();
        check(i18n != nullptr, "the engine has an I18n");
        if (!i18n) {
            return;
        }
        check(i18n->addDataFromFile(std::string(VISUTWIN_ASSETS_DIR) + "/localization/text-localization.json"),
              "the text-localization example's data loads");

        std::vector<std::unique_ptr<Entity>> owned;
        const auto text = [&]() {
            owned.push_back(std::make_unique<Entity>());
            Entity* entity = owned.back().get();
            entity->setEngine(engine.get());
            auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
            element->setup({.type = ElementType::Text});
            return element;
        };

        ElementComponent* title = text();
        title->setKey("title");
        expectEq(title->text(), "Treasure Hunt", "a keyed element shows the current locale's message");
        i18n->setLocale("fr-CA");
        expectEq(title->text(), "Chasse au trésor", "it follows the locale; fr-CA falls back to fr-FR");
        expectEq(i18n->findAvailableLocale("fr-CA"), "fr-FR", "fr-CA is served by fr-FR");
        i18n->setLocale("pl-PL");
        expectEq(title->text(), "Poszukiwanie skarbu", "and to Polish");

        ElementComponent* late = text();
        late->setKey("greeting");
        expectEq(late->text(), "greeting", "a key with no message shows the key");
        i18n->addData(translation("pl-PL", "greeting", R"("Cześć")"));
        expectEq(late->text(), "Cześć", "data added later for the key updates the text");

        owned.push_back(std::make_unique<Entity>());
        Entity* copy = static_cast<Entity*>(title->entity()->clone());
        owned.back().reset(copy);
        copy->setEngine(engine.get());
        auto* copied = copy->findComponent<ElementComponent>();
        expectEq(copied ? copied->key() : std::string(), "title", "a clone keeps the key");
        i18n->setLocale("es-ES");
        expectEq(copied ? copied->text() : std::string(), "Caza del tesoro", "and follows the locale");

        title->setText("Plain");
        i18n->setLocale("en-US");
        check(title->key().empty() && title->text() == "Plain", "setText clears the key; the text stays put");

        // The example's purse: Polish takes the few and many forms its data carries.
        const auto coins = [&](const double n) { return i18n->getPluralText("coins", n, "pl-PL"); };
        expectEq(coins(1), "Masz {number} monetę", "pl one");
        expectEq(coins(3), "Masz {number} monety", "pl few");
        expectEq(coins(5), "Masz {number} monet", "pl many");
        expectEq(i18n->getPluralText("coins", 1, "fr-CA"), "Vous avez {number} pièce", "fr-CA one, via fr-FR");
        expectEq(i18n->getPluralText("coins", 0, "fr-CA"), "Vous avez {number} pièce", "fr 0 is one");
        expectEq(i18n->getPluralText("coins", 2, "es-ES"), "Tienes {number} monedas", "es other");
    }
}

int main()
{
    std::cout << std::unitbuf;
    testFindAvailableLocale();
    testGetPluralText();
    testGetText();
    testLocaleAndData();
    testTextElementKey();
    return finish("i18n");
}
