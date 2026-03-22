// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "color.h"
#include <stdexcept>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace visutwin::canvas
{
    // Static color constants
    const Color Color::BLACK(0.0f, 0.0f, 0.0f, 1.0f);
    const Color Color::BLUE(0.0f, 0.0f, 1.0f, 1.0f);
    const Color Color::CYAN(0.0f, 1.0f, 1.0f, 1.0f);
    const Color Color::GRAY(0.5f, 0.5f, 0.5f, 1.0f);
    const Color Color::GREEN(0.0f, 1.0f, 0.0f, 1.0f);
    const Color Color::MAGENTA(1.0f, 0.0f, 1.0f, 1.0f);
    const Color Color::RED(1.0f, 0.0f, 0.0f, 1.0f);
    const Color Color::WHITE(1.0f, 1.0f, 1.0f, 1.0f);
    const Color Color::YELLOW(1.0f, 1.0f, 0.0f, 1.0f);

    Color::Color(const std::vector<float>& arr)
    {
        if (arr.size() >= 3)
        {
            r = arr[0];
            g = arr[1];
            b = arr[2];
            a = arr.size() >= 4 ? arr[3] : 1.0f;
        }
        else
        {
            throw std::invalid_argument("Array must have at least 3 elements");
        }
    }

    Color::Color(const float* arr, size_t size)
    {
        if (size >= 3 && arr != nullptr)
        {
            r = arr[0];
            g = arr[1];
            b = arr[2];
            a = size >= 4 ? arr[3] : 1.0f;
        }
        else
        {
            throw std::invalid_argument("Array must have at least 3 elements");
        }
    }

    Color Color::clone() const
    {
        return Color(r, g, b, a);
    }

    Color& Color::copy(const Color& rhs)
    {
        r = rhs.r;
        g = rhs.g;
        b = rhs.b;
        a = rhs.a;
        return *this;
    }

    bool Color::equals(const Color& rhs) const
    {
        return r == rhs.r && g == rhs.g && b == rhs.b && a == rhs.a;
    }

    Color& Color::set(float r, float g, float b, float a)
    {
        this->r = r;
        this->g = g;
        this->b = b;
        this->a = a;
        return *this;
    }

    Color& Color::lerp(const Color& lhs, const Color& rhs, float alpha)
    {
        r = lhs.r + alpha * (rhs.r - lhs.r);
        g = lhs.g + alpha * (rhs.g - lhs.g);
        b = lhs.b + alpha * (rhs.b - lhs.b);
        a = lhs.a + alpha * (rhs.a - lhs.a);
        return *this;
    }

    Color& Color::linear(const Color* src)
    {
        const Color* source = src ? src : this;
        r = std::pow(source->r, 2.2f);
        g = std::pow(source->g, 2.2f);
        b = std::pow(source->b, 2.2f);
        a = source->a;
        return *this;
    }

    Color& Color::gamma(const Color* src)
    {
        const Color* source = src ? src : this;
        r = std::pow(source->r, 1.0f / 2.2f);
        g = std::pow(source->g, 1.0f / 2.2f);
        b = std::pow(source->b, 1.0f / 2.2f);
        a = source->a;
        return *this;
    }

    Color& Color::mulScalar(float scalar)
    {
        r *= scalar;
        g *= scalar;
        b *= scalar;
        return *this;
    }

    Color& Color::fromString(const std::string& hex)
    {
        if (hex.empty() || hex[0] != '#')
        {
            throw std::invalid_argument("Invalid hex color format");
        }

        std::string hexStr = hex.substr(1); // Remove '#'
        uint32_t i = std::stoul(hexStr, nullptr, 16);

        std::array<uint8_t, 4> bytes;
        if (hexStr.length() > 6)
        {
            auto bytes32 = intToBytes32(i);
            bytes[0] = bytes32[0];
            bytes[1] = bytes32[1];
            bytes[2] = bytes32[2];
            bytes[3] = bytes32[3];
        }
        else
        {
            auto bytes24 = intToBytes24(i);
            bytes[0] = bytes24[0];
            bytes[1] = bytes24[1];
            bytes[2] = bytes24[2];
            bytes[3] = 255;
        }

        set(bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f, bytes[3] / 255.0f);
        return *this;
    }

    Color& Color::fromArray(const std::vector<float>& arr, size_t offset)
    {
        if (arr.size() > offset) r = arr[offset];
        if (arr.size() > offset + 1) g = arr[offset + 1];
        if (arr.size() > offset + 2) b = arr[offset + 2];
        if (arr.size() > offset + 3) a = arr[offset + 3];
        return *this;
    }

    Color& Color::fromArray(const float* arr, size_t size, size_t offset)
    {
        if (arr && size > offset) r = arr[offset];
        if (arr && size > offset + 1) g = arr[offset + 1];
        if (arr && size > offset + 2) b = arr[offset + 2];
        if (arr && size > offset + 3) a = arr[offset + 3];
        return *this;
    }

    std::string Color::toString(bool alpha, bool asArray) const
    {
        // If any component exceeds 1 (HDR), return the color as an array
        if (asArray || r > 1.0f || g > 1.0f || b > 1.0f)
        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(3);
            oss << r << ", " << g << ", " << b << ", " << a;
            return oss.str();
        }

        auto roundToByte = [](float value) -> uint8_t
        {
            return static_cast<uint8_t>(std::round(clamp(value, 0.0f, 1.0f) * 255.0f));
        };

        uint8_t rByte = roundToByte(r);
        uint8_t gByte = roundToByte(g);
        uint8_t bByte = roundToByte(b);

        std::ostringstream oss;
        oss << "#" << std::hex << std::setfill('0');
        oss << std::setw(2) << static_cast<int>(rByte);
        oss << std::setw(2) << static_cast<int>(gByte);
        oss << std::setw(2) << static_cast<int>(bByte);

        if (alpha)
        {
            uint8_t aByte = roundToByte(a);
            oss << std::setw(2) << static_cast<int>(aByte);
        }

        return oss.str();
    }

    std::vector<float> Color::toArray(std::vector<float> arr, const size_t offset, const bool alpha) const
    {
        // Ensure the array is large enough
        if (const size_t requiredSize = offset + (alpha ? 4 : 3); arr.size() < requiredSize)
        {
            arr.resize(requiredSize);
        }

        arr[offset] = r;
        arr[offset + 1] = g;
        arr[offset + 2] = b;
        if (alpha)
        {
            arr[offset + 3] = a;
        }
        return arr;
    }

    void Color::toArray(float* arr, size_t size, size_t offset, bool alpha) const
    {
        if (!arr)
        {
            return;
        }

        if (const size_t requiredSize = offset + (alpha ? 4 : 3); size < requiredSize)
        {
            return;
        }

        arr[offset] = r;
        arr[offset + 1] = g;
        arr[offset + 2] = b;
        if (alpha)
        {
            arr[offset + 3] = a;
        }
    }

    bool Color::operator==(const Color& other) const
    {
        return equals(other);
    }

    bool Color::operator!=(const Color& other) const
    {
        return !equals(other);
    }

    std::array<uint8_t, 3> Color::intToBytes24(uint32_t i)
    {
        uint8_t r = (i >> 16) & 0xFF;
        uint8_t g = (i >> 8) & 0xFF;
        uint8_t b = i & 0xFF;
        return {r, g, b};
    }

    std::array<uint8_t, 4> Color::intToBytes32(uint32_t i)
    {
        const uint8_t r = (i >> 24) & 0xFF;
        uint8_t g = (i >> 16) & 0xFF;
        uint8_t b = (i >> 8) & 0xFF;
        uint8_t a = i & 0xFF;
        return {r, g, b, a};
    }

    float Color::clamp(float value, float min, float max)
    {
        if (value >= max) return max;
        if (value <= min) return min;
        return value;
    }
}
