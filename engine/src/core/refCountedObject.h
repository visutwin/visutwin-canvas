// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 04.01.2026
//
#pragma once

namespace visutwin::canvas
{
    class RefCountedObject
    {
    public:
        void incRefCount() { _refCount++; }
        void decRefCount() { _refCount--; }
        [[nodiscard]] int refCount() const { return _refCount; }

    private:
        int _refCount = 0;
    };
}
