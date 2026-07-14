// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Tightly-packed 4-component POD vectors for GPU uniform/vertex layouts.
//
// Unlike Vector4 (which is alignas(16) and carries a SIMD union + user-provided
// constructors), PackedVec4<T> is a plain aggregate: 16 bytes, natural
// alignment, no padding — byte-identical to a bare `T[4]`. That makes it a
// drop-in for GPU-mirrored structs (LightingUniforms, vertex formats) where the
// layout must match the shader exactly and the whole struct is memcpy'd to the
// device. The `operator[]` overloads keep existing positional call sites
// (`field[0] = ...`) compiling unchanged, while `.x/.y/.z/.w` reads better for
// genuinely-vector fields. Aggregate-ness also preserves the compiler's
// brace-init arity check (`= {a, b, c, d}`).
//
#pragma once

#include <cstdint>
#include <type_traits>

namespace visutwin::canvas
{
    /**
     * @brief Packed 4-component POD vector mirroring a shader `T4` (float4/uint4).
     * @ingroup group_core_math
     */
    template <class T>
    struct PackedVec4
    {
        T x, y, z, w;

        // Positional access — relies on the four same-type members being laid out
        // contiguously with no padding, which is guaranteed for a standard-layout
        // aggregate of identical scalar members.
        constexpr T& operator[](const int i) { return (&x)[i]; }
        constexpr const T& operator[](const int i) const { return (&x)[i]; }
    };

    using PackedVector4f = PackedVec4<float>;
    using PackedVector4u = PackedVec4<uint32_t>;
    using PackedVector4i = PackedVec4<int32_t>;

    static_assert(sizeof(PackedVector4f) == 16 && alignof(PackedVector4f) == 4);
    static_assert(std::is_standard_layout_v<PackedVector4f> && std::is_trivially_copyable_v<PackedVector4f>);
    static_assert(sizeof(PackedVector4u) == 16 && std::is_trivially_copyable_v<PackedVector4u>);
}
