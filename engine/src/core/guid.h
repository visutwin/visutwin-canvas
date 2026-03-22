// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>

namespace visutwin::canvas
{
    class GUID
    {
    public:
        static std::string create()
        {
            static thread_local std::mt19937 generator(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, 255);

            std::array<uint8_t, 16> bytes{};
            for (auto& b : bytes) {
                b = static_cast<uint8_t>(dist(generator));
            }

            // RFC4122 version 4
            bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
            bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

            std::ostringstream stream;
            stream << std::hex;
            for (size_t i = 0; i < bytes.size(); ++i) {
                stream.width(2);
                stream.fill('0');
                stream << static_cast<int>(bytes[i]);
                if (i == 3 || i == 5 || i == 7 || i == 9) {
                    stream << '-';
                }
            }

            return stream.str();
        }
    };
}
