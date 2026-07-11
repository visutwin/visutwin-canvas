// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <cstdint>
#include <string>

namespace visutwin::canvas
{
    // Control state names shared by every layer's state graph (upstream ANIM_STATE_*).
    inline const std::string ANIM_STATE_START = "START";
    inline const std::string ANIM_STATE_END = "END";
    inline const std::string ANIM_STATE_ANY = "ANY";

    inline bool isAnimControlState(const std::string& name)
    {
        return name == ANIM_STATE_START || name == ANIM_STATE_END || name == ANIM_STATE_ANY;
    }

    /** Which active transition sources may interrupt an in-progress transition. */
    enum class AnimInterruption : uint8_t
    {
        NONE = 0,        // ANIM_INTERRUPTION_NONE
        PREV,            // ANIM_INTERRUPTION_PREV
        NEXT,            // ANIM_INTERRUPTION_NEXT
        PREV_NEXT,       // ANIM_INTERRUPTION_PREV_NEXT
        NEXT_PREV        // ANIM_INTERRUPTION_NEXT_PREV
    };

    /** Transition condition comparison operators. */
    enum class AnimPredicate : uint8_t
    {
        GREATER_THAN = 0,
        LESS_THAN,
        GREATER_THAN_EQUAL_TO,
        LESS_THAN_EQUAL_TO,
        EQUAL_TO,
        NOT_EQUAL_TO
    };

    enum class AnimParameterType : uint8_t
    {
        FLOAT = 0,
        INTEGER,
        BOOLEAN,
        TRIGGER
    };

    /**
     * A named value used to control state-graph transitions and blend-tree weights.
     * DEVIATION: all types share one float storage (int/bool/trigger as 0/1) instead
     * of upstream's dynamically-typed value.
     */
    struct AnimParameter
    {
        AnimParameterType type = AnimParameterType::FLOAT;
        float value = 0.0f;
    };

    /** A single condition that must pass for a transition to activate. */
    struct AnimCondition
    {
        std::string parameterName;
        AnimPredicate predicate = AnimPredicate::EQUAL_TO;
        float value = 0.0f;
    };

    /** Blend tree weighting algorithms. */
    enum class AnimBlendType : uint8_t
    {
        BLEND_1D = 0,          // ANIM_BLEND_1D
        BLEND_2D_CARTESIAN,    // ANIM_BLEND_2D_CARTESIAN
        BLEND_2D_DIRECTIONAL,  // ANIM_BLEND_2D_DIRECTIONAL
        BLEND_DIRECT           // ANIM_BLEND_DIRECT
    };
}
