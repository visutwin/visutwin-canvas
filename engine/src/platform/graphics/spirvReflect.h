// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2026
//
// The little a pipeline needs to know about a SPIR-V module without a reflection library:
// which input locations a stage declares. A Vulkan pipeline declares a vertex attribute only
// at a location its vertex stage reads; an attribute the stage leaves unread is a validation
// warning and costs a fetch on some drivers. And the workgroup size a compute module
// declares, which a dispatch sized by thread count divides by.
//
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace visutwin::canvas
{
    /// A bit per Location (0-63) that an Input variable of the module is decorated with;
    /// built-ins (no Location) are not counted. Empty when the words are not a SPIR-V
    /// module or an instruction runs past the end.
    [[nodiscard]] inline std::optional<uint64_t> spirvInputLocations(const uint32_t* words, const size_t count)
    {
        // The header is five words; each instruction's first word holds its word count (high
        // half) and opcode (low half). OpDecorate is 71 (target, decoration, literals),
        // Location is decoration 30; OpVariable is 59 (type, result, storage class), Input is
        // storage class 1. Decorations precede the variables they name.
        constexpr uint32_t kOpDecorate = 71;
        constexpr uint32_t kOpVariable = 59;
        constexpr uint32_t kDecorationLocation = 30;
        constexpr uint32_t kStorageClassInput = 1;
        if (!words || count <= 5 || words[0] != 0x07230203u) {
            return std::nullopt;
        }
        std::unordered_map<uint32_t, uint32_t> locations;
        uint64_t mask = 0;
        for (size_t i = 5; i < count;) {
            const uint32_t wordCount = words[i] >> 16;
            const uint32_t opcode = words[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > count) {
                return std::nullopt;
            }
            if (opcode == kOpDecorate && wordCount >= 4 && words[i + 2] == kDecorationLocation) {
                locations[words[i + 1]] = words[i + 3];
            } else if (opcode == kOpVariable && wordCount >= 4 && words[i + 3] == kStorageClassInput) {
                if (const auto it = locations.find(words[i + 2]); it != locations.end() && it->second < 64) {
                    mask |= uint64_t{1} << it->second;
                }
            }
            i += wordCount;
        }
        return mask;
    }

    /// The workgroup size of the module's compute entry point: its LocalSize execution mode.
    /// All zero when the words are not a SPIR-V module or declare none (a LocalSizeId, sized
    /// by specialization constants, is not resolved).
    [[nodiscard]] inline std::array<uint32_t, 3> spirvComputeLocalSize(const uint32_t* words, const size_t count)
    {
        // OpExecutionMode is 16 (entry point, mode, literals); LocalSize is mode 17 with the
        // x, y and z as its three literals.
        constexpr uint32_t kOpExecutionMode = 16;
        constexpr uint32_t kExecutionModeLocalSize = 17;
        if (!words || count <= 5 || words[0] != 0x07230203u) {
            return {};
        }
        for (size_t i = 5; i < count;) {
            const uint32_t wordCount = words[i] >> 16;
            const uint32_t opcode = words[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > count) {
                return {};
            }
            if (opcode == kOpExecutionMode && wordCount >= 6 && words[i + 2] == kExecutionModeLocalSize) {
                return {words[i + 3], words[i + 4], words[i + 5]};
            }
            i += wordCount;
        }
        return {};
    }
}
