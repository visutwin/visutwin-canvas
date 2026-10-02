// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// Layout groups, the element drag helper, scrollbars and scroll views (upstream
// layout-group, element-drag-helper, scrollbar and scroll-view component tests, the cases
// that apply here, plus the port's own contracts): a real engine on a stub device, driven
// through ElementInput as the application's events would drive it. With no window the canvas
// is the device's 300x150, y down; a screen-space screen's units are canvas points, y up.
//
// The layout group's reflow is the port's DEVIATION from upstream's event wiring: it compares
// each group's inputs after every update. What this file holds for it is what the events
// guaranteed — a change to any input lays the group out again, in the same update, outermost
// first, and nothing else does.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/element/elementDragHelper.h"
#include "framework/components/layoutchild/layoutChildComponent.h"
#include "framework/components/layoutchild/layoutChildComponentSystem.h"
#include "framework/components/layoutgroup/layoutGroupComponent.h"
#include "framework/components/layoutgroup/layoutGroupComponentSystem.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/components/scrollbar/scrollbarComponent.h"
#include "framework/components/scrollbar/scrollbarComponentSystem.h"
#include "framework/components/scrollview/scrollViewComponent.h"
#include "framework/components/scrollview/scrollViewComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/input/elementInput.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/camera.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-3f;

    std::shared_ptr<Engine> engine;
    std::shared_ptr<ElementInput> input;
    Entity* screen = nullptr;

    /// An element under `parent`, anchored and pivoted at its bottom-left unless told
    /// otherwise, at (x, y) in its parent's units.
    ElementComponent* element(Entity* parent, const std::string& name, ElementDesc desc, const float x = 0.0f,
                              const float y = 0.0f)
    {
        Entity* e = newEntity(engine.get(), name);
        parent->addChild(std::unique_ptr<GraphNode>(e));
        if (!desc.type) {
            desc.type = ElementType::Group;
        }
        if (!desc.anchor) {
            desc.anchor = Vector4(0, 0, 0, 0);
        }
        if (!desc.pivot) {
            desc.pivot = Vector2(0, 0);
        }
        auto* el = static_cast<ElementComponent*>(e->addComponent<ElementComponent>());
        el->setup(desc);
        e->setLocalPosition(x, y, 0.0f);
        return el;
    }

    void update() { engine->update(1.0f / 60.0f); }

    /// Canvas y (down) of a screen y (up).
    float canvasY(const float screenY) { return 150.0f - screenY; }

    void clearScreen()
    {
        std::vector<GraphNode*> children;
        for (const auto& child : screen->children()) {
            children.push_back(child.get());
        }
        for (GraphNode* child : children) {
            if (auto* e = dynamic_cast<Entity*>(child)) {
                e->destroy();
            }
            (void)screen->removeChild(child);
        }
        update();
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.size = {300, 150}, .resizable = true});
    input = std::make_shared<ElementInput>();
    engine = makeTestEngine<CameraComponentSystem, RenderComponentSystem, ScreenComponentSystem, ElementComponentSystem,
        LayoutGroupComponentSystem, LayoutChildComponentSystem, ScrollbarComponentSystem, ScrollViewComponentSystem>(
        device, [&](AppOptions& options) { options.elementInput = input; });

    Entity* cameraEntity = newEntity(engine.get(), "camera");
    engine->root()->addChild(std::unique_ptr<GraphNode>(cameraEntity));
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    camera->camera()->setAspectRatio(2.0f);

    screen = newEntity(engine.get(), "screen");
    engine->root()->addChild(std::unique_ptr<GraphNode>(screen));
    static_cast<ScreenComponent*>(screen->addComponent<ScreenComponent>())->setScreenSpace(true);
    update();

    std::cout << "layout group\n";
    {
        // A column 100 wide: rows of 20 stretched to its width, less padding, from the top.
        ElementComponent* list = element(screen, "list", {.width = 100.0f, .height = 120.0f});
        auto* group = static_cast<LayoutGroupComponent*>(list->entity()->addComponent<LayoutGroupComponent>());
        group->setOrientation(Orientation::Vertical);
        group->setAlignment(Vector2(0.0f, 1.0f));
        group->setPadding(Vector4(5, 5, 5, 5));
        group->setSpacing(Vector2(0, 10));
        group->setWidthFitting(LayoutFitting::Stretch);
        std::vector<ElementComponent*> rows;
        for (int i = 0; i < 3; ++i) {
            rows.push_back(element(list->entity(), "row" + std::to_string(i),
                                   {.anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .width = 40.0f, .height = 20.0f}));
        }
        int reflows = 0;
        Vector4 bounds;
        group->on("reflow", [&](const Vector4& b) {
            ++reflows;
            bounds = b;
        });

        update();
        check(reflows == 1, "an update lays a new group out once");
        check(rows[0]->anchor().getX() == 0.0f && rows[0]->anchor().getW() == 0.0f, "children's anchors reset to zero");
        check(near(rows[0]->calculatedWidth(), 90.0f, kTolerance) && near(rows[2]->calculatedWidth(), 90.0f, kTolerance),
              "stretched to the width less the padding");
        check(near(rows[0]->entity()->localPosition().getY(), 95.0f, kTolerance) &&
                  near(rows[1]->entity()->localPosition().getY(), 65.0f, kTolerance) &&
                  near(rows[2]->entity()->localPosition().getY(), 35.0f, kTolerance) &&
                  near(rows[0]->entity()->localPosition().getX(), 5.0f, kTolerance),
              "rows stack down from the top padding (reverseY is the default)");
        check(near(bounds.getZ(), 90.0f, kTolerance) && near(bounds.getW(), 80.0f, kTolerance), "reflow carries the bounds");

        update();
        check(reflows == 1, "an update with nothing changed does not lay out again");

        rows[1]->entity()->setEnabled(false);
        update();
        check(reflows == 2 && near(rows[2]->entity()->localPosition().getY(), 65.0f, kTolerance),
              "a disabled child leaves the layout and the rest close up");
        rows[1]->entity()->setEnabled(true);
        update();
        check(reflows == 3 && near(rows[2]->entity()->localPosition().getY(), 35.0f, kTolerance), "re-enabled, it is back");

        rows[0]->setHeight(40.0f);
        update();
        check(reflows == 4 && near(rows[1]->entity()->localPosition().getY(), 45.0f, kTolerance), "a child's new height reflows");

        auto* child = static_cast<LayoutChildComponent*>(rows[2]->entity()->addComponent<LayoutChildComponent>());
        child->setMaxWidth(50.0f);
        update();
        check(reflows == 5 && near(rows[2]->calculatedWidth(), 50.0f, kTolerance), "a layout child's limit reflows");
        child->setExcludeFromLayout(true);
        update();
        check(reflows == 6, "excluding a child reflows");

        // Moving a child to the end of the list reorders it.
        list->entity()->addChild(rows[0]->entity());
        update();
        check(reflows == 7 && rows[1]->entity()->localPosition().getY() > rows[0]->entity()->localPosition().getY(),
              "reordering the children reflows in the new order");

        // A reflow handler that sizes the group to its content (the scroll view's content
        // does) lays the group out again in the same update, at its new size.
        auto handle = group->on("reflow", [&](const Vector4& b) { list->setHeight(b.getW() + 10.0f); });
        list->setHeight(300.0f);
        update();
        check(near(list->calculatedHeight(), bounds.getW() + 10.0f, kTolerance), "a handler's new size ...");
        check(near(rows[1]->entity()->localPosition().getY() + rows[1]->calculatedHeight(),
                   list->calculatedHeight() - 5.0f, kTolerance),
              "... is laid out in the same update: the first row still meets the top padding");
        handle->off();
        clearScreen();
    }

    std::cout << "nested layout groups\n";
    {
        // An outer row stretches an inner column, which must lay out at the stretched width in
        // the same update: outermost first.
        ElementComponent* outer = element(screen, "outer", {.width = 200.0f, .height = 100.0f});
        auto* outerGroup = static_cast<LayoutGroupComponent*>(outer->entity()->addComponent<LayoutGroupComponent>());
        outerGroup->setWidthFitting(LayoutFitting::Stretch);
        ElementComponent* inner = element(outer->entity(), "inner", {.width = 50.0f, .height = 100.0f});
        auto* innerGroup = static_cast<LayoutGroupComponent*>(inner->entity()->addComponent<LayoutGroupComponent>());
        innerGroup->setOrientation(Orientation::Vertical);
        innerGroup->setWidthFitting(LayoutFitting::Stretch);
        ElementComponent* cell = element(inner->entity(), "cell", {.width = 10.0f, .height = 10.0f});
        update();
        check(near(inner->calculatedWidth(), 200.0f, kTolerance) && near(cell->calculatedWidth(), 200.0f, kTolerance),
              "the inner group lays out at the width the outer one gave it, in one update");

        // A group whose handler changes its own size every time never settles; the system
        // gives up after 100 passes.
        int reflows = 0;
        outerGroup->on("reflow", [&]() { outer->setWidth(outer->width() + 1.0f); ++reflows; });
        outer->setWidth(201.0f);
        update();
        check(reflows == 100, "a layout that never settles stops after 100 passes");
        clearScreen();
    }

    std::cout << "drag helper\n";
    {
        ElementComponent* knob = element(screen, "knob", {.width = 20.0f, .height = 20.0f, .useInput = true}, 50, 50);
        auto helper = std::make_unique<ElementDragHelper>(knob);
        std::vector<std::string> events;
        Vector3 moved;
        helper->on("drag:start", [&]() { events.push_back("start"); });
        helper->on("drag:move", [&](const Vector3& p) { events.push_back("move"); moved = p; });
        helper->on("drag:end", [&]() { events.push_back("end"); });
        update();

        input->onMouseDown(60.0f, canvasY(60.0f), MouseButton::Left);
        input->onMouseMove(90.0f, canvasY(40.0f));
        check(events == std::vector<std::string>{"start", "move"} && helper->isDragging(), "press and move: start, move");
        check(near(moved.getX(), 80.0f, kTolerance) && near(moved.getY(), 30.0f, kTolerance) &&
                  near(knob->entity()->localPosition().getX(), 80.0f, kTolerance),
              "the element follows the pointer by the distance moved");
        input->onMouseMove(200.0f, canvasY(140.0f));
        check(near(knob->entity()->localPosition().getX(), 190.0f, kTolerance), "moves go to the pressed element even off it");
        input->onMouseUp(200.0f, canvasY(140.0f), MouseButton::Left);
        check(events.back() == "end" && !helper->isDragging(), "release: end");
        events.clear();
        input->onMouseMove(10.0f, 10.0f);
        check(events.empty(), "no moves after the end");

        // Constrained to x, in a parent scaled by 2: the pointer's distance halves.
        knob->entity()->setLocalPosition(50.0f, 50.0f, 0.0f);
        helper = std::make_unique<ElementDragHelper>(knob, DragAxis::X);
        helper->on("drag:move", [&](const Vector3& p) { moved = p; });
        screen->setLocalScale(1.0f, 1.0f, 1.0f);
        Entity* holder = element(screen, "holder", {.width = 100.0f, .height = 75.0f})->entity();
        holder->setLocalScale(2.0f, 2.0f, 1.0f);
        holder->addChild(knob->entity());
        knob->entity()->setLocalPosition(10.0f, 10.0f, 0.0f);
        update();
        // The knob's screen box is (20..60, 20..60).
        input->onMouseDown(30.0f, canvasY(30.0f), MouseButton::Left);
        input->onMouseMove(50.0f, canvasY(50.0f));
        check(near(knob->entity()->localPosition().getX(), 20.0f, kTolerance) && near(knob->entity()->localPosition().getY(), 10.0f, kTolerance),
              "constrained to x, and scaled into the parent's units");
        input->onMouseUp(50.0f, canvasY(50.0f), MouseButton::Left);

        helper->setEnabled(false);
        input->onMouseDown(50.0f, canvasY(50.0f), MouseButton::Left);
        check(!helper->isDragging(), "a disabled helper starts no drag");
        input->onMouseUp(50.0f, canvasY(50.0f), MouseButton::Left);

        helper.reset();
        input->onMouseDown(50.0f, canvasY(50.0f), MouseButton::Left);
        input->onMouseMove(80.0f, canvasY(50.0f));
        check(near(knob->entity()->localPosition().getX(), 20.0f, kTolerance), "a destroyed helper moves nothing");
        input->onMouseUp(80.0f, canvasY(50.0f), MouseButton::Left);
        clearScreen();
    }

    std::cout << "scrollbar\n";
    {
        ElementComponent* track = element(screen, "track", {.width = 200.0f, .height = 20.0f}, 50, 50);
        ElementComponent* handle = element(track->entity(), "handle", {.anchor = Vector4(0, 0, 0, 1),
            .margin = Vector4(0, 0, 0, 0), .useInput = true});
        auto* scrollbar = static_cast<ScrollbarComponent*>(track->entity()->addComponent<ScrollbarComponent>());
        check(scrollbar->orientation() == Orientation::Horizontal && scrollbar->value() == 0.0f &&
                  scrollbar->handleSize() == 0.0f && !scrollbar->handleEntity(),
              "defaults: horizontal, value 0, handle size 0, no handle");
        scrollbar->setHandleEntity(handle->entity());
        scrollbar->setHandleSize(0.25f);
        check(near(handle->calculatedWidth(), 50.0f, kTolerance), "the handle is handleSize of the track");

        std::vector<float> values;
        scrollbar->on("set:value", [&](const float v) { values.push_back(v); });
        scrollbar->setValue(1.0f);
        check(near(handle->entity()->localPosition().getX(), 150.0f, kTolerance), "value 1 puts the handle at the far end");
        scrollbar->setValue(2.0f);
        check(scrollbar->value() == 1.0f && values == std::vector<float>{1.0f, 1.0f},
              "values clamp to [0, 1], and set:value fires the clamped value");
        scrollbar->setValue(1.0f + 5e-6f);
        check(values.size() == 2, "a change below 1e-5 fires nothing");
        scrollbar->setHandleSize(-1.0f);
        check(scrollbar->handleSize() == 0.0f, "handle size clamps too");
        scrollbar->setHandleSize(0.25f);

        // Dragging the handle sets the value (the track's usable length is 150).
        scrollbar->setValue(0.0f);
        update();
        input->onMouseDown(60.0f, canvasY(60.0f), MouseButton::Left);
        input->onMouseMove(135.0f, canvasY(70.0f));
        check(near(scrollbar->value(), 0.5f, kTolerance) && near(handle->entity()->localPosition().getY(), 0.0f, kTolerance),
              "dragging the handle 75 along a usable 150 sets 0.5, along x only");
        input->onMouseUp(135.0f, canvasY(70.0f), MouseButton::Left);

        track->setWidth(400.0f);
        check(near(handle->calculatedWidth(), 100.0f, kTolerance) && near(handle->entity()->localPosition().getX(), 150.0f, kTolerance),
              "a resized track refits the handle and moves it");

        // A disabled scrollbar's handle does not drag.
        scrollbar->setEnabled(false);
        // The handle's box is now x 200..300.
        input->onMouseDown(250.0f, canvasY(60.0f), MouseButton::Left);
        input->onMouseMove(290.0f, canvasY(60.0f));
        check(input->pressedElement() == handle && near(scrollbar->value(), 0.5f, kTolerance),
              "a disabled scrollbar ignores a drag on its handle");
        input->onMouseUp(290.0f, canvasY(60.0f), MouseButton::Left);
        scrollbar->setEnabled(true);

        // Vertical: the value runs down from the top, and the handle loses its width.
        scrollbar->setOrientation(Orientation::Vertical);
        check(handle->width() == 0.0f, "a new orientation zeroes the handle's other dimension");
        track->setWidth(20.0f);
        track->setHeight(200.0f);
        handle->setAnchor(Vector4(0, 1, 1, 1));
        handle->setPivot(Vector2(0, 1));
        scrollbar->setValue(1.0f);
        check(near(handle->calculatedHeight(), 50.0f, kTolerance) && near(handle->entity()->localPosition().getY(), -150.0f, kTolerance),
              "vertical: value 1 puts the handle 150 below the top");

        handle->entity()->destroy();
        check(!scrollbar->handleEntity(), "a destroyed handle entity is dropped");
        scrollbar->setValue(0.0f);
        clearScreen();
    }

    std::cout << "scroll view\n";
    {
        // A 200x150 view holding a 100x100 viewport at (10, 10) and a 400-tall content hung
        // from its top, with a vertical scrollbar on the right.
        ElementComponent* view = element(screen, "view", {.width = 200.0f, .height = 150.0f});
        ElementComponent* viewport = element(view->entity(), "viewport", {.width = 100.0f, .height = 100.0f}, 10, 10);
        ElementComponent* content = element(viewport->entity(), "content", {.anchor = Vector4(0, 1, 1, 1),
            .pivot = Vector2(0, 1), .margin = Vector4(0, 0, 0, 0), .useInput = true});
        content->setHeight(400.0f);
        ElementComponent* item = element(content->entity(), "item", {.width = 50.0f, .height = 20.0f,
            .useInput = true}, 60, 350);   // screen x 70..120, clear of the presses below
        ElementComponent* bar = element(view->entity(), "bar", {.width = 10.0f, .height = 100.0f}, 150, 10);
        ElementComponent* barHandle = element(bar->entity(), "handle", {.anchor = Vector4(0, 1, 1, 1),
            .pivot = Vector2(0, 1), .margin = Vector4(0, 0, 0, 0)});
        auto* scrollbar = static_cast<ScrollbarComponent*>(bar->entity()->addComponent<ScrollbarComponent>());
        scrollbar->setOrientation(Orientation::Vertical);
        scrollbar->setHandleEntity(barHandle->entity());

        auto* scrollView = static_cast<ScrollViewComponent*>(view->entity()->addComponent<ScrollViewComponent>());
        scrollView->setHorizontal(false);
        scrollView->setScrollMode(ScrollMode::Clamp);
        scrollView->setViewportEntity(viewport->entity());
        scrollView->setContentEntity(content->entity());
        scrollView->setVerticalScrollbarEntity(bar->entity());
        update();

        std::vector<Vector2> scrolls;
        scrollView->on("set:scroll", [&](const Vector2& s) { scrolls.push_back(s); });
        check(near(content->entity()->localPosition().getY(), 0.0f, kTolerance), "scroll 0: the content's top at the viewport's");
        check(near(scrollbar->handleSize(), 0.25f, kTolerance), "the scrollbar's handle is the visible share of the content");

        scrollView->setScroll(Vector2(0.0f, 0.5f));
        check(near(content->entity()->localPosition().getY(), 150.0f, kTolerance) && scrolls.size() == 1,
              "scroll 0.5 raises the content by half of its 300 overflow, and fires set:scroll");
        check(near(scrollbar->value(), 0.5f, kTolerance), "the scrollbar follows");
        scrollbar->setValue(1.0f);
        check(near(scrollView->scroll().y, 1.0f, kTolerance) && near(content->entity()->localPosition().getY(), 300.0f, kTolerance),
              "the scrollbar drives the view");
        scrollView->setScroll(Vector2(0.0f, 3.0f));
        check(near(scrollView->scroll().y, 1.0f, kTolerance), "Clamp keeps it within [0, 1]");

        // The wheel: a notch toward the user is 100 pixels of the 400 content.
        scrollView->setScroll(Vector2(0.0f, 0.0f));
        input->onMouseWheel(50.0f, canvasY(60.0f), -1.0f);
        check(near(scrollView->scroll().y, 0.25f, kTolerance), "a wheel notch toward the user scrolls a quarter of this content");

        // Dragging the content up past the threshold scrolls it and turns off the item's
        // input until the drag ends.
        scrollView->setScroll(Vector2(0.0f, 0.0f));
        update();
        input->onMouseDown(60.0f, canvasY(60.0f), MouseButton::Left);
        input->onMouseMove(60.0f, canvasY(90.0f));
        check(near(scrollView->scroll().y, 0.1f, kTolerance) && near(content->entity()->localPosition().getX(), 0.0f, kTolerance),
              "dragging up 30 scrolls by 30 of 300, and not sideways (horizontal off)");
        check(!item->useInput(), "a drag past the threshold turns off the content's input");
        input->onMouseUp(60.0f, canvasY(90.0f), MouseButton::Left);
        check(item->useInput(), "the end of the drag turns it back on");

        // Bounce: dragged past the top, it springs back after the release.
        scrollView->setScrollMode(ScrollMode::Bounce);
        scrollView->setScroll(Vector2(0.0f, 0.0f));
        update();
        input->onMouseDown(60.0f, canvasY(60.0f), MouseButton::Left);
        input->onMouseMove(60.0f, canvasY(0.0f));
        const float dragged = scrollView->scroll().y;
        input->onMouseUp(60.0f, canvasY(0.0f), MouseButton::Left);
        check(near(dragged, -std::log10(1.2f), kTolerance), "dragged 0.2 past the top, the tension leaves -log10(1.2)");
        for (int i = 0; i < 200; ++i) {
            update();
        }
        check(std::abs(scrollView->scroll().y) < 0.01f, "released, it springs back to the top");

        // Content that shrinks below the viewport leaves nothing to scroll.
        scrollView->setVerticalScrollbarVisibility(ScrollbarVisibility::ShowWhenRequired);
        content->setHeight(80.0f);
        update();
        check(!bar->entity()->enabledLocal(), "ShowWhenRequired hides the scrollbar when the content fits");
        content->setHeight(400.0f);
        update();
        check(bar->entity()->enabledLocal(), "and shows it when it does not");

        content->entity()->destroy();
        check(!scrollView->contentEntity(), "a destroyed content entity is dropped");
        update();
        clearScreen();
        update();
        check(true, "the view and its entities are torn down in any order");
    }

    return finish("layout and scroll");
}
