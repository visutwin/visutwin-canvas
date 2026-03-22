// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <cctype>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace visutwin::canvas
{
    inline std::string encodeURIComponent(const std::string& input)
    {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (const unsigned char c : input) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            } else {
                escaped << '%' << std::uppercase << std::setw(2) << static_cast<int>(c) << std::nouppercase;
            }
        }

        return escaped.str();
    }

    inline std::string decodeURIComponent(const std::string& input)
    {
        std::string decoded;
        decoded.reserve(input.size());

        for (size_t i = 0; i < input.size(); ++i) {
            if (input[i] == '%' && i + 2 < input.size()) {
                const auto hex = input.substr(i + 1, 2);
                const char ch = static_cast<char>(std::stoi(hex, nullptr, 16));
                decoded += ch;
                i += 2;
            } else if (input[i] == '+') {
                decoded += ' ';
            } else {
                decoded += input[i];
            }
        }

        return decoded;
    }

    struct URIOptions
    {
        std::optional<std::string> scheme;
        std::optional<std::string> authority;
        std::optional<std::string> host;
        std::optional<std::string> path;
        std::optional<std::string> hostpath;
        std::optional<std::string> query;
        std::optional<std::string> fragment;
    };

    inline std::string createURI(const URIOptions& options)
    {
        if ((options.authority || options.scheme) && (options.host || options.hostpath)) {
            throw std::invalid_argument("Can't have 'scheme' or 'authority' and 'host' or 'hostpath' option");
        }

        if (options.host && options.hostpath) {
            throw std::invalid_argument("Can't have 'host' and 'hostpath' option");
        }

        if (options.path && options.hostpath) {
            throw std::invalid_argument("Can't have 'path' and 'hostpath' option");
        }

        std::string result;
        if (options.scheme) {
            result += *options.scheme + ":";
        }
        if (options.authority) {
            result += "//" + *options.authority;
        }
        if (options.host) {
            result += *options.host;
        }
        if (options.path) {
            result += *options.path;
        }
        if (options.hostpath) {
            result += *options.hostpath;
        }
        if (options.query) {
            result += "?" + *options.query;
        }
        if (options.fragment) {
            result += "#" + *options.fragment;
        }

        return result;
    }

    class URI
    {
    public:
        explicit URI(const std::string& uri)
        {
            static const std::regex regex(R"(^(([^:/?#]+):)?(//([^/?#]*))?([^?#]*)(\?([^#]*))?(#(.*))?)");
            std::smatch match;
            std::regex_match(uri, match, regex);
            scheme = match[2].str();
            authority = match[4].str();
            path = match[5].str();
            query = match[7].str();
            fragment = match[9].str();
        }

        [[nodiscard]] std::string toString() const
        {
            std::string result;
            if (!scheme.empty()) {
                result += scheme + ":";
            }
            if (!authority.empty()) {
                result += "//" + authority;
            }
            result += path;
            if (!query.empty()) {
                result += "?" + query;
            }
            if (!fragment.empty()) {
                result += "#" + fragment;
            }
            return result;
        }

        [[nodiscard]] std::map<std::string, std::string> getQuery() const
        {
            std::map<std::string, std::string> result;
            if (query.empty()) {
                return result;
            }

            std::stringstream stream(decodeURIComponent(query));
            std::string kv;
            while (std::getline(stream, kv, '&')) {
                const size_t sep = kv.find('=');
                if (sep == std::string::npos) {
                    result[kv] = "";
                } else {
                    result[kv.substr(0, sep)] = kv.substr(sep + 1);
                }
            }

            return result;
        }

        void setQuery(const std::map<std::string, std::string>& params)
        {
            std::string q;
            for (const auto& [key, value] : params) {
                if (!q.empty()) {
                    q += '&';
                }
                q += encodeURIComponent(key) + "=" + encodeURIComponent(value);
            }
            query = q;
        }

        std::string scheme;
        std::string authority;
        std::string path;
        std::string query;
        std::string fragment;
    };
}
