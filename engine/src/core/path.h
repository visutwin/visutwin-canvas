// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <initializer_list>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace visutwin::canvas
{
    class Path
    {
    public:
        static constexpr char delimiter = '/';

        static std::string join(std::initializer_list<std::string> sections)
        {
            return join(std::vector<std::string>(sections));
        }

        template<typename... Args>
        static std::string join(const std::string& first, const Args&... rest)
        {
            std::vector<std::string> sections;
            sections.reserve(1 + sizeof...(rest));
            sections.push_back(first);
            (sections.push_back(rest), ...);
            return join(sections);
        }

        static std::string join(const std::vector<std::string>& sections)
        {
            if (sections.empty()) {
                return {};
            }

            std::string result = sections.front();
            for (size_t i = 0; i < sections.size() - 1; ++i) {
                const std::string& one = sections[i];
                const std::string& two = sections[i + 1];

                if (!two.empty() && two[0] == delimiter) {
                    result = two;
                    continue;
                }

                if (!one.empty() && !two.empty() && one[one.length() - 1] != delimiter && two[0] != delimiter) {
                    result += delimiter;
                    result += two;
                } else {
                    result += two;
                }
            }

            return result;
        }

        static std::string normalize(const std::string& pathname)
        {
            const bool lead = !pathname.empty() && pathname.front() == delimiter;
            const bool trail = !pathname.empty() && pathname.back() == delimiter;

            std::vector<std::string> parts;
            std::string token;
            for (char ch : pathname) {
                if (ch == delimiter) {
                    if (!token.empty()) {
                        parts.push_back(token);
                        token.clear();
                    }
                } else {
                    token += ch;
                }
            }
            if (!token.empty()) {
                parts.push_back(token);
            }

            std::vector<std::string> cleaned;
            for (const std::string& part : parts) {
                if (part.empty() || part == ".") {
                    continue;
                }
                if (part == ".." && !cleaned.empty())
                {
                    cleaned.pop_back();
                    continue;
                }
                cleaned.push_back(part);
            }

            std::string result;
            if (lead) {
                result += delimiter;
            }

            for (size_t i = 0; i < cleaned.size(); ++i) {
                if (i > 0) {
                    result += delimiter;
                }
                result += cleaned[i];
            }

            if (trail && (result.empty() || result.back() != delimiter)) {
                result += delimiter;
            }

            if (!lead && !result.empty() && result.front() == delimiter) {
                result.erase(result.begin());
            }

            return result;
        }

        static std::pair<std::string, std::string> split(const std::string& pathname)
        {
            const size_t lastDelimiter = pathname.find_last_of(delimiter);
            if (lastDelimiter == std::string::npos) {
                return {"", pathname};
            }

            return {pathname.substr(0, lastDelimiter), pathname.substr(lastDelimiter + 1)};
        }

        static std::string getBasename(const std::string& pathname)
        {
            return split(pathname).second;
        }

        static std::string getDirectory(const std::string& pathname)
        {
            return split(pathname).first;
        }

        static std::string getExtension(const std::string& pathname)
        {
            const std::string pathNoQuery = pathname.substr(0, pathname.find('?'));
            const size_t dot = pathNoQuery.find_last_of('.');
            if (dot == std::string::npos) {
                return {};
            }
            return pathNoQuery.substr(dot);
        }

        static bool isRelativePath(const std::string& pathname)
        {
            if (pathname.empty()) {
                return true;
            }

            return pathname.front() != delimiter && !std::regex_search(pathname, std::regex(R"(:\/\/)"));
        }

        static std::string extractPath(const std::string& pathname)
        {
            std::vector<std::string> parts;
            std::string token;
            for (char ch : pathname) {
                if (ch == delimiter) {
                    parts.push_back(token);
                    token.clear();
                } else {
                    token += ch;
                }
            }
            parts.push_back(token);

            if (parts.size() <= 1) {
                return {};
            }

            std::string result;
            if (isRelativePath(pathname) && parts[0] != "." && parts[0] != "..") {
                result = ".";
            }

            for (size_t i = 0; i + 1 < parts.size(); ++i) {
                if (i > 0 || !result.empty()) {
                    result += '/';
                }
                result += parts[i];
            }

            return result;
        }
    };
}
