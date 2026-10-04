// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// The live instances of one component type, in CREATION order.
//
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "component.h"

namespace visutwin::canvas
{
    /**
     * @brief Creation-ordered list of a component type's live instances.
     * @ingroup group_framework_ecs
     *
     * An engine's ComponentRegistry keeps one per component type, and the renderer, the
     * systems and the examples sweep it. The ORDER is part of the contract: it is the order
     * draws of equal sort key keep, the order lights fill their slots in and the order
     * scripts run in, so a removal must not move the survivors.
     *
     * A removal does not erase: an erase is a scan for the component and a shift of
     * everything after it, so destroying K of N components would cost K x N. Instead a
     * removal finds its slot
     * by binary search (slots are in creation order, so their serials are sorted) and
     * leaves a NULL in it; the holes are closed in one pass, still in order, the next
     * time the list is asked for.
     *
     * Two consequences for callers, both already the convention everywhere:
     *  - a loop over items() checks each entry for null. The list never hands out a
     *    hole, but a component destroyed DURING the loop becomes one under it (never a
     *    shift, so the loop skips no live component);
     *  - items() may compact, so a loop must not call it again for the same type once a
     *    component of that type may have been destroyed inside it.
     *
     * Main thread only, like the components themselves.
     */
    template <class T>
    class ComponentInstanceList
    {
    public:
        /// Adds a component at its creation position (its serial, given at construction).
        /// Appending is the common case; a component listed late, when its entity reached
        /// an engine after newer components did, is inserted where it belongs.
        void add(T* component)
        {
            const std::uint64_t serial = static_cast<const Component*>(component)->_instanceSerial;
            if (_serials.empty() || serial > _serials.back()) {
                _items.push_back(component);
                _serials.push_back(serial);
                return;
            }
            const auto it = std::lower_bound(_serials.begin(), _serials.end(), serial);
            const auto slot = it - _serials.begin();
            _items.insert(_items.begin() + slot, component);
            _serials.insert(it, serial);
        }

        /// Takes a component out without moving the others. O(log n).
        void remove(T* component)
        {
            const std::uint64_t serial = static_cast<const Component*>(component)->_instanceSerial;
            const auto it = std::lower_bound(_serials.begin(), _serials.end(), serial);
            if (it == _serials.end() || *it != serial) {
                return;
            }
            const auto slot = static_cast<std::size_t>(it - _serials.begin());
            if (_items[slot] == component) {
                _items[slot] = nullptr;
                ++_holes;
            }
        }

        /// The live components in creation order, with no holes.
        const std::vector<T*>& items()
        {
            if (_holes != 0) {
                compact();
            }
            return _items;
        }

        /// Visits the live components in creation order WITHOUT compacting, for a caller
        /// that may itself be running inside someone's loop over items() — a destructor,
        /// or a teardown hook.
        template <class Visitor>
        void forEachLive(Visitor&& visit) const
        {
            // By index: the visitor may add a component.
            for (std::size_t i = 0; i < _items.size(); ++i) {
                if (T* component = _items[i]) {
                    visit(component);
                }
            }
        }

        /// Holes waiting for the next items() call (for tests).
        [[nodiscard]] std::size_t pendingHoles() const { return _holes; }

    private:
        void compact()
        {
            std::size_t write = 0;
            for (std::size_t read = 0; read < _items.size(); ++read) {
                if (_items[read]) {
                    _items[write] = _items[read];
                    _serials[write] = _serials[read];
                    ++write;
                }
            }
            _items.resize(write);
            _serials.resize(write);
            _holes = 0;
        }

        // Parallel arrays: the serial of the component in each slot, kept beside the
        // pointers so neither a removal nor a compaction has to touch a component.
        std::vector<T*> _items;
        std::vector<std::uint64_t> _serials;
        std::size_t _holes = 0;
    };
}
