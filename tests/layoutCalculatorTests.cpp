// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// The layout group's calculator (framework/components/layoutgroup/layoutCalculator.h),
// ported case for case from upstream's test/framework/components/layout-group/
// layout-calculator.test.mjs. That test builds elements and reads their calculated sizes and
// local positions back; the calculator here is a pure function, so the harness plays the
// element's part: an item left out of the layout (a layout child's `excludeFromLayout`)
// keeps its own size and its position at the origin, as an untouched element does.

#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "framework/components/layoutgroup/layoutCalculator.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    constexpr float kInf = std::numeric_limits<float>::infinity();

    struct Child
    {
        float minWidth = 0.0f;
        float minHeight = 0.0f;
        float maxWidth = kInf;
        float maxHeight = kInf;
        float fitWidthProportion = 0.0f;
        float fitHeightProportion = 0.0f;
        bool excludeFromLayout = false;
    };

    struct Spec
    {
        float width = 0.0f;
        float height = 0.0f;
        Vector2 pivot = Vector2(0.0f, 0.0f);
        std::optional<Child> layoutChild;
    };

    struct Outcome
    {
        std::vector<LayoutPlacement> elements;
        Vector4 bounds;
    };

    Outcome calculate(const std::vector<Spec>& specs, const LayoutOptions& options)
    {
        std::vector<LayoutItem> items;
        std::vector<size_t> included;
        for (size_t i = 0; i < specs.size(); ++i) {
            const Spec& spec = specs[i];
            if (spec.layoutChild && spec.layoutChild->excludeFromLayout) {
                continue;
            }
            LayoutItem item;
            item.width = spec.width;
            item.height = spec.height;
            item.pivot = spec.pivot;
            if (spec.layoutChild) {
                item.minWidth = spec.layoutChild->minWidth;
                item.minHeight = spec.layoutChild->minHeight;
                item.maxWidth = spec.layoutChild->maxWidth;
                item.maxHeight = spec.layoutChild->maxHeight;
                item.fitWidthProportion = spec.layoutChild->fitWidthProportion;
                item.fitHeightProportion = spec.layoutChild->fitHeightProportion;
            }
            items.push_back(item);
            included.push_back(i);
        }

        const LayoutResult result = calculateLayout(items, options);
        Outcome outcome;
        for (const Spec& spec : specs) {
            outcome.elements.push_back({spec.width, spec.height, 0.0f, 0.0f});
        }
        for (size_t k = 0; k < included.size(); ++k) {
            outcome.elements[included[k]] = result.placements[k];
        }
        outcome.bounds = result.bounds;
        return outcome;
    }

    enum class Property { X, Y, Width, Height };

    void assertValues(const std::string& test, const Outcome& outcome, const Property property,
                      const std::vector<float>& expected)
    {
        static const char* names[] = {"x", "y", "calculatedWidth", "calculatedHeight"};
        bool ok = outcome.elements.size() == expected.size();
        std::string got;
        for (size_t i = 0; i < outcome.elements.size(); ++i) {
            const auto& e = outcome.elements[i];
            const float value = property == Property::X ? e.x : property == Property::Y ? e.y
                : property == Property::Width ? e.width : e.height;
            got += (i ? ", " : "") + std::to_string(value);
            if (i >= expected.size() || std::abs(value - expected[i]) > 0.001f) {
                ok = false;
            }
        }
        check(ok, test + ": " + names[static_cast<int>(property)] + (ok ? "" : " = [" + got + "]"));
    }

    LayoutOptions defaultOptions()
    {
        LayoutOptions options;
        options.orientation = Orientation::Horizontal;
        options.reverseX = false;
        options.reverseY = false;
        options.alignment = Vector2(0.0f, 0.0f);
        options.padding = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        options.spacing = Vector2(0.0f, 0.0f);
        options.widthFitting = LayoutFitting::None;
        options.heightFitting = LayoutFitting::None;
        options.wrap = false;
        options.containerSize = Vector2(500.0f, 400.0f);
        return options;
    }

    std::vector<Spec> mixedWidthElements()
    {
        return {{100, 100}, {50, 100}, {100, 100}, {20, 100}, {30, 100}};
    }

    std::vector<Spec> mixedHeightElements()
    {
        return {{100, 100}, {100, 50}, {100, 100}, {100, 20}, {100, 30}};
    }

    std::vector<Spec> mixedWidthElementsWithLayoutChildComponents()
    {
        const auto child = [](const float minWidth, const float maxWidth, const float proportion) {
            Child c;
            c.minWidth = minWidth;
            c.maxWidth = maxWidth;
            c.fitWidthProportion = proportion;
            return c;
        };
        return {{100, 100, Vector2(0, 0), child(50, 200, 0.2f)}, {50, 100, Vector2(0, 0), child(25, 100, 0.4f)},
                {100, 100, Vector2(0, 0), child(50, 200, 0.1f)}, {20, 100, Vector2(0, 0), child(10, 40, 0.1f)},
                {30, 100, Vector2(0, 0), child(15, 60, 0.2f)}};
    }
}

int main()
{
    std::cout << "layout calculator (upstream layout-calculator.test.mjs)\n";

    {
        const std::string t = "lays children out horizontally when orientation is HORIZONTAL";
        auto options = defaultOptions();
        const auto o = calculate(mixedWidthElements(), options);
        assertValues(t, o, Property::X, {0, 100, 150, 250, 270});
        assertValues(t, o, Property::Y, {0, 0, 0, 0, 0});
    }
    {
        const std::string t = "lays children out vertically when orientation is VERTICAL";
        auto options = defaultOptions();
        options.orientation = Orientation::Vertical;
        const auto o = calculate(mixedHeightElements(), options);
        assertValues(t, o, Property::X, {0, 0, 0, 0, 0});
        assertValues(t, o, Property::Y, {0, 100, 150, 250, 270});
    }
    {
        const std::string t = "takes into account each element's pivot (horizontal)";
        auto specs = mixedWidthElements();
        specs[0].pivot = Vector2(0.5f, 0.1f);
        specs[1].pivot = Vector2(0.2f, 0.1f);
        const auto o = calculate(specs, defaultOptions());
        assertValues(t, o, Property::X, {50, 110, 150, 250, 270});
        assertValues(t, o, Property::Y, {10, 10, 0, 0, 0});
    }
    {
        const std::string t = "takes into account each element's pivot (vertical)";
        auto specs = mixedHeightElements();
        specs[0].pivot = Vector2(0.1f, 0.5f);
        specs[1].pivot = Vector2(0.1f, 0.2f);
        auto options = defaultOptions();
        options.orientation = Orientation::Vertical;
        const auto o = calculate(specs, options);
        assertValues(t, o, Property::X, {10, 10, 0, 0, 0});
        assertValues(t, o, Property::Y, {50, 110, 150, 250, 270});
    }
    {
        const std::string t = "returns the layout bounds";
        const auto specs = mixedWidthElementsWithLayoutChildComponents();
        auto options = defaultOptions();
        options.wrap = true;
        const auto bounds = [&](const Vector4& expected, const std::string& what) {
            const Vector4 b = calculate(specs, options).bounds;
            check(b.getX() == expected.getX() && b.getY() == expected.getY() && b.getZ() == expected.getZ() &&
                      b.getW() == expected.getW(),
                  t + " " + what);
        };
        options.alignment = Vector2(0.0f, 0.0f);
        bounds(Vector4(0, 0, 300, 100), "at [0, 0]");
        options.alignment = Vector2(1.0f, 0.5f);
        bounds(Vector4(200, 150, 300, 100), "at [1, 0.5]");
        options.alignment = Vector2(0.5f, 1.0f);
        bounds(Vector4(100, 300, 300, 100), "at [0.5, 1]");
        options.widthFitting = LayoutFitting::Stretch;
        bounds(Vector4(0, 300, 500, 100), "stretched");
    }
    {
        const std::string t = "{wrap: false} NONE keeps sizes and positions";
        const auto o = calculate(mixedWidthElements(), defaultOptions());
        assertValues(t, o, Property::X, {0, 100, 150, 250, 270});
        assertValues(t, o, Property::Y, {0, 0, 0, 0, 0});
        assertValues(t, o, Property::Width, {100, 50, 100, 20, 30});
        assertValues(t, o, Property::Height, {100, 100, 100, 100, 100});
    }
    {
        const std::string t = "{wrap: false} STRETCH uses natural widths when larger than the container";
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Stretch;
        options.containerSize.x = 250;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 100, 150, 250, 270});
        assertValues(t, o, Property::Width, {100, 50, 100, 20, 30});
        assertValues(t, o, Property::Height, {100, 100, 100, 100, 100});
    }
    {
        const std::string t = "{wrap: false} STRETCH stretches proportionally";
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Stretch;
        options.containerSize.x = 400;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 120, 210, 320, 350});
        assertValues(t, o, Property::Y, {0, 0, 0, 0, 0});
        assertValues(t, o, Property::Width, {120, 90, 110, 30, 50});
        assertValues(t, o, Property::Height, {100, 100, 100, 100, 100});
    }
    {
        const std::string t = "{wrap: false} STRETCH respects maxWidth";
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Stretch;
        options.containerSize.x = 1000;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 200, 300, 500, 540});
        assertValues(t, o, Property::Width, {200, 100, 200, 40, 60});
    }
    {
        const std::string t = "{wrap: false} STRETCH redistributes what a tiny maxWidth cannot take";
        auto specs = mixedWidthElementsWithLayoutChildComponents();
        specs[0].layoutChild->maxWidth = 300;
        specs[1].layoutChild->maxWidth = 300;
        specs[2].layoutChild->minWidth = 0;
        specs[2].layoutChild->maxWidth = 1;
        specs[3].layoutChild->maxWidth = 300;
        specs[4].layoutChild->maxWidth = 300;
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Stretch;
        options.containerSize.x = 1000;
        const auto o = calculate(specs, options);
        assertValues(t, o, Property::X, {0, 277.556f, 577.556f, 578.556f, 722.370f});
        assertValues(t, o, Property::Width, {277.555f, 300, 1, 143.815f, 277.630f});
        assertValues(t, o, Property::Height, {100, 100, 100, 100, 100});
    }
    {
        const std::string t = "{wrap: false} STRETCH includes spacing and padding";
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Stretch;
        options.containerSize.x = 600;
        options.padding = Vector4(20, 0, 40, 0);
        options.spacing.x = 10;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {20, 196.667f, 306.667f, 450, 500});
        assertValues(t, o, Property::Width, {166.667f, 100, 133.333f, 40, 60});
    }
    {
        const std::string t = "{wrap: false} SHRINK uses natural widths when smaller than the container";
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Shrink;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 100, 150, 250, 270});
        assertValues(t, o, Property::Width, {100, 50, 100, 20, 30});
    }
    {
        const std::string t = "{wrap: false} SHRINK shrinks proportionally";
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Shrink;
        options.containerSize.x = 290;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 98, 146.5f, 244.25f, 262});
        assertValues(t, o, Property::Width, {98, 48.5f, 97.75f, 17.75f, 28});
        assertValues(t, o, Property::Height, {100, 100, 100, 100, 100});
    }
    {
        const std::string t = "{wrap: false} SHRINK respects minWidth";
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Shrink;
        options.containerSize.x = 100;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 60, 85, 140, 150});
        assertValues(t, o, Property::Width, {60, 25, 55, 10, 15});
    }
    {
        const std::string t = "{wrap: false} SHRINK redistributes what a large minWidth cannot give";
        auto specs = mixedWidthElementsWithLayoutChildComponents();
        specs[0].layoutChild->minWidth = 1;
        specs[1].layoutChild->minWidth = 1;
        specs[2].layoutChild->minWidth = 60;
        specs[3].layoutChild->minWidth = 1;
        specs[4].layoutChild->minWidth = 1;
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Shrink;
        options.containerSize.x = 100;
        const auto o = calculate(specs, options);
        assertValues(t, o, Property::X, {0, 58.71f, 77.742f, 137.742f, 138.742f});
        assertValues(t, o, Property::Width, {58.71f, 19.032f, 60, 1, 1});
    }
    {
        const std::string t = "{wrap: false} SHRINK includes spacing and padding";
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Shrink;
        options.containerSize.x = 300;
        options.padding = Vector4(20, 0, 40, 0);
        options.spacing.x = 10;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {20, 110, 155, 242.5f, 262.5f});
        assertValues(t, o, Property::Width, {80, 35, 77.5f, 10, 15});
    }
    {
        const std::string t = "{wrap: false} BOTH stretches when smaller";
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Both;
        options.containerSize.x = 400;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 120, 210, 320, 350});
        assertValues(t, o, Property::Width, {120, 90, 110, 30, 50});
    }
    {
        const std::string t = "{wrap: false} BOTH shrinks when larger";
        auto options = defaultOptions();
        options.widthFitting = LayoutFitting::Both;
        options.containerSize.x = 290;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 98, 146.5f, 244.25f, 262});
        assertValues(t, o, Property::Width, {98, 48.5f, 97.75f, 17.75f, 28});
    }
    {
        const std::string t = "{wrap: false} reverses on x";
        auto options = defaultOptions();
        options.reverseX = true;
        options.containerSize.x = 260;
        const auto o = calculate(mixedWidthElements(), options);
        assertValues(t, o, Property::X, {200, 150, 50, 30, 0});
        assertValues(t, o, Property::Y, {0, 0, 0, 0, 0});
    }
    {
        const std::string t = "{wrap: false} reverses on y";
        auto options = defaultOptions();
        options.reverseY = true;
        options.orientation = Orientation::Vertical;
        options.containerSize.x = 260;
        const auto o = calculate(mixedHeightElements(), options);
        assertValues(t, o, Property::X, {0, 0, 0, 0, 0});
        assertValues(t, o, Property::Y, {200, 150, 50, 30, 0});
    }
    {
        const std::string t = "{wrap: false} aligns to [1, 0.5]";
        auto options = defaultOptions();
        options.containerSize.x = 260;
        options.alignment = Vector2(1.0f, 0.5f);
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {-40, 60, 110, 210, 230});
        assertValues(t, o, Property::Y, {150, 150, 150, 150, 150});
    }
    {
        const std::string t = "{wrap: false} aligns to [0.5, 1]";
        auto options = defaultOptions();
        options.containerSize.x = 260;
        options.alignment = Vector2(0.5f, 1.0f);
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {-20, 80, 130, 230, 250});
        assertValues(t, o, Property::Y, {300, 300, 300, 300, 300});
    }
    {
        const std::string t = "{wrap: false} excludes elements from the layout";
        auto specs = mixedWidthElementsWithLayoutChildComponents();
        specs[1].layoutChild->excludeFromLayout = true;
        const auto o = calculate(specs, defaultOptions());
        assertValues(t, o, Property::X, {0, 0, 100, 200, 220});
        assertValues(t, o, Property::Y, {0, 0, 0, 0, 0});
    }
    {
        const std::string t = "{wrap: true} NONE keeps sizes, breaks lines";
        auto options = defaultOptions();
        options.wrap = true;
        options.containerSize.x = 260;
        const auto o = calculate(mixedWidthElements(), options);
        assertValues(t, o, Property::X, {0, 100, 150, 0, 20});
        assertValues(t, o, Property::Y, {0, 0, 0, 100, 100});
        assertValues(t, o, Property::Width, {100, 50, 100, 20, 30});
        assertValues(t, o, Property::Height, {100, 100, 100, 100, 100});
    }
    {
        const std::string t = "{wrap: true} NONE places lines by their largest element";
        auto specs = mixedWidthElements();
        specs[2].height = 200;
        auto options = defaultOptions();
        options.wrap = true;
        options.containerSize.x = 260;
        const auto o = calculate(specs, options);
        assertValues(t, o, Property::X, {0, 100, 150, 0, 20});
        assertValues(t, o, Property::Y, {0, 0, 0, 200, 200});
        assertValues(t, o, Property::Height, {100, 100, 200, 100, 100});
    }
    {
        const std::string t = "{wrap: true} NONE includes spacing and padding";
        auto options = defaultOptions();
        options.wrap = true;
        options.padding = Vector4(20, 0, 40, 0);
        options.spacing = Vector2(10, 15);
        options.containerSize.x = 260;
        const auto o = calculate(mixedWidthElements(), options);
        assertValues(t, o, Property::X, {20, 130, 20, 130, 160});
        assertValues(t, o, Property::Y, {0, 0, 115, 115, 115});
    }
    {
        const std::string t = "{wrap: true} NONE includes spacing in line breaks";
        auto specs = mixedWidthElements();
        for (auto& spec : specs) {
            spec.width = 100;
        }
        auto options = defaultOptions();
        options.wrap = true;
        options.spacing.x = 20;
        options.containerSize.x = 500;
        const auto o = calculate(specs, options);
        assertValues(t, o, Property::X, {0, 120, 240, 360, 0});
        assertValues(t, o, Property::Y, {0, 0, 0, 0, 100});
    }
    {
        const std::string t = "{wrap: true} STRETCH stretches each line";
        auto options = defaultOptions();
        options.wrap = true;
        options.widthFitting = LayoutFitting::Stretch;
        options.containerSize.x = 265;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 104.286f, 162.857f, 0, 40});
        assertValues(t, o, Property::Y, {0, 0, 0, 100, 100});
        assertValues(t, o, Property::Width, {104.286f, 58.571f, 102.143f, 40, 60});
    }
    {
        const std::string t = "{wrap: true} SHRINK breaks after the overrun and shrinks";
        auto options = defaultOptions();
        options.wrap = true;
        options.widthFitting = LayoutFitting::Shrink;
        options.containerSize.x = 265;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 98.75f, 147.917f, 246.458f, 0});
        assertValues(t, o, Property::Y, {0, 0, 0, 0, 100});
        assertValues(t, o, Property::Width, {98.75f, 49.167f, 98.542f, 18.542f, 30});
    }
    {
        const std::string t = "{wrap: true} BOTH stretches each line";
        auto options = defaultOptions();
        options.wrap = true;
        options.widthFitting = LayoutFitting::Both;
        options.containerSize.x = 265;
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {0, 104.286f, 162.857f, 0, 40});
        assertValues(t, o, Property::Width, {104.286f, 58.571f, 102.143f, 40, 60});
    }
    {
        const std::string t = "{wrap: true} reverses on x";
        auto options = defaultOptions();
        options.wrap = true;
        options.reverseX = true;
        options.containerSize.x = 260;
        const auto o = calculate(mixedWidthElements(), options);
        assertValues(t, o, Property::X, {150, 100, 0, 30, 0});
        assertValues(t, o, Property::Y, {0, 0, 0, 100, 100});
    }
    {
        const std::string t = "{wrap: true} reverses on y";
        auto options = defaultOptions();
        options.wrap = true;
        options.reverseY = true;
        options.containerSize.x = 260;
        const auto o = calculate(mixedWidthElements(), options);
        assertValues(t, o, Property::X, {0, 100, 150, 0, 20});
        assertValues(t, o, Property::Y, {100, 100, 100, 0, 0});
    }
    {
        const std::string t = "{wrap: true} reverses on both axes";
        auto options = defaultOptions();
        options.wrap = true;
        options.reverseX = true;
        options.reverseY = true;
        options.containerSize.x = 260;
        const auto o = calculate(mixedWidthElements(), options);
        assertValues(t, o, Property::X, {150, 100, 0, 30, 0});
        assertValues(t, o, Property::Y, {100, 100, 100, 0, 0});
    }
    {
        const std::string t = "{wrap: true} aligns to [1, 0.5]";
        auto options = defaultOptions();
        options.wrap = true;
        options.containerSize = Vector2(260, 400);
        options.alignment = Vector2(1.0f, 0.5f);
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {10, 110, 160, 210, 230});
        assertValues(t, o, Property::Y, {100, 100, 100, 200, 200});
    }
    {
        const std::string t = "{wrap: true} aligns to [0.5, 1]";
        auto options = defaultOptions();
        options.wrap = true;
        options.containerSize = Vector2(260, 400);
        options.alignment = Vector2(0.5f, 1.0f);
        const auto o = calculate(mixedWidthElementsWithLayoutChildComponents(), options);
        assertValues(t, o, Property::X, {5, 105, 155, 105, 125});
        assertValues(t, o, Property::Y, {200, 200, 200, 300, 300});
    }
    {
        const std::string t = "{wrap: true} excludes elements from the layout";
        auto specs = mixedWidthElementsWithLayoutChildComponents();
        specs[1].layoutChild->excludeFromLayout = true;
        auto options = defaultOptions();
        options.wrap = true;
        options.containerSize.x = 260;
        const auto o = calculate(specs, options);
        assertValues(t, o, Property::X, {0, 0, 100, 200, 220});
        assertValues(t, o, Property::Y, {0, 0, 0, 0, 0});
    }

    if (failures) {
        std::cout << failures << " layout calculator check(s) FAILED\n";
        return 1;
    }
    std::cout << "All layout calculator tests passed\n";
    return 0;
}
