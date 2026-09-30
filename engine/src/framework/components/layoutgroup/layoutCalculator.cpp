// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "layoutCalculator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace visutwin::canvas
{
    namespace
    {
        // Axis 0 is x / width, axis 1 is y / height. Upstream swizzles property names per
        // orientation; here the primary axis `a` and the secondary `b` are indices.

        struct Sizes
        {
            double size[2] = {0.0, 0.0};
            double minSize[2] = {0.0, 0.0};
            double maxSize[2] = {0.0, 0.0};
            double proportion[2] = {0.0, 0.0};
        };

        struct Line
        {
            std::vector<size_t> items;
            Sizes* largest = nullptr;
            double size[2] = {0.0, 0.0};
        };

        enum class FittingAction
        {
            None,
            Stretch,
            Shrink
        };

        double component(const Vector2& v, const int axis) { return axis == 0 ? v.x : v.y; }
        // Padding is left, bottom, right, top: the left or bottom one for an axis.
        double leadingPadding(const Vector4& v, const int axis) { return axis == 0 ? v.getX() : v.getY(); }

        class Calculator
        {
        public:
            Calculator(const std::vector<LayoutItem>& items, const LayoutOptions& options)
                : _items(items), _options(options)
            {
                _a = options.orientation == Orientation::Horizontal ? 0 : 1;
                _b = 1 - _a;
                _available[0] = static_cast<double>(options.containerSize.x) - options.padding.getX() -
                    options.padding.getZ();
                _available[1] = static_cast<double>(options.containerSize.y) - options.padding.getY() -
                    options.padding.getW();
                _spacing[0] = options.spacing.x;
                _spacing[1] = options.spacing.y;
                _fitting[0] = options.widthFitting;
                _fitting[1] = options.heightFitting;
                _sizes.reserve(items.size());
                for (const auto& item : items) {
                    _sizes.push_back(sanitized(item));
                }
            }

            LayoutResult run()
            {
                LayoutResult result;
                result.placements.resize(_items.size());
                if (_items.empty()) {
                    return result;
                }

                splitLines();
                reverseLinesIfRequired();
                calculateSizesOnAxisA();
                calculateSizesOnAxisB();
                calculateBasePositions();
                applyAlignmentAndPadding();

                for (size_t i = 0; i < _items.size(); ++i) {
                    auto& placement = result.placements[i];
                    placement.width = static_cast<float>(_sizes[i].size[0]);
                    placement.height = static_cast<float>(_sizes[i].size[1]);
                    placement.x = static_cast<float>(_positions[i][0]);
                    placement.y = static_cast<float>(_positions[i][1]);
                }

                // upstream createLayoutInfo
                const double xOffset = (_available[0] - _linesSize[0]) * _options.alignment.x + _options.padding.getX();
                const double yOffset = (_available[1] - _linesSize[1]) * _options.alignment.y + _options.padding.getY();
                result.bounds = Vector4(static_cast<float>(xOffset), static_cast<float>(yOffset),
                                        static_cast<float>(_linesSize[0]), static_cast<float>(_linesSize[1]));
                return result;
            }

        private:
            // upstream getElementSizeProperties: min >= 0, max >= min, size within them
            static Sizes sanitized(const LayoutItem& item)
            {
                Sizes s;
                s.minSize[0] = std::max(static_cast<double>(item.minWidth), 0.0);
                s.minSize[1] = std::max(static_cast<double>(item.minHeight), 0.0);
                s.maxSize[0] = std::max(static_cast<double>(item.maxWidth), s.minSize[0]);
                s.maxSize[1] = std::max(static_cast<double>(item.maxHeight), s.minSize[1]);
                s.size[0] = std::min(std::max(static_cast<double>(item.width), s.minSize[0]), s.maxSize[0]);
                s.size[1] = std::min(std::max(static_cast<double>(item.height), s.minSize[1]), s.maxSize[1]);
                s.proportion[0] = item.fitWidthProportion;
                s.proportion[1] = item.fitHeightProportion;
                return s;
            }

            // Lines of item indices, breaking before an item would overrun the group (after it,
            // under Shrink) when wrapping.
            void splitLines()
            {
                _lines.emplace_back();
                if (!_options.wrap) {
                    for (size_t i = 0; i < _items.size(); ++i) {
                        _lines.back().items.push_back(i);
                    }
                    return;
                }

                double runningSize = 0.0;
                const bool allowOverrun = _fitting[_a] == LayoutFitting::Shrink;
                for (size_t i = 0; i < _items.size(); ++i) {
                    if (!_lines.back().items.empty()) {
                        runningSize += _spacing[_a];
                    }
                    const double idealElementSize = _sizes[i].size[_a];
                    runningSize += idealElementSize;

                    if (!allowOverrun && runningSize > _available[_a] && !_lines.back().items.empty()) {
                        runningSize = idealElementSize;
                        _lines.emplace_back();
                    }

                    _lines.back().items.push_back(i);

                    if (allowOverrun && runningSize > _available[_a] && i != _items.size() - 1) {
                        runningSize = 0.0;
                        _lines.emplace_back();
                    }
                }
            }

            void reverseLinesIfRequired()
            {
                const bool horizontal = _options.orientation == Orientation::Horizontal;
                const bool reverseAxisA = (horizontal && _options.reverseX) || (!horizontal && _options.reverseY);
                const bool reverseAxisB = (horizontal && _options.reverseY) || (!horizontal && _options.reverseX);
                if (reverseAxisA) {
                    for (auto& line : _lines) {
                        std::reverse(line.items.begin(), line.items.end());
                    }
                }
                if (reverseAxisB) {
                    std::reverse(_lines.begin(), _lines.end());
                }
            }

            FittingAction determineFittingAction(const LayoutFitting mode, const double currentSize,
                                                 const double availableSize) const
            {
                switch (mode) {
                case LayoutFitting::None:
                    return FittingAction::None;
                case LayoutFitting::Stretch:
                    return currentSize < availableSize ? FittingAction::Stretch : FittingAction::None;
                case LayoutFitting::Shrink:
                    return currentSize >= availableSize ? FittingAction::Shrink : FittingAction::None;
                case LayoutFitting::Both:
                    return currentSize < availableSize ? FittingAction::Stretch : FittingAction::Shrink;
                }
                return FittingAction::None;
            }

            double calculateTotalSpace(const std::vector<Sizes*>& sizes, const int axis) const
            {
                double total = 0.0;
                for (const Sizes* s : sizes) {
                    total += s->size[axis];
                }
                return total + static_cast<double>(sizes.size() - 1) * _spacing[axis];
            }

            // upstream getTraversalOrder: indices sorted (stably, as JS's sort) by a limit
            static std::vector<size_t> traversalOrder(const std::vector<Sizes*>& sizes, double (Sizes::* limit)[2],
                                                      const int axis, const bool descending)
            {
                std::vector<size_t> order(sizes.size());
                std::iota(order.begin(), order.end(), 0);
                std::stable_sort(order.begin(), order.end(), [&](const size_t i, const size_t j) {
                    const double a = (sizes[i]->*limit)[axis];
                    const double b = (sizes[j]->*limit)[axis];
                    return descending ? b < a : a < b;
                });
                return order;
            }

            static std::vector<double> normalizedProportions(const std::vector<Sizes*>& sizes, const int axis)
            {
                double sum = 0.0;
                for (const Sizes* s : sizes) {
                    sum += s->proportion[axis];
                }
                std::vector<double> values;
                values.reserve(sizes.size());
                for (const Sizes* s : sizes) {
                    values.push_back(sum == 0.0 ? 1.0 / static_cast<double>(sizes.size()) : s->proportion[axis] / sum);
                }
                return values;
            }

            static std::vector<double> invertNormalizedValues(const std::vector<double>& values)
            {
                if (values.size() == 1) {
                    return {1.0};
                }
                std::vector<double> inverted;
                inverted.reserve(values.size());
                for (const double v : values) {
                    inverted.push_back((1.0 - v) / static_cast<double>(values.size() - 1));
                }
                return inverted;
            }

            // upstream createSumArray: the running sum from the end of the traversal order
            static std::vector<double> sumArray(const std::vector<double>& values, const std::vector<size_t>& order)
            {
                std::vector<double> sums(values.size(), 0.0);
                const size_t n = values.size();
                sums[order[n - 1]] = values[order[n - 1]];
                for (size_t k = n - 1; k-- > 0;) {
                    sums[order[k]] = sums[order[k + 1]] + values[order[k]];
                }
                return sums;
            }

            static double calculateAdjustment(const size_t index, const double remaining,
                                              const std::vector<double>& proportions, const std::vector<double>& sums)
            {
                const double proportion = proportions[index];
                const double sumOfRemaining = sums[index];
                if (std::abs(proportion) < 1e-5 && std::abs(sumOfRemaining) < 1e-5) {
                    return remaining;
                }
                return remaining * proportion / sumOfRemaining;
            }

            // Grow the sizes to fill the space, smallest maximum first, handing what one
            // cannot take to the rest.
            void stretchSizesToFitContainer(const std::vector<Sizes*>& sizes, const double idealRequiredSpace,
                                            const int axis) const
            {
                const auto order = traversalOrder(sizes, &Sizes::maxSize, axis, false);
                const auto proportions = normalizedProportions(sizes, axis);
                const auto sums = sumArray(proportions, order);

                double remainingUndershoot = _available[axis] - idealRequiredSpace;
                for (const size_t index : order) {
                    const double targetIncrease = calculateAdjustment(index, remainingUndershoot, proportions, sums);
                    const double targetSize = sizes[index]->size[axis] + targetIncrease;
                    const double actualSize = std::min(targetSize, sizes[index]->maxSize[axis]);
                    sizes[index]->size[axis] = actualSize;
                    const double actualIncrease = std::max(targetSize - actualSize, 0.0);
                    remainingUndershoot -= targetIncrease - actualIncrease;
                }
            }

            // Shrink the sizes to fit, largest minimum first, by the INVERSE proportions, so
            // the balance between items matches stretching.
            void shrinkSizesToFitContainer(const std::vector<Sizes*>& sizes, const double idealRequiredSpace,
                                           const int axis) const
            {
                const auto order = traversalOrder(sizes, &Sizes::minSize, axis, true);
                const auto proportions = normalizedProportions(sizes, axis);
                const auto inverse = invertNormalizedValues(proportions);
                const auto sums = sumArray(inverse, order);

                double remainingOvershoot = idealRequiredSpace - _available[axis];
                for (const size_t index : order) {
                    const double targetReduction = calculateAdjustment(index, remainingOvershoot, inverse, sums);
                    const double targetSize = sizes[index]->size[axis] - targetReduction;
                    const double actualSize = std::max(targetSize, sizes[index]->minSize[axis]);
                    sizes[index]->size[axis] = actualSize;
                    const double actualReduction = std::max(actualSize - targetSize, 0.0);
                    remainingOvershoot -= targetReduction - actualReduction;
                }
            }

            void fitSizes(const std::vector<Sizes*>& sizes, const int axis, const double availableSize)
            {
                const double ideal = calculateTotalSpace(sizes, axis);
                switch (determineFittingAction(_fitting[axis], ideal, availableSize)) {
                case FittingAction::Stretch:
                    stretchSizesToFitContainer(sizes, ideal, axis);
                    break;
                case FittingAction::Shrink:
                    shrinkSizesToFitContainer(sizes, ideal, axis);
                    break;
                case FittingAction::None:
                    break;
                }
            }

            void calculateSizesOnAxisA()
            {
                for (auto& line : _lines) {
                    std::vector<Sizes*> sizes;
                    for (const size_t i : line.items) {
                        sizes.push_back(&_sizes[i]);
                    }
                    if (!sizes.empty()) {
                        fitSizes(sizes, _a, _available[_a]);
                    }
                }
            }

            // Line heights follow each line's largest item; the largest items are fitted to the
            // group, and every other item to its line (or to the group when there is one line).
            // The largest sizes ARE those items' sizes, as upstream shares the objects.
            void calculateSizesOnAxisB()
            {
                std::vector<Sizes*> largestSizes;
                for (auto& line : _lines) {
                    for (const size_t i : line.items) {
                        if (!line.largest || _sizes[i].size[_b] > line.largest->size[_b]) {
                            line.largest = &_sizes[i];
                        }
                    }
                    if (line.largest) {
                        largestSizes.push_back(line.largest);
                    }
                }
                if (!largestSizes.empty()) {
                    fitSizes(largestSizes, _b, _available[_b]);
                }

                for (auto& line : _lines) {
                    for (const size_t i : line.items) {
                        Sizes& s = _sizes[i];
                        const double currentSize = s.size[_b];
                        const double availableSize = _lines.size() == 1 ? _available[_b] : line.largest->size[_b];
                        switch (determineFittingAction(_fitting[_b], currentSize, availableSize)) {
                        case FittingAction::Stretch:
                            s.size[_b] = std::min(availableSize, s.maxSize[_b]);
                            break;
                        case FittingAction::Shrink:
                            s.size[_b] = std::max(availableSize, s.minSize[_b]);
                            break;
                        case FittingAction::None:
                            break;
                        }
                    }
                }
            }

            // Items along each line from 0, each placed by its pivot; lines stacked from 0 on
            // the other axis, each as deep as its largest item.
            void calculateBasePositions()
            {
                _positions.assign(_items.size(), {0.0, 0.0});
                double cursor[2] = {0.0, 0.0};
                _linesSize[_a] = -std::numeric_limits<double>::infinity();

                for (auto& line : _lines) {
                    if (line.items.empty()) {
                        continue;
                    }
                    for (const size_t i : line.items) {
                        const Sizes& s = _sizes[i];
                        const Vector2& pivot = _items[i].pivot;
                        const double minExtentA = -s.size[_a] * component(pivot, _a);
                        const double minExtentB = -s.size[_b] * component(pivot, _b);
                        const double maxExtentA = s.size[_a] * (1.0 - component(pivot, _a));

                        cursor[_b] -= minExtentB;
                        cursor[_a] -= minExtentA;
                        _positions[i][_a] = cursor[_a];
                        _positions[i][_b] = cursor[_b];
                        cursor[_b] += minExtentB;
                        cursor[_a] += maxExtentA + _spacing[_a];
                    }

                    line.size[_a] = cursor[_a] - _spacing[_a];
                    line.size[_b] = line.largest->size[_b];
                    _linesSize[_a] = std::max(_linesSize[_a], line.size[_a]);

                    cursor[_a] = 0.0;
                    cursor[_b] += line.size[_b] + _spacing[_b];
                }

                _linesSize[_b] = cursor[_b] - _spacing[_b];
            }

            void applyAlignmentAndPadding()
            {
                const double alignmentA = component(_options.alignment, _a);
                const double alignmentB = component(_options.alignment, _b);
                const double paddingA = leadingPadding(_options.padding, _a);
                const double paddingB = leadingPadding(_options.padding, _b);

                for (const auto& line : _lines) {
                    const double axisAOffset = (_available[_a] - line.size[_a]) * alignmentA + paddingA;
                    const double axisBOffset = (_available[_b] - _linesSize[_b]) * alignmentB + paddingB;
                    for (const size_t i : line.items) {
                        const double withinLineAxisBOffset = (line.size[_b] - _sizes[i].size[_b]) * alignmentB;
                        _positions[i][_a] += axisAOffset;
                        _positions[i][_b] += axisBOffset + withinLineAxisBOffset;
                    }
                }
            }

            const std::vector<LayoutItem>& _items;
            const LayoutOptions& _options;
            int _a = 0;
            int _b = 1;
            double _available[2] = {0.0, 0.0};
            double _spacing[2] = {0.0, 0.0};
            LayoutFitting _fitting[2] = {LayoutFitting::None, LayoutFitting::None};
            std::vector<Sizes> _sizes;
            std::vector<Line> _lines;
            std::vector<std::array<double, 2>> _positions;
            double _linesSize[2] = {0.0, 0.0};
        };
    }

    LayoutResult calculateLayout(const std::vector<LayoutItem>& items, const LayoutOptions& options)
    {
        return Calculator(items, options).run();
    }
}
