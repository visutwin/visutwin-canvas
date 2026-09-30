// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 12.10.2025.
//

#include "i18n.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    namespace
    {
        // Upstream DEFAULT_LOCALE_FALLBACKS: the locale to use for a locale, or a language,
        // that has no messages of its own.
        const std::map<std::string, std::string>& defaultLocaleFallbacks()
        {
            static const std::map<std::string, std::string> fallbacks = {
                {"en", "en-US"}, {"es", "es-ES"}, {"zh", "zh-CN"}, {"zh-HK", "zh-TW"},
                {"zh-TW", "zh-HK"}, {"zh-MO", "zh-HK"}, {"fr", "fr-FR"}, {"de", "de-DE"},
                {"it", "it-IT"}, {"ru", "ru-RU"}, {"ja", "ja-JP"}};
            return fallbacks;
        }

        const std::string* fallbackFor(const std::string& localeOrLang)
        {
            const auto& fallbacks = defaultLocaleFallbacks();
            const auto it = fallbacks.find(localeOrLang);
            return it != fallbacks.end() ? &it->second : nullptr;
        }

        bool isInteger(const double n) { return std::isfinite(n) && std::floor(n) == n; }

        // JS `%` on integers (sign of the dividend), for the CLDR rules below.
        double mod(const double n, const double m) { return std::fmod(n, m); }

        bool contains(const std::vector<std::string>& list, const std::string& value)
        {
            return std::find(list.begin(), list.end(), value) != list.end();
        }
    }

    I18n::I18n(const std::shared_ptr<Engine>& /*engine*/)
    {
        setLocale(DEFAULT_LOCALE);
    }

    std::string I18n::getLang(const std::string& locale)
    {
        const size_t index = locale.find('-');
        return index != std::string::npos ? locale.substr(0, index) : locale;
    }

    std::string I18n::replaceLang(const std::string& locale, const std::string& lang)
    {
        const size_t index = locale.find('-');
        return index != std::string::npos ? lang + locale.substr(index) : lang;
    }

    // Upstream utils.js PLURALS, from the CLDR plural rules.
    int I18n::pluralIndex(const std::string& lang, const double n)
    {
        static const std::vector<std::string> otherOnly = {"ja", "ko", "th", "vi", "zh", "id"};
        static const std::vector<std::string> zeroToOne = {"fa", "hi"};
        static const std::vector<std::string> belowTwo = {"fr", "pt"};
        static const std::vector<std::string> oneOther = {"de", "en", "it", "el", "es", "tr", "fi",
                                                          "sv", "nb", "no", "ur"};
        static const std::vector<std::string> slavic = {"ru", "uk"};

        if (contains(otherOnly, lang)) {
            return 0;
        }
        if (contains(zeroToOne, lang)) {
            return n >= 0.0 && n <= 1.0 ? 0 : 1;
        }
        if (contains(belowTwo, lang)) {
            return n >= 0.0 && n < 2.0 ? 0 : 1;
        }
        if (lang == "da") {
            return n == 1.0 || (!isInteger(n) && n >= 0.0 && n <= 1.0) ? 0 : 1;
        }
        if (contains(slavic, lang)) {
            if (isInteger(n)) {
                const double mod10 = mod(n, 10.0);
                const double mod100 = mod(n, 100.0);
                if (mod10 == 1.0 && mod100 != 11.0) {
                    return 0;   // one
                }
                if (mod10 >= 2.0 && mod10 <= 4.0 && (mod100 < 12.0 || mod100 > 14.0)) {
                    return 1;   // few
                }
                if (mod10 == 0.0 || (mod10 >= 5.0 && mod10 <= 9.0) || (mod100 >= 11.0 && mod100 <= 14.0)) {
                    return 2;   // many
                }
            }
            return 3;   // other
        }
        if (lang == "pl") {
            if (isInteger(n)) {
                if (n == 1.0) {
                    return 0;   // one
                }
                const double mod10 = mod(n, 10.0);
                const double mod100 = mod(n, 100.0);
                if (mod10 >= 2.0 && mod10 <= 4.0 && (mod100 < 12.0 || mod100 > 14.0)) {
                    return 1;   // few
                }
                if ((mod10 >= 0.0 && mod10 <= 1.0) || (mod10 >= 5.0 && mod10 <= 9.0) ||
                    (mod100 >= 12.0 && mod100 <= 14.0)) {
                    return 2;   // many
                }
            }
            return 3;   // other
        }
        if (lang == "ar") {
            if (n == 0.0) {
                return 0;   // zero
            }
            if (n == 1.0) {
                return 1;   // one
            }
            if (n == 2.0) {
                return 2;   // two
            }
            if (isInteger(n)) {
                const double mod100 = mod(n, 100.0);
                if (mod100 >= 3.0 && mod100 <= 10.0) {
                    return 3;   // few
                }
                if (mod100 >= 11.0 && mod100 <= 99.0) {
                    return 4;   // many
                }
            }
            return 5;   // other
        }
        // The listed "one, other" languages, and en's rule (upstream DEFAULT_PLURAL_FN) for any other.
        return n == 1.0 ? 0 : 1;
    }

    void I18n::setLocale(const std::string& value)
    {
        if (_locale == value) {
            return;
        }
        std::string locale = value;
        std::string lang = getLang(locale);
        if (lang == "in") {
            lang = "id";
            locale = replaceLang(locale, lang);
            if (_locale == locale) {
                return;
            }
        }
        const std::string old = _locale;
        _locale = locale;
        _lang = lang;
        fire("change", std::string(_locale), old);
    }

    std::string I18n::findAvailableLocale(const std::string& desiredLocale,
        const std::vector<std::string>& availableLocales)
    {
        if (contains(availableLocales, desiredLocale)) {
            return desiredLocale;
        }
        if (const std::string* fallback = fallbackFor(desiredLocale); fallback && contains(availableLocales, *fallback)) {
            return *fallback;
        }
        const std::string lang = getLang(desiredLocale);
        if (const std::string* fallback = fallbackFor(lang); fallback && contains(availableLocales, *fallback)) {
            return *fallback;
        }
        if (contains(availableLocales, lang)) {
            return lang;
        }
        return DEFAULT_LOCALE;
    }

    std::string I18n::findAvailableLocale(const std::string& desiredLocale) const
    {
        if (_translations.contains(desiredLocale)) {
            return desiredLocale;
        }
        return findFallbackLocale(desiredLocale, getLang(desiredLocale));
    }

    std::string I18n::findFallbackLocale(const std::string& locale, const std::string& lang) const
    {
        if (const std::string* result = fallbackFor(locale); result && _translations.contains(*result)) {
            return *result;
        }
        if (const std::string* result = fallbackFor(lang); result && _translations.contains(*result)) {
            return *result;
        }
        if (const auto it = _availableLangs.find(lang); it != _availableLangs.end() && _translations.contains(it->second)) {
            return it->second;
        }
        return DEFAULT_LOCALE;
    }

    const I18n::Messages* I18n::translations(const std::string& locale) const
    {
        const auto it = _translations.find(locale);
        return it != _translations.end() ? &it->second : nullptr;
    }

    std::string I18n::getText(const std::string& key, const std::string& locale) const
    {
        std::string target = locale.empty() ? _locale : locale;
        const Messages* messages = translations(target);
        if (!messages) {
            target = findFallbackLocale(target, locale.empty() ? _lang : getLang(target));
            messages = translations(target);
        }
        if (messages) {
            if (const auto it = messages->find(key); it != messages->end() && !it->second.empty() && it->second[0]) {
                return *it->second[0];
            }
        }
        return key;
    }

    std::string I18n::getPluralText(const std::string& key, const double n, const std::string& locale) const
    {
        std::string target = locale.empty() ? _locale : locale;
        std::string lang = locale.empty() ? _lang : getLang(target);
        const Messages* messages = translations(target);
        if (!messages) {
            target = findFallbackLocale(target, lang);
            lang = getLang(target);
            messages = translations(target);
        }
        if (messages) {
            if (const auto it = messages->find(key); it != messages->end()) {
                const int index = pluralIndex(lang, n);
                if (index >= 0 && static_cast<size_t>(index) < it->second.size() && it->second[index]) {
                    return *it->second[index];
                }
            }
        }
        return key;
    }

    // Upstream I18nParser: the validation it runs in debug builds, here always, since a
    // malformed file otherwise fails later and further from its cause.
    bool I18n::parse(const std::string& json, const char* caller, std::vector<ParsedEntry>& out)
    {
        const nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
        const auto fail = [caller](const std::string& reason) {
            spdlog::error("I18n::{}: failed to parse localization data: {}", caller, reason);
            return false;
        };
        if (root.is_discarded()) {
            return fail("not valid JSON");
        }
        if (!root.is_object() || !root.contains("header")) {
            return fail("missing \"header\" field");
        }
        const auto& header = root["header"];
        if (!header.is_object() || !header.contains("version")) {
            return fail("missing \"header.version\" field");
        }
        if (!header["version"].is_number() || header["version"].get<double>() != 1.0) {
            return fail("invalid \"header.version\" field");
        }
        if (!root.contains("data")) {
            return fail("missing \"data\" field");
        }
        if (!root["data"].is_array()) {
            return fail("\"data\" field must be an array");
        }
        const auto& data = root["data"];
        for (size_t i = 0; i < data.size(); ++i) {
            const auto& entry = data[i];
            const std::string at = "data[" + std::to_string(i) + "]";
            if (!entry.is_object() || !entry.contains("info")) {
                return fail("missing \"" + at + ".info\" field");
            }
            if (!entry["info"].is_object() || !entry["info"].contains("locale")) {
                return fail("missing \"" + at + ".info.locale\" field");
            }
            if (!entry["info"]["locale"].is_string()) {
                return fail("\"" + at + ".info.locale\" must be a string");
            }
            if (!entry.contains("messages") || !entry["messages"].is_object()) {
                return fail("missing \"" + at + ".messages\" field");
            }
        }

        out.clear();
        for (const auto& entry : data) {
            ParsedEntry parsed;
            parsed.locale = entry["info"]["locale"].get<std::string>();
            for (const auto& [key, value] : entry["messages"].items()) {
                Message message;
                if (value.is_array()) {
                    for (const auto& form : value) {
                        message.push_back(form.is_string() ? std::optional(form.get<std::string>()) : std::nullopt);
                    }
                } else {
                    message.push_back(value.is_string() ? std::optional(value.get<std::string>()) : std::nullopt);
                }
                parsed.messages[key] = std::move(message);
            }
            out.push_back(std::move(parsed));
        }
        return true;
    }

    bool I18n::addData(const std::string& json)
    {
        std::vector<ParsedEntry> entries;
        if (!parse(json, "addData", entries)) {
            return false;
        }
        for (auto& entry : entries) {
            if (!_translations.contains(entry.locale)) {
                _translations[entry.locale] = {};
                _availableLangs.try_emplace(getLang(entry.locale), entry.locale);
            }
            std::vector<std::string> keys;
            auto& target = _translations[entry.locale];
            for (auto& [key, message] : entry.messages) {
                keys.push_back(key);
                target[key] = std::move(message);
            }
            fire("data:add", std::string(entry.locale), keys);
        }
        return true;
    }

    bool I18n::addDataFromFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            spdlog::error("I18n::addDataFromFile: cannot open '{}'", path);
            return false;
        }
        std::stringstream text;
        text << file.rdbuf();
        return addData(text.str());
    }

    bool I18n::removeData(const std::string& json)
    {
        std::vector<ParsedEntry> entries;
        if (!parse(json, "removeData", entries)) {
            return false;
        }
        for (const auto& entry : entries) {
            const auto it = _translations.find(entry.locale);
            if (it == _translations.end()) {
                continue;
            }
            std::vector<std::string> keys;
            for (const auto& [key, message] : entry.messages) {
                keys.push_back(key);
                it->second.erase(key);
            }
            if (it->second.empty()) {
                _translations.erase(it);
                _availableLangs.erase(getLang(entry.locale));
            }
            fire("data:remove", std::string(entry.locale), keys);
        }
        return true;
    }
}
