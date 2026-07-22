// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2025.
//
#pragma once

#include <cstdint>

#include "core/stringIds.h"

namespace visutwin::canvas
{
    enum class StencilCompareFunction : uint8_t
    {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always
    };

    enum class StencilOperation : uint8_t
    {
        Keep,
        Zero,
        Replace,
        IncrementClamp,
        DecrementClamp,
        Invert,
        IncrementWrap,
        DecrementWrap
    };

    /**
     * Holds stencil test settings
     */
    class StencilParameters
    {
    public:
        [[nodiscard]] StencilCompareFunction compareFunction() const { return _func; }
        [[nodiscard]] uint32_t reference() const { return _ref; }
        [[nodiscard]] StencilOperation failOperation() const { return _fail; }
        [[nodiscard]] StencilOperation depthFailOperation() const { return _zfail; }
        [[nodiscard]] StencilOperation passOperation() const { return _zpass; }
        [[nodiscard]] uint32_t readMask() const { return _readMask; }
        [[nodiscard]] uint32_t writeMask() const { return _writeMask; }

        void setCompareFunction(const StencilCompareFunction value) { _func = value; markStateDirty(); }
        void setReference(const uint32_t value) { _ref = value; _dirty = true; }
        void setFailOperation(const StencilOperation value) { _fail = value; markStateDirty(); }
        void setDepthFailOperation(const StencilOperation value) { _zfail = value; markStateDirty(); }
        void setPassOperation(const StencilOperation value) { _zpass = value; markStateDirty(); }
        void setReadMask(const uint32_t value) { _readMask = value; markStateDirty(); }
        void setWriteMask(const uint32_t value) { _writeMask = value; markStateDirty(); }

        /**
         * Gets a unique key representing this stencil parameters configuration.
         * Used for fast equality comparison.
         */
        uint32_t key();

        /// Key for immutable GPU state. The dynamic reference value is excluded.
        uint32_t stateKey();

    private:
        // Evaluates the key based on current parameters
        void evalKey();
        void evalStateKey();
        void markStateDirty() { _dirty = true; _stateDirty = true; }

        StencilCompareFunction _func = StencilCompareFunction::Always;
        uint32_t _ref = 0;
        StencilOperation _fail = StencilOperation::Keep;
        StencilOperation _zfail = StencilOperation::Keep;
        StencilOperation _zpass = StencilOperation::Keep;
        uint32_t _readMask = 0xff;
        uint32_t _writeMask = 0xff;

        uint32_t _key = 0;
        bool _dirty = true;
        uint32_t _stateKey = 0;
        bool _stateDirty = true;

        // Shared string ID cache for generating unique keys
        static StringIds stringIds;
        static StringIds stateStringIds;
    };
}
