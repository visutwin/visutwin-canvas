// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025
//
// Localization: the current locale, localized messages and their plural forms.
//
// DEVIATIONS from upstream: localization ASSETS are not ported (no `assets` list; data is added
// with addData from JSON text or a file), and a text element does not swap its font asset per
// locale. A message given as a plain string is its own only plural form, where upstream's
// getPluralText indexes into the string and returns one character.
//
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/eventHandler.h"

namespace visutwin::canvas
{
    class Engine;

    /**
     * Handles localization: the current locale, the messages for each locale, and plural forms.
     *
     * Events: "change" (std::string locale, std::string oldLocale) when the locale
     * changes; "data:add" and "data:remove" (std::string locale, std::vector<std::string> keys)
     * when messages are added or removed.
     */
    class I18n : public EventHandler
    {
    public:
        static constexpr const char* DEFAULT_LOCALE = "en-US";

        explicit I18n(const std::shared_ptr<Engine>& engine = nullptr);

        /// The current locale, e.g. "en-US". Setting it fires "change" when it differs; an
        /// "in" language is replaced by "id".
        [[nodiscard]] const std::string& locale() const { return _locale; }
        void setLocale(const std::string& value);

        /// The locale to use for `desiredLocale`: itself when it has messages, else a fallback
        /// (DEFAULT_LOCALE_FALLBACKS by locale then by language, then the first locale added for
        /// the language), else DEFAULT_LOCALE.
        [[nodiscard]] std::string findAvailableLocale(const std::string& desiredLocale) const;

        /// findAvailableLocale over a given set of locales.
        [[nodiscard]] static std::string findAvailableLocale(const std::string& desiredLocale,
            const std::vector<std::string>& availableLocales);

        /// The message of `key` in `locale` (the current one when empty), falling back as
        /// findAvailableLocale does; `key` itself when there is none. Of a plural message, the
        /// first form.
        [[nodiscard]] std::string getText(const std::string& key, const std::string& locale = {}) const;

        /// The plural form of `key` for the number `n` by the locale's CLDR rule; `key` when
        /// there is none.
        [[nodiscard]] std::string getPluralText(const std::string& key, double n, const std::string& locale = {}) const;

        /// Add localization data, in this format:
        /// {"header": {"version": 1}, "data": [{"info": {"locale": "en-US"}, "messages": {...}}]}.
        /// A message is a string, or an array of plural forms (null for a missing form). False,
        /// with the reason logged, when the data is malformed.
        bool addData(const std::string& json);
        bool addDataFromFile(const std::string& path);
        /// Remove the messages the data names; a locale left with none is forgotten.
        bool removeData(const std::string& json);

        /// The language part of a locale ("fr" of "fr-CA").
        [[nodiscard]] static std::string getLang(const std::string& locale);
        /// The locale with its language replaced.
        [[nodiscard]] static std::string replaceLang(const std::string& locale, const std::string& lang);
        /// The plural form index for `n` in `lang` (en's rule for a
        /// language the table does not list).
        [[nodiscard]] static int pluralIndex(const std::string& lang, double n);

    private:
        // A message's forms: one for a string, one per plural form for an array.
        using Message = std::vector<std::optional<std::string>>;
        using Messages = std::map<std::string, Message>;

        struct ParsedEntry
        {
            std::string locale;
            Messages messages;
        };
        static bool parse(const std::string& json, const char* caller, std::vector<ParsedEntry>& out);

        [[nodiscard]] std::string findFallbackLocale(const std::string& locale, const std::string& lang) const;
        [[nodiscard]] const Messages* translations(const std::string& locale) const;

        std::string _locale;
        std::string _lang;
        std::map<std::string, Messages> _translations;
        std::map<std::string, std::string> _availableLangs;   // language -> first locale added
    };
}
