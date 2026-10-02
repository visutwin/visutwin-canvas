// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.09.2026
//
// Text markup: `[name]...[/name]` and
// `[name="value" attr="value"]...[/name]` tags inside a text element's string. Evaluation
// strips the tags and returns, for every visible symbol, the tags open over it, merged in
// the order they were opened so an inner tag overrides an outer one of the same name. A
// backslash escapes an opening bracket (`\[`); any other backslash is kept.
//
// A syntax error or a tag left unclosed is not fatal: evaluation reports it and returns
// the text AS WRITTEN with no tags, so the line still draws.
//
// Symbols are bytes, as the text layout's are.
//
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace visutwin::canvas
{
    struct MarkupTag
    {
        /// `[name="value"]`; null for a bare `[name]`.
        std::optional<std::string> value;
        std::map<std::string, std::string> attributes;
    };

    /// The tags over one symbol, by name.
    using MarkupTags = std::map<std::string, MarkupTag>;

    struct MarkupResult
    {
        /// The visible text, tags removed (the input unchanged on an error).
        std::string symbols;
        /// One entry per symbol: its tags, or null where none is open. EMPTY when the text
        /// has no tags, or when evaluation failed.
        std::vector<std::optional<MarkupTags>> tags;
        /// Why evaluation failed, empty when it did not.
        std::string error;
    };

    MarkupResult evaluateMarkup(const std::string& text);
}
