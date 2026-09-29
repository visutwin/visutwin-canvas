// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "markup.h"

#include <cctype>
#include <cstddef>

namespace visutwin::canvas
{
    namespace
    {
        enum class Token
        {
            Eof,
            Error,
            Text,
            OpenBracket,
            CloseBracket,
            Equals,
            String,
            Identifier,
            Whitespace
        };

        bool isWhitespace(const char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
        }

        /// Upstream's IDENTIFIER_REGEX, /[\w|/]/.
        bool isIdentifierSymbol(const char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '|' || c == '/';
        }

        /// Upstream's Scanner: TEXT mode outside brackets, TAG mode inside.
        class Scanner
        {
        public:
            explicit Scanner(const std::string& symbols) : _symbols(symbols) {}

            Token read()
            {
                Token token = readOne();
                while (token == Token::Whitespace) {
                    token = readOne();
                }
                if (token != Token::Eof && token != Token::Error) {
                    _last = _index;
                }
                return token;
            }

            const std::string& buf() const { return _buf; }
            size_t last() const { return _last; }
            const std::string& error() const { return _error; }

        private:
            bool eof() const { return _index >= _symbols.size(); }
            char cur() const { return _symbols[_index]; }
            void next()
            {
                if (!eof()) {
                    ++_index;
                }
            }
            void store()
            {
                _buf.push_back(cur());
                next();
            }

            Token readOne()
            {
                _buf.clear();
                if (eof()) {
                    return Token::Eof;
                }
                return _textMode ? text() : tag();
            }

            Token text()
            {
                while (true) {
                    if (eof()) {
                        return _buf.empty() ? Token::Eof : Token::Text;
                    }
                    switch (cur()) {
                    case '[':
                        _textMode = false;
                        return _buf.empty() ? tag() : Token::Text;
                    case '\\':
                        next();   // skip the backslash
                        if (!eof() && cur() == '[') {
                            store();   // an escaped bracket is text
                        } else {
                            _buf.push_back('\\');   // any other backslash is kept
                        }
                        break;
                    default:
                        store();
                        break;
                    }
                }
            }

            Token tag()
            {
                if (eof()) {
                    _error = "unexpected end of input reading tag";
                    return Token::Error;
                }
                const char c = cur();
                if (c == '[') {
                    store();
                    return Token::OpenBracket;
                }
                if (c == ']') {
                    store();
                    _textMode = true;
                    return Token::CloseBracket;
                }
                if (c == '=') {
                    store();
                    return Token::Equals;
                }
                if (isWhitespace(c)) {
                    while (!eof() && isWhitespace(cur())) {
                        store();
                    }
                    return Token::Whitespace;
                }
                if (c == '"') {
                    next();   // skip the opening quote
                    while (true) {
                        if (eof()) {
                            _error = "unexpected end of input reading string";
                            return Token::Error;
                        }
                        if (cur() == '"') {
                            next();
                            return Token::String;
                        }
                        store();
                    }
                }
                if (!isIdentifierSymbol(c)) {
                    _error = "unrecognized character";
                    return Token::Error;
                }
                while (!eof() && isIdentifierSymbol(cur())) {
                    store();
                }
                return Token::Identifier;
            }

            const std::string& _symbols;
            size_t _index = 0;
            size_t _last = 0;
            std::string _buf;
            std::string _error;
            bool _textMode = true;
        };

        struct ParsedTag
        {
            std::string name;
            MarkupTag tag;
            size_t start = 0;
            std::optional<size_t> end;
        };

        /// Upstream's Parser.
        class Parser
        {
        public:
            explicit Parser(const std::string& symbols) : _scanner(symbols) {}

            bool parse(std::string& symbols, std::vector<ParsedTag>& tags)
            {
                while (true) {
                    switch (_scanner.read()) {
                    case Token::Eof:
                        return true;
                    case Token::Error:
                        return false;
                    case Token::Text:
                        symbols += _scanner.buf();
                        break;
                    case Token::OpenBracket:
                        if (!parseTag(symbols, tags)) {
                            return false;
                        }
                        break;
                    default:
                        return false;
                    }
                }
            }

            std::string error() const
            {
                return "Error evaluating markup at #" + std::to_string(_scanner.last()) + " (" +
                    (!_scanner.error().empty() ? _scanner.error() : _error) + ")";
            }

        private:
            bool parseTag(const std::string& symbols, std::vector<ParsedTag>& tags)
            {
                Token token = _scanner.read();
                if (token != Token::Identifier) {
                    _error = "expected identifier";
                    return false;
                }
                const std::string name = _scanner.buf();

                // A closing tag closes the most recent open tag of that name.
                if (name[0] == '/') {
                    for (auto it = tags.rbegin(); it != tags.rend(); ++it) {
                        if (name.substr(1) == it->name && !it->end) {
                            it->end = symbols.size();
                            if (_scanner.read() != Token::CloseBracket) {
                                _error = "expected close bracket";
                                return false;
                            }
                            return true;
                        }
                    }
                    _error = "failed to find matching tag";
                    return false;
                }

                ParsedTag tag;
                tag.name = name;
                tag.start = symbols.size();
                token = _scanner.read();
                if (token == Token::Equals) {
                    if (_scanner.read() != Token::String) {
                        _error = "expected string";
                        return false;
                    }
                    tag.tag.value = _scanner.buf();
                    token = _scanner.read();
                }
                while (true) {
                    if (token == Token::CloseBracket) {
                        tags.push_back(std::move(tag));
                        return true;
                    }
                    if (token != Token::Identifier) {
                        _error = "expected close bracket or identifier";
                        return false;
                    }
                    const std::string identifier = _scanner.buf();
                    if (_scanner.read() != Token::Equals) {
                        _error = "expected equals";
                        return false;
                    }
                    if (_scanner.read() != Token::String) {
                        _error = "expected string";
                        return false;
                    }
                    tag.tag.attributes[identifier] = _scanner.buf();
                    token = _scanner.read();
                }
            }

            Scanner _scanner;
            std::string _error;
        };
    }

    MarkupResult evaluateMarkup(const std::string& text)
    {
        MarkupResult result;
        std::string symbols;
        std::vector<ParsedTag> tags;
        Parser parser(text);
        if (!parser.parse(symbols, tags)) {
            result.symbols = text;
            result.error = parser.error();
            return result;
        }
        for (const auto& tag : tags) {
            if (!tag.end) {
                result.symbols = text;
                result.error = "Markup error: found unclosed tag='" + tag.name + "'";
                return result;
            }
        }

        result.symbols = std::move(symbols);
        if (tags.empty()) {
            return result;
        }
        // Upstream resolveMarkupTags + combineTags: every tag open over a symbol, in the
        // order the tags were opened, merged so an inner tag replaces an outer one's VALUE
        // (a bare inner tag replaces it with null) and adds to or replaces its attributes.
        result.tags.resize(result.symbols.size());
        for (size_t i = 0; i < result.symbols.size(); ++i) {
            std::optional<MarkupTags> merged;
            for (const auto& tag : tags) {
                if (tag.start <= i && i < *tag.end) {
                    if (!merged) {
                        merged.emplace();
                    }
                    MarkupTag& target = (*merged)[tag.name];
                    target.value = tag.tag.value;
                    for (const auto& [key, value] : tag.tag.attributes) {
                        target.attributes[key] = value;
                    }
                }
            }
            result.tags[i] = std::move(merged);
        }
        return result;
    }
}
