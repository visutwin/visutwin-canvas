// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <string>
#include <vector>
#include <array>

namespace visutwin::canvas
{
    /**
     * An RGBA color.
     * Each color component is a floating point value in the range 0 to 1.
     * The r (red), g (green) and b (blue) components define a color in the RGB color space.
     * The (alpha) component defines transparency.
     * An alpha of 1 is fully opaque. An alpha of 0 is fully transparent.
     */
    struct Color {
        float r;

        float g;

        float b;

        float a;

        explicit Color(const float r = 0.0f, const float g = 0.0f, const float b = 0.0f, const float a = 1.0f) : r(r), g(g), b(b), a(a) {}

        /**
         * Creates a new Color instance from an array.
         * @param arr The array to set the color values from (must have at least 3 elements).
         */
        explicit Color(const std::vector<float>& arr);

        /**
         * Creates a new Color instance from a C-style array.
         * @param arr The array to set the color values from.
         * @param size Size of the array (3 or 4).
         */
        Color(const float* arr, size_t size);

        /**
         * Copy constructor
         */
        Color(const Color& other) = default;

        Color& operator=(const Color& other) = default;

        /**
         * Returns a clone of the specified color.
         * @returns A duplicate color object.
         */
        [[nodiscard]] Color clone() const;

        /**
         * Copies the contents of a source color to a destination color.
         * @param rhs A color to copy to the specified color.
         * @returns Self for chaining.
         */
        Color& copy(const Color& rhs);

        /**
         * Reports whether two colors are equal.
         * @param rhs The color to compare to the specified color.
         * @returns True if the colors are equal and false otherwise.
         */
        [[nodiscard]] bool equals(const Color& rhs) const;

        /**
         * Assign values to the color components, including alpha.
         * @param r The value for red (0-1).
         * @param g The value for green (0-1).
         * @param b The value for blue (0-1).
         * @param a The value for the alpha (0-1), defaults to 1.
         * @returns Self for chaining.
         */
        Color& set(float r, float g, float b, float a = 1.0f);

        /**
         * Returns the result of a linear interpolation between two specified colors.
         * @param lhs The color to interpolate from.
         * @param rhs The color to interpolate to.
         * @param alpha The value controlling the point of interpolation. Between 0 and 1,
         * the linear interpolant will occur on a straight line between lhs and rhs. Outside of this
         * range, the linear interpolant will occur on a ray extrapolated from this line.
         * @returns Self for chaining.
         */
        Color& lerp(const Color& lhs, const Color& rhs, float alpha);

        /**
         * Converts the color from gamma to linear color space.
         * @param src The color to convert to linear color space. If not set, the operation
         * is done in place.
         * @returns Self for chaining.
         */
        Color& linear(const Color* src = nullptr);

        /**
         * Converts the color from linear to gamma color space.
         * @param src The color to convert to gamma color space. If not set, the operation is
         * done in place.
         * @returns Self for chaining.
         */
        Color& gamma(const Color* src = nullptr);

        /**
         * Multiplies RGB elements of a Color by a number. Note that the alpha value is left unchanged.
         * @param scalar The number to multiply by.
         * @returns Self for chaining.
         */
        Color& mulScalar(float scalar);

        /**
         * Set the values of the color from a string representation '#11223344' or '#112233'.
         * @param hex A string representation in the format '#RRGGBBAA' or '#RRGGBB'. Where
         * RR, GG, BB, AA are red, green, blue and alpha values. This is the same format used in
         * HTML/CSS.
         * @returns Self for chaining.
         */
        Color& fromString(const std::string& hex);

        /**
         * Set the values of the color from an array.
         * @param arr The array to set the color values from.
         * @param offset The zero-based index at which to start copying elements from the
         * array. Default is 0.
         * @returns Self for chaining.
         */
        Color& fromArray(const std::vector<float>& arr, size_t offset = 0);

        /**
         * Set the values of the color from a C-style array.
         * @param arr The array to set the color values from.
         * @param size Size of the array.
         * @param offset The zero-based index at which to start copying elements from the
         * array. Default is 0.
         * @returns Self for chaining.
         */
        Color& fromArray(const float* arr, size_t size, size_t offset = 0);

        /**
         * Converts the color to a string form. The format is '#RRGGBBAA', where RR, GG, BB, AA are the
         * red, green, blue and alpha values. When the alpha value is not included (the default), this
         * is the same format as used in HTML/CSS.
         * @param alpha If true, the output string will include the alpha value.
         * @param asArray If true, the output will be an array of numbers. Defaults to false.
         * @returns The color in string form.
         */
        [[nodiscard]] std::string toString(bool alpha = false, bool asArray = false) const;

        /**
         * Converts the color to an array.
         * @param arr The array to populate with the color's number components. If empty, a new array is created.
         * @param offset The zero-based index at which to start copying elements to the
         * array. Default is 0.
         * @param alpha If true, the output array will include the alpha value.
         * @returns The color as an array.
         */
        [[nodiscard]] std::vector<float> toArray(std::vector<float> arr = {}, size_t offset = 0, bool alpha = true) const;

        /**
         * Copies the color to a C-style array.
         * @param arr The array to populate with the color's number components.
         * @param size Size of the target array.
         * @param offset The zero-based index at which to start copying elements to the
         * array. Default is 0.
         * @param alpha If true, the output array will include the alpha value.
         */
        void toArray(float* arr, size_t size, size_t offset = 0, bool alpha = true) const;

        bool operator==(const Color& other) const;
        bool operator!=(const Color& other) const;

        static const Color BLACK;
        static const Color BLUE;
        static const Color CYAN;
        static const Color GRAY;
        static const Color GREEN;
        static const Color MAGENTA;
        static const Color RED;
        static const Color WHITE;
        static const Color YELLOW;

    private:
        /**
         * Convert a 24-bit integer into an array of 3 bytes.
         * @param i Number holding an integer value.
         * @returns An array of 3 bytes.
         */
        static std::array<uint8_t, 3> intToBytes24(uint32_t i);

        /**
         * Convert a 32-bit integer into an array of 4 bytes.
         * @param i Number holding an integer value.
         * @returns An array of 4 bytes.
         */
        static std::array<uint8_t, 4> intToBytes32(uint32_t i);

        /**
         * Clamp a number between min and max inclusive.
         * @param value Number to clamp.
         * @param min Min value.
         * @param max Max value.
         * @returns The clamped value.
         */
        static float clamp(float value, float min, float max);
    };
}