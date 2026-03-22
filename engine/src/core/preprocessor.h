// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace visutwin::canvas
{
    struct PreprocessorOptions
    {
        bool stripUnusedColorAttachments = false;
        bool stripDefines = false;
        std::string sourceName;
    };

    class Preprocessor
    {
    public:
        static std::string stripComments(const std::string& source)
        {
            return std::regex_replace(source, std::regex(R"(/\*[\s\S]*?\*/|([^\\:]|^)//.*$)", std::regex_constants::multiline), "$1");
        }

        static std::string removeEmptyLines(const std::string& source)
        {
            std::string output = source;
            output = std::regex_replace(output, std::regex(R"((\n\n){3,})"), "\n\n");
            return output;
        }

        static std::string run(const std::string& source,
                               const std::unordered_map<std::string, std::string>& includes,
                               const bool stripDefines)
        {
            PreprocessorOptions options;
            options.stripDefines = stripDefines;
            return run(source, includes, options);
        }

        static std::string run(const std::string& source,
                               const std::unordered_map<std::string, std::string>& includes = {},
                               const PreprocessorOptions& options = {})
        {
            std::unordered_map<std::string, std::string> defines;
            std::unordered_map<std::string, std::string> injectDefines;

            std::string output = stripComments(source);
            output = trimEndPerLine(output);
            output = preprocess(output, defines, injectDefines, includes, options.stripDefines);
            output = stripComments(output);
            output = stripUnusedColorAttachments(output, options.stripUnusedColorAttachments);
            output = removeEmptyLines(output);
            output = processArraySize(output, defines);
            output = injectDefinesIntoSource(output, injectDefines);
            return output;
        }

    private:
        struct ConditionalFrame
        {
            bool parentKeep = true;
            bool branchTaken = false;
            bool keep = true;
        };

        static std::string trim(const std::string& s)
        {
            size_t start = 0;
            while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
                ++start;
            }
            size_t end = s.size();
            while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
                --end;
            }
            return s.substr(start, end - start);
        }

        static std::string trimEndPerLine(const std::string& source)
        {
            std::stringstream in(source);
            std::string line;
            std::string out;
            bool first = true;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
                    line.pop_back();
                }
                if (!first) {
                    out += '\n';
                }
                first = false;
                out += line;
            }
            return out;
        }

        static bool active(const std::vector<ConditionalFrame>& stack)
        {
            return stack.empty() ? true : stack.back().keep;
        }

        static bool parseDefinedExpr(const std::string& expr, const std::unordered_map<std::string, std::string>& defines)
        {
            const std::string e = trim(expr);
            std::smatch match;
            static const std::regex definedPattern(R"(^(!)?\s*defined\(([^)]+)\)\s*$)");
            if (std::regex_match(e, match, definedPattern)) {
                const bool negated = match[1].matched;
                const std::string id = trim(match[2].str());
                const bool value = defines.contains(id);
                return negated ? !value : value;
            }
            return false;
        }

        static std::optional<bool> parseComparisonExpr(const std::string& expr, const std::unordered_map<std::string, std::string>& defines)
        {
            std::smatch match;
            static const std::regex comparison(R"(^\s*([A-Za-z_]\w*)\s*(==|!=|<=|>=|<|>)\s*([\w"']+)\s*$)");
            if (!std::regex_match(expr, match, comparison)) {
                return std::nullopt;
            }

            const std::string lhsId = match[1].str();
            const std::string op = match[2].str();
            std::string rhs = match[3].str();

            auto it = defines.find(lhsId);
            const std::string lhsRaw = (it != defines.end()) ? it->second : "0";

            auto stripQuotes = [](std::string s) {
                if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') || (s.front() == '"' && s.back() == '"'))) {
                    return s.substr(1, s.size() - 2);
                }
                return s;
            };

            const std::string lhs = stripQuotes(lhsRaw);
            rhs = stripQuotes(rhs);

            auto toDouble = [](const std::string& s) -> std::optional<double> {
                char* end = nullptr;
                const double v = std::strtod(s.c_str(), &end);
                if (end && *end == '\0') {
                    return v;
                }
                return std::nullopt;
            };

            if (const auto ln = toDouble(lhs); ln.has_value()) {
                if (const auto rn = toDouble(rhs); rn.has_value()) {
                    if (op == "==") return *ln == *rn;
                    if (op == "!=") return *ln != *rn;
                    if (op == "<") return *ln < *rn;
                    if (op == "<=") return *ln <= *rn;
                    if (op == ">") return *ln > *rn;
                    if (op == ">=") return *ln >= *rn;
                }
            }

            if (op == "==") return lhs == rhs;
            if (op == "!=") return lhs != rhs;
            if (op == "<") return lhs < rhs;
            if (op == "<=") return lhs <= rhs;
            if (op == ">") return lhs > rhs;
            if (op == ">=") return lhs >= rhs;
            return std::nullopt;
        }

        static bool evalExpr(const std::string& expr, const std::unordered_map<std::string, std::string>& defines)
        {
            const std::string e = trim(expr);
            if (e.empty()) {
                return false;
            }

            const size_t orPos = e.find("||");
            if (orPos != std::string::npos) {
                return evalExpr(e.substr(0, orPos), defines) || evalExpr(e.substr(orPos + 2), defines);
            }

            const size_t andPos = e.find("&&");
            if (andPos != std::string::npos) {
                return evalExpr(e.substr(0, andPos), defines) && evalExpr(e.substr(andPos + 2), defines);
            }

            if (parseDefinedExpr(e, defines)) {
                return true;
            }

            static const std::regex negDefinedPattern(R"(^\s*!\s*defined\(([^)]+)\)\s*$)");
            if (std::regex_match(e, negDefinedPattern)) {
                return parseDefinedExpr(e, defines);
            }

            if (const auto comparison = parseComparisonExpr(e, defines); comparison.has_value()) {
                return *comparison;
            }

            if (defines.contains(e)) {
                const auto& value = defines.at(e);
                return !(value == "0" || value == "false" || value.empty());
            }

            if (e == "true") return true;
            if (e == "false") return false;
            return false;
        }

        static std::string preprocess(const std::string& source,
                                      std::unordered_map<std::string, std::string>& defines,
                                      std::unordered_map<std::string, std::string>& injectDefines,
                                      const std::unordered_map<std::string, std::string>& includes,
                                      const bool stripDefines)
        {
            std::stringstream in(source);
            std::string line;
            std::vector<ConditionalFrame> stack;
            std::vector<std::string> output;

            static const std::regex includePattern(R"(^\s*#include\s+"([\w-]+)(?:\s*,\s*([\w-]+))?"\s*$)");
            static const std::regex definePattern(R"(^\s*#define\s+([^\s]+)\s*(.*)$)");
            static const std::regex undefPattern(R"(^\s*#undef\s+([^\s]+)\s*$)");
            static const std::regex extensionPattern(R"(^\s*#extension\s+([\w-]+)\s*:\s*(enable|require)\s*$)");
            static const std::regex ifdefPattern(R"(^\s*#ifdef\s+(.+)$)");
            static const std::regex ifndefPattern(R"(^\s*#ifndef\s+(.+)$)");
            static const std::regex ifPattern(R"(^\s*#if\s+(.+)$)");
            static const std::regex elifPattern(R"(^\s*#elif\s+(.+)$)");
            static const std::regex elsePattern(R"(^\s*#else\s*$)");
            static const std::regex endifPattern(R"(^\s*#endif\s*$)");

            while (std::getline(in, line)) {
                std::smatch match;
                const bool keep = active(stack);

                if (std::regex_match(line, match, includePattern)) {
                    if (keep) {
                        const auto includeIt = includes.find(match[1].str());
                        if (includeIt != includes.end()) {
                            output.push_back(includeIt->second);
                        }
                        if (match[2].matched) {
                            const auto includeIt2 = includes.find(match[2].str());
                            if (includeIt2 != includes.end()) {
                                output.push_back(includeIt2->second);
                            }
                        }
                    }
                    continue;
                }

                if (std::regex_match(line, match, definePattern)) {
                    if (keep) {
                        const std::string id = trim(match[1].str());
                        std::string value = trim(match[2].str());
                        if (value.empty()) {
                            value = "true";
                        }

                        if (id.size() > 2 && id.front() == '{' && id.back() == '}') {
                            injectDefines[id] = value;
                        } else {
                            defines[id] = value;
                        }
                    }

                    if (!stripDefines && keep) {
                        output.push_back(line);
                    }
                    continue;
                }

                if (std::regex_match(line, match, undefPattern)) {
                    if (keep) {
                        defines.erase(trim(match[1].str()));
                    }
                    if (!stripDefines && keep) {
                        output.push_back(line);
                    }
                    continue;
                }

                if (std::regex_match(line, match, extensionPattern)) {
                    if (keep) {
                        defines[trim(match[1].str())] = "true";
                    }
                    if (!stripDefines && keep) {
                        output.push_back(line);
                    }
                    continue;
                }

                if (std::regex_match(line, match, ifdefPattern)) {
                    const bool parentKeep = keep;
                    const bool cond = parentKeep && defines.contains(trim(match[1].str()));
                    stack.push_back({parentKeep, cond, parentKeep && cond});
                    continue;
                }

                if (std::regex_match(line, match, ifndefPattern)) {
                    const bool parentKeep = keep;
                    const bool cond = parentKeep && !defines.contains(trim(match[1].str()));
                    stack.push_back({parentKeep, cond, parentKeep && cond});
                    continue;
                }

                if (std::regex_match(line, match, ifPattern)) {
                    const bool parentKeep = keep;
                    const bool cond = parentKeep && evalExpr(match[1].str(), defines);
                    stack.push_back({parentKeep, cond, parentKeep && cond});
                    continue;
                }

                if (std::regex_match(line, match, elifPattern)) {
                    if (!stack.empty()) {
                        auto& top = stack.back();
                        const bool cond = top.parentKeep && !top.branchTaken && evalExpr(match[1].str(), defines);
                        top.keep = cond;
                        if (cond) {
                            top.branchTaken = true;
                        }
                    }
                    continue;
                }

                if (std::regex_match(line, elsePattern)) {
                    if (!stack.empty()) {
                        auto& top = stack.back();
                        top.keep = top.parentKeep && !top.branchTaken;
                        top.branchTaken = true;
                    }
                    continue;
                }

                if (std::regex_match(line, endifPattern)) {
                    if (!stack.empty()) {
                        stack.pop_back();
                    }
                    continue;
                }

                if (keep) {
                    output.push_back(line);
                }
            }

            std::string result;
            for (size_t i = 0; i < output.size(); ++i) {
                result += output[i];
                if (i + 1 < output.size()) {
                    result += '\n';
                }
            }
            return result;
        }

        static std::string processArraySize(std::string source,
                                            const std::unordered_map<std::string, std::string>& defines)
        {
            for (const auto& [key, value] : defines) {
                char* end = nullptr;
                std::strtol(value.c_str(), &end, 10);
                if (!end || *end != '\0') {
                    continue;
                }

                source = std::regex_replace(source, std::regex("\\\\[" + key + "\\\\]"), "[" + value + "]");
            }
            return source;
        }

        static std::string injectDefinesIntoSource(const std::string& source,
                                                   const std::unordered_map<std::string, std::string>& injectDefines)
        {
            if (injectDefines.empty()) {
                return source;
            }

            std::stringstream in(source);
            std::string line;
            std::string out;
            bool first = true;

            while (std::getline(in, line)) {
                if (line.find('#') == std::string::npos) {
                    for (const auto& [key, value] : injectDefines) {
                        line = std::regex_replace(line, std::regex(regexEscape(key)), value);
                    }
                }

                if (!first) {
                    out += '\n';
                }
                first = false;
                out += line;
            }

            return out;
        }

        static std::string regexEscape(const std::string& input)
        {
            static const std::regex specials(R"([.^$|()\\[\]{}*+?])");
            return std::regex_replace(input, specials, R"(\$&)");
        }

        static std::string stripUnusedColorAttachments(const std::string& source, const bool enabled)
        {
            if (!enabled) {
                return source;
            }

            static const std::regex fragColorPattern(R"((pcFragColor[1-8])\b)");

            std::unordered_map<int, int> counts;
            for (auto it = std::sregex_iterator(source.begin(), source.end(), fragColorPattern);
                 it != std::sregex_iterator(); ++it) {
                const auto& match = *it;
                const std::string token = match[1].str();
                const int index = token.back() - '0';
                counts[index]++;
            }

            bool anySingleUse = false;
            for (const auto& [_, c] : counts) {
                if (c == 1) {
                    anySingleUse = true;
                    break;
                }
            }

            if (!anySingleUse) {
                return source;
            }

            std::stringstream in(source);
            std::string line;
            std::vector<std::string> keep;

            while (std::getline(in, line)) {
                std::smatch m;
                if (std::regex_search(line, m, fragColorPattern)) {
                    const int index = m[1].str().back() - '0';
                    if (index > 0 && counts[index] == 1) {
                        continue;
                    }
                }
                keep.push_back(line);
            }

            std::string out;
            for (size_t i = 0; i < keep.size(); ++i) {
                out += keep[i];
                if (i + 1 < keep.size()) {
                    out += '\n';
                }
            }
            return out;
        }
    };
}
