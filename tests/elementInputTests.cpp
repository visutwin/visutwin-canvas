// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// UI input and buttons: ElementInput's event delivery (upstream element-input.js — hover,
// press capture, click, bubbling, the hit test through screen corners, world corners and
// hit padding, touches and the touch click brake) and ButtonComponent's visual states, with
// upstream's button component.test.mjs ported.
//
// Until 2026-09-29 ElementInput delivered one event, a "click" on mouse DOWN over the
// front-most screen-space element's bounding box; nothing hovered, released, bubbled or hit
// a world-space element, and a button only held an image entity.
//
// Input is invisible in a screenshot, so this is the test that says it works. A real engine
// on a stub device: with no window the canvas is the device's 300x150, y down.

#include <algorithm>
#include <any>
#include <cmath>
#include <iostream>
#include <typeinfo>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "framework/appOptions.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/input/elementInput.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/input/mouse.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "platform/graphics/blendState.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/sprite.h"

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

    bool near(const float a, const float b, const float eps = 1e-5f) { return std::abs(a - b) <= eps; }

    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&, int,
            const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(const int width, const int height) override { _size = {width, height}; }
        std::pair<int, int> size() const override { return _size; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    private:
        std::pair<int, int> _size{300, 150};
    };

    std::shared_ptr<Engine> engine;
    std::shared_ptr<ElementInput> input;

    Entity* newEntity(const std::string& name = "e")
    {
        auto* e = new Entity();
        e->setEngine(engine.get());
        e->setName(name);
        return e;
    }

    Entity* addTo(GraphNode* parent, Entity* child)
    {
        parent->addChild(std::unique_ptr<GraphNode>(child));
        return child;
    }

    ElementComponent* addElement(Entity* e, const ElementDesc& desc)
    {
        auto* element = static_cast<ElementComponent*>(e->addComponent<ElementComponent>());
        element->setup(desc);
        return element;
    }

    ButtonComponent* addButton(Entity* e)
    {
        return static_cast<ButtonComponent*>(e->addComponent<ButtonComponent>());
    }

    Entity* screenSpaceScreen()
    {
        Entity* screen = addTo(engine->root(), newEntity("screen"));
        static_cast<ScreenComponent*>(screen->addComponent<ScreenComponent>())->setScreenSpace(true);
        return screen;
    }

    /// A box on a screen-space screen, positioned by its bottom-left corner in canvas points
    /// with y UP from the bottom (the screen's own units).
    ElementComponent* box(Entity* parent, const std::string& name, const float x, const float y, const float w,
                          const float h, const bool useInput = true)
    {
        Entity* e = addTo(parent, newEntity(name));
        ElementComponent* element = addElement(e, {.type = ElementType::Group, .anchor = Vector4(0, 0, 0, 0),
            .pivot = Vector2(0, 0), .width = w, .height = h, .useInput = useInput});
        e->setLocalPosition(x, y, 0.0f);
        return element;
    }

    /// Mouse down and up at canvas (x, y), y DOWN.
    void clickAt(const float x, const float y)
    {
        input->onMouseDown(x, y, MouseButton::Left);
        input->onMouseUp(x, y, MouseButton::Left);
    }

    /// Record every event an element (or button) fires, in order.
    struct Recorder
    {
        std::vector<std::string> names;
        std::vector<ElementInputEvent> events;

        void follow(EventHandler* target, const std::vector<std::string>& eventNames)
        {
            for (const auto& name : eventNames) {
                target->on(name, [this, name](const EventArgs& args) {
                    names.push_back(name);
                    if (!args.empty() && args[0].type() == typeid(ElementInputEvent*)) {
                        events.push_back(*std::any_cast<ElementInputEvent*>(args[0]));
                    } else {
                        events.emplace_back();
                    }
                });
            }
        }
        int count(const std::string& name) const
        {
            return static_cast<int>(std::count(names.begin(), names.end(), name));
        }
        void clear()
        {
            names.clear();
            events.clear();
        }
    };

    const std::vector<std::string> kMouseEvents = {"mousedown", "mouseup", "mousemove", "mousewheel", "mouseenter",
                                                   "mouseleave", "click"};
    const std::vector<std::string> kTouchEvents = {"touchstart", "touchmove", "touchend", "touchleave",
                                                   "touchcancel", "click"};

    /// Upstream's createButton: an image button entity holding an image child it tints.
    struct TestButton
    {
        Entity* button = nullptr;
        Entity* image = nullptr;
        ButtonComponent* component = nullptr;
        ElementComponent* imageElement = nullptr;
    };

    TestButton createButton(GraphNode* parent)
    {
        TestButton b;
        b.button = addTo(parent, newEntity("button"));
        b.image = addTo(b.button, newEntity("image"));
        b.imageElement = addElement(b.image, {.type = ElementType::Image});
        addElement(b.button, {.type = ElementType::Image, .useInput = true});
        b.component = addButton(b.button);
        b.component->setImageEntity(b.image);
        return b;
    }
    /// The mesh instances of an element's visual (the render component on its child).
    std::vector<MeshInstance*> visualInstances(Entity* elementEntity)
    {
        for (const auto& child : elementEntity->children()) {
            auto* entity = dynamic_cast<Entity*>(child.get());
            if (auto* render = entity ? entity->findComponent<RenderComponent>() : nullptr) {
                return render->meshInstances();
            }
        }
        return {};
    }

    bool stencilIs(const MeshInstance* mi, const StencilCompareFunction func, const StencilOperation pass,
                   const uint32_t ref)
    {
        const auto& sp = mi ? mi->stencilFront() : nullptr;
        return sp && mi->stencilBack() == sp && sp->compareFunction() == func && sp->passOperation() == pass &&
               sp->reference() == ref;
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubDevice>();
    engine = std::make_shared<Engine>(nullptr);
    input = std::make_shared<ElementInput>();
    AppOptions options;
    options.graphicsDevice = device;
    options.elementInput = input;
    auto mouseDevice = std::make_shared<Mouse>();
    options.mouse = mouseDevice;
    options.registerComponentSystem<CameraComponentSystem>();
    // the element visuals are render components
    options.registerComponentSystem<RenderComponentSystem>();
    options.registerComponentSystem<ScreenComponentSystem>();
    options.registerComponentSystem<ElementComponentSystem>();
    options.registerComponentSystem<ButtonComponentSystem>();
    engine->init(options);

    Entity* cameraEntity = addTo(engine->root(), newEntity("camera"));
    cameraEntity->setLocalPosition(0.0f, 0.0f, 0.0f);   // looking down -Z
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    camera->camera()->setAspectRatio(2.0f);             // the 300x150 canvas

    Entity* screen = screenSpaceScreen();
    engine->update(0.0f);   // the screen takes the canvas size

    std::cout << "mouse on a screen-space element\n";
    {
        // Canvas rows 100..120 from the top are 30..50 up from the bottom.
        ElementComponent* tag = box(screen, "tag", 80.0f, 30.0f, 40.0f, 20.0f);
        Recorder r;
        r.follow(tag, kMouseEvents);

        input->onMouseMove(100.0f, 110.0f);
        check(r.names == std::vector<std::string>{"mousemove", "mouseenter"} && input->hoveredElement() == tag,
            "moving onto it fires mousemove, then mouseenter");
        r.clear();
        clickAt(100.0f, 110.0f);
        check(r.names == std::vector<std::string>{"mousedown", "mouseup", "click"}, "down and up over it: a click");
        check(r.events.size() == 3 && r.events[2].element == tag && r.events[2].camera == camera &&
              r.events[2].x == 100.0f && r.events[2].y == 110.0f && r.events[0].button == MouseButton::Left,
            "the event carries the element, the camera, the canvas point and the button");
        r.clear();
        clickAt(100.0f, 60.0f);
        check(r.names == std::vector<std::string>{"mouseleave"}, "a click above it (y is down) only leaves it");
        r.clear();

        input->onMouseDown(100.0f, 110.0f, MouseButton::Left);
        check(input->pressedElement() == tag, "a mouse down presses it");
        input->onMouseMove(10.0f, 10.0f);
        input->onMouseUp(10.0f, 10.0f, MouseButton::Left);
        check(r.names == std::vector<std::string>{"mousedown", "mouseenter", "mousemove", "mouseleave", "mouseup"},
            "the pressed element takes the move and the release away from it, and no click");
        check(input->pressedElement() == nullptr, "the release lets it go");
        r.clear();

        input->onMouseWheel(100.0f, 110.0f, 1.0f);
        check(r.count("mousewheel") == 1 && r.events[0].wheelDelta == -1 &&
              near(r.events[0].wheel(), 2.0f), "a wheel notch away from the user is wheelDelta -1 (wheel +2)");
        r.clear();
        input->onMouseMove(10.0f, 10.0f);
        r.clear();

        tag->entity()->setEnabled(false);
        clickAt(100.0f, 110.0f);
        check(r.names.empty(), "a disabled element takes nothing");
        tag->entity()->setEnabled(true);
        tag->setUseInput(false);
        clickAt(100.0f, 110.0f);
        check(r.names.empty(), "nor does one without useInput");
        tag->setUseInput(true);

        camera->setLayers({LAYERID_WORLD});
        clickAt(100.0f, 110.0f);
        check(r.names.empty(), "nor one on a layer no camera draws");
        camera->setLayers({});

        camera->camera()->setRect(Vector4(0.5f, 0.0f, 0.5f, 1.0f));
        clickAt(100.0f, 110.0f);
        check(r.names.empty(), "nor a point outside the camera's rectangle");
        clickAt(200.0f, 110.0f);
        check(r.count("click") == 1, "a camera on the right half maps its half of the canvas onto the screen");
        camera->camera()->setRect(Vector4(0.0f, 0.0f, 1.0f, 1.0f));
        input->onMouseMove(1.0f, 1.0f);

        input->setEnabled(false);
        r.clear();
        clickAt(100.0f, 110.0f);
        check(r.names.empty(), "a disabled ElementInput delivers nothing");
        input->setEnabled(true);
        tag->entity()->destroy();
    }

    std::cout << "\nbubbling and the front element\n";
    {
        ElementComponent* panel = box(screen, "panel", 20.0f, 20.0f, 200.0f, 100.0f);
        ElementComponent* inner = box(panel->entity(), "inner", 20.0f, 20.0f, 50.0f, 50.0f);
        // A child without useInput lets the input through to what is under it.
        ElementComponent* label = box(panel->entity(), "label", 120.0f, 20.0f, 50.0f, 50.0f, false);
        engine->update(0.0f);   // the screen assigns draw orders
        Recorder rp;
        Recorder ri;
        rp.follow(panel, {"click"});
        ri.follow(inner, {"click"});

        // inner covers screen x 40..90, y 40..90: canvas rows 60..110.
        clickAt(60.0f, 80.0f);
        check(ri.count("click") == 1 && rp.count("click") == 1, "a click on the child bubbles to the parent");
        check(rp.events.size() == 1 && rp.events[0].element == inner, "with the child as the event's element");
        rp.clear();
        ri.clear();
        auto stop = inner->on("click", [](ElementInputEvent* event) { event->stopPropagation(); });
        clickAt(60.0f, 80.0f);
        check(ri.count("click") == 1 && rp.count("click") == 0, "stopPropagation keeps it from the parent");
        stop->off();
        rp.clear();
        ri.clear();

        clickAt(160.0f, 80.0f);
        check(rp.count("click") == 1 && rp.events[0].element == panel, "the input-less label passes it to the panel");
        check(label->drawOrder() > panel->drawOrder(), "(the label draws over the panel)");

        // Overlapping siblings: the later one draws on top and is hit first.
        ElementComponent* a = box(screen, "a", 230.0f, 20.0f, 40.0f, 40.0f);
        ElementComponent* b = box(screen, "b", 240.0f, 30.0f, 40.0f, 40.0f);
        engine->update(0.0f);
        Recorder ra;
        Recorder rb;
        ra.follow(a, {"click"});
        rb.follow(b, {"click"});
        clickAt(255.0f, 105.0f);
        check(rb.count("click") == 1 && ra.count("click") == 0, "the higher draw order wins an overlap");
        clickAt(235.0f, 125.0f);
        check(ra.count("click") == 1, "and the one below takes what it alone covers");
        input->onMouseMove(1.0f, 1.0f);
        panel->entity()->destroy();
        a->entity()->destroy();
        b->entity()->destroy();
    }

    std::cout << "\nhit padding (ElementInput.buildHitCorners)\n";
    {
        ElementComponent* close = box(screen, "close", 100.0f, 50.0f, 32.0f, 32.0f);
        ButtonComponent* button = addButton(close->entity());
        button->setHitPadding(Vector4(16.0f, 16.0f, 16.0f, 16.0f));
        Recorder r;
        r.follow(close, {"click"});
        // The image covers screen x 100..132, y 50..82: canvas rows 68..100.
        clickAt(90.0f, 84.0f);
        check(r.count("click") == 1, "10 units left of the image is inside the padding");
        clickAt(140.0f, 60.0f);
        check(r.count("click") == 2, "8 units right and 8 above is inside too");
        clickAt(80.0f, 84.0f);
        check(r.count("click") == 2, "20 units left is not");
        close->entity()->setLocalScale(2.0f, 2.0f, 1.0f);   // 64 wide from x 100, padding 32
        clickAt(70.0f, 60.0f);
        check(r.count("click") == 3, "the padding scales with the element (30 left of a 2x image)");

        const auto corners = ElementInput::buildHitCorners(close, close->screenCorners(), Vector3(-1.0f, 1.0f, 1.0f));
        check(corners[0].getX() > corners[1].getX(), "a negative x scale swaps left and right");
        input->onMouseMove(1.0f, 1.0f);
        close->entity()->destroy();
    }

    std::cout << "\ntouch\n";
    {
        ElementComponent* tag = box(screen, "tag", 80.0f, 30.0f, 40.0f, 20.0f);
        Recorder r;
        r.follow(tag, kTouchEvents);
        r.follow(tag, {"mousedown", "mouseup"});

        input->onTouchStart(7, 100.0f, 110.0f);
        check(r.names == std::vector<std::string>{"touchstart"} && r.events[0].touch && r.events[0].touchId == 7,
            "a touch on it fires touchstart with the finger");
        input->onTouchMove(7, 102.0f, 112.0f);
        input->onTouchEnd(7, 102.0f, 112.0f);
        check(r.names == std::vector<std::string>{"touchstart", "touchmove", "click", "touchend"},
            "released over it: touchmove, then a click before touchend");
        r.clear();
        clickAt(100.0f, 110.0f);
        check(r.count("click") == 0 && r.count("mouseup") == 1,
            "the platform's mouse copy of that tap, straight after, clicks nothing (the 300 ms brake)");
        r.clear();

        input->onTouchStart(8, 100.0f, 110.0f);
        input->onTouchMove(8, 10.0f, 10.0f);
        input->onTouchMove(8, 12.0f, 10.0f);
        input->onTouchEnd(8, 12.0f, 10.0f);
        check(r.names == std::vector<std::string>{"touchstart", "touchleave", "touchmove", "touchmove", "touchend"},
            "moving off fires touchleave once, the moves still go to it, and no click");
        r.clear();
        input->onTouchStart(9, 100.0f, 110.0f);
        input->onTouchCancel(9, 100.0f, 110.0f);
        check(r.names == std::vector<std::string>{"touchstart", "touchcancel"}, "a cancelled touch is not a click");
        tag->entity()->destroy();
    }

    std::cout << "\nSDL events (Engine::handleInputEvent)\n";
    {
        ElementComponent* tag = box(screen, "tag", 80.0f, 30.0f, 40.0f, 20.0f);
        Recorder r;
        r.follow(tag, kMouseEvents);
        r.follow(tag, {"touchstart"});
        const auto mouse = [](const SDL_EventType type, const SDL_MouseID which) {
            SDL_Event e{};
            e.type = type;
            e.button.which = which;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.x = 100.0f;
            e.button.y = 110.0f;
            return e;
        };
        engine->handleInputEvent(mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 1));
        engine->handleInputEvent(mouse(SDL_EVENT_MOUSE_BUTTON_UP, 1));
        check(r.count("click") == 1 && r.events.back().button == MouseButton::Left,
            "an SDL press and release over it reach it as a left click");
        r.clear();
        engine->handleInputEvent(mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_TOUCH_MOUSEID));
        engine->handleInputEvent(mouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_TOUCH_MOUSEID));
        check(r.names.empty(), "the mouse events SDL synthesizes from touches are dropped");

        SDL_Event wheel{};
        wheel.type = SDL_EVENT_MOUSE_WHEEL;
        wheel.wheel.which = 1;
        wheel.wheel.mouse_x = 100.0f;
        wheel.wheel.mouse_y = 110.0f;
        wheel.wheel.y = 1.0f;
        wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
        engine->handleInputEvent(wheel);
        check(r.count("mousewheel") == 1 && r.events[0].wheelDelta == 1,
            "a flipped (natural) wheel event is turned back to the physical direction");
        r.clear();

        SDL_Event finger{};
        finger.type = SDL_EVENT_FINGER_DOWN;
        finger.tfinger.touchID = 12345;   // no such DIRECT device: a trackpad, say
        finger.tfinger.x = 100.0f / 300.0f;
        finger.tfinger.y = 110.0f / 150.0f;
        engine->handleInputEvent(finger);
        check(r.count("touchstart") == 0, "a finger on no DIRECT touch device (a trackpad, or SDL video not up) is not a touch");

        // Upstream's stopImmediatePropagation: a press an element handler stops does not
        // reach the mouse device, so game code reading it does not act on a UI click.
        int devicePresses = 0;
        auto deviceHandle = mouseDevice->on("mousedown", [&devicePresses]() { ++devicePresses; });
        engine->handleInputEvent(mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 1));
        engine->handleInputEvent(mouse(SDL_EVENT_MOUSE_BUTTON_UP, 1));
        check(devicePresses == 1, "a press on an element that does not stop it reaches the mouse device too");
        auto stop = tag->on("mousedown", [](ElementInputEvent* event) { event->stopPropagation(); });
        engine->handleInputEvent(mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 1));
        engine->handleInputEvent(mouse(SDL_EVENT_MOUSE_BUTTON_UP, 1));
        check(devicePresses == 1, "a press an element handler stops does not");
        auto press = mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 1);
        press.button.x = 10.0f;
        press.button.y = 10.0f;
        engine->handleInputEvent(press);
        check(devicePresses == 2, "a press on no element reaches it as before");
        stop->off();
        deviceHandle->off();
        input->onMouseMove(1.0f, 1.0f);
        tag->entity()->destroy();
    }

    std::cout << "\nCameraComponent::screenToWorld (upstream)\n";
    {
        // The camera at the origin looking down -Z, vertical fov 45, aspect 2 on 300x150.
        const Vector3 centre = camera->screenToWorld(150.0f, 75.0f, 10.0f);
        check(near(centre.getX(), 0.0f, 1e-4f) && near(centre.getY(), 0.0f, 1e-4f) && near(centre.getZ(), -10.0f, 1e-4f),
            "the canvas centre at distance 10 is 10 down the view axis");
        const Vector3 top = camera->screenToWorld(150.0f, 0.0f, 1.0f);
        const float fov = camera->camera()->fov() * 3.14159265f / 180.0f;
        check(near(std::atan2(top.getY(), -top.getZ()), fov * 0.5f, 1e-4f) && near(top.length(), 1.0f, 1e-4f),
            "the top edge is half the fov up, at distance 1 from the camera");
    }

    std::cout << "\na destroyed element is forgotten\n";
    {
        ElementComponent* tag = box(screen, "tag", 80.0f, 30.0f, 40.0f, 20.0f);
        input->onMouseDown(100.0f, 110.0f, MouseButton::Left);
        check(input->pressedElement() == tag && input->hoveredElement() == tag, "hovered and pressed");
        tag->entity()->destroy();
        check(input->pressedElement() == nullptr && input->hoveredElement() == nullptr,
            "destroying it clears both, so the release reaches nothing freed");
        input->onMouseUp(100.0f, 110.0f, MouseButton::Left);
        input->onTouchStart(3, 100.0f, 110.0f);
    }

    std::cout << "\nworld-space elements (ray through world corners)\n";
    {
        // Two 2x2 elements on no screen, facing the camera at the origin; the canvas centre
        // looks straight at both.
        const auto worldBox = [&](const std::string& name, const float z) {
            Entity* e = addTo(engine->root(), newEntity(name));
            ElementComponent* element = addElement(e, {.type = ElementType::Group, .pivot = Vector2(0.5f, 0.5f),
                .width = 2.0f, .height = 2.0f, .useInput = true});
            e->setLocalPosition(0.0f, 0.0f, z);
            return element;
        };
        ElementComponent* farBox = worldBox("far", -10.0f);
        ElementComponent* nearBox = worldBox("near", -5.0f);
        check(input->elementAt(camera, 150.0f, 75.0f) == nearBox, "the nearer of two hit elements wins");
        nearBox->entity()->setEnabled(false);
        check(input->elementAt(camera, 150.0f, 75.0f) == farBox, "and the farther one once it is gone");
        check(input->elementAt(camera, 10.0f, 10.0f) == nullptr, "a corner of the canvas misses both");
        nearBox->entity()->destroy();
        farBox->entity()->destroy();
    }

    std::cout << "\nmasks (upstream _updateMask)\n";
    {
        // Upstream's masking card: a card mask, a cover mask inside it over a photo, and an
        // avatar mask with its picture, the last element of the card.
        const auto image = [&](GraphNode* parent, const std::string& name, const bool mask, const float w,
                               const float h, const bool useInput = false) {
            Entity* e = addTo(parent, newEntity(name));
            ElementComponent* element = addElement(e, {.type = ElementType::Image, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
                .pivot = Vector2(0.5f, 0.5f), .width = w, .height = h, .useInput = useInput});
            element->setMask(mask);
            return element;
        };
        ElementComponent* card = image(screen, "card", true, 120.0f, 120.0f);
        ElementComponent* background = image(card->entity(), "background", false, 120.0f, 120.0f);
        ElementComponent* cover = image(card->entity(), "cover", true, 100.0f, 40.0f);
        ElementComponent* photo = image(cover->entity(), "photo", false, 200.0f, 200.0f, true);
        ElementComponent* avatar = image(card->entity(), "avatar", true, 30.0f, 30.0f);
        ElementComponent* picture = image(avatar->entity(), "picture", false, 30.0f, 30.0f);
        ElementComponent* outside = image(screen, "outside", false, 10.0f, 10.0f);
        engine->update(0.0f);   // draw orders
        input->syncElements();

        const auto first = [](ElementComponent* e) {
            const auto instances = visualInstances(e->entity());
            return instances.empty() ? nullptr : instances[0];
        };
        const auto unmask = [](ElementComponent* e) {
            const auto instances = visualInstances(e->entity());
            return instances.size() < 2 ? nullptr : instances[1];
        };
        using F = StencilCompareFunction;
        using O = StencilOperation;
        check(card->maskedBy() == nullptr && background->maskedBy() == card && cover->maskedBy() == card &&
              photo->maskedBy() == cover && avatar->maskedBy() == card && picture->maskedBy() == avatar &&
              outside->maskedBy() == nullptr, "maskedBy is the nearest mask above");
        check(stencilIs(first(card), F::Always, O::Replace, 1), "the outer mask writes its depth (ALWAYS, REPLACE 1)");
        check(stencilIs(first(background), F::Equal, O::Keep, 1), "an element under it draws where the stencil is 1");
        check(stencilIs(first(cover), F::Equal, O::IncrementClamp, 1),
              "a nested mask increments inside its parent (EQUAL 1, INCREMENT)");
        check(stencilIs(first(photo), F::Equal, O::Keep, 2), "and what is under it draws at 2");
        check(stencilIs(first(picture), F::Equal, O::Keep, 2), "a sibling nested mask reuses the same depth");
        check(first(outside) && !first(outside)->stencilFront(), "an element under no mask draws with the stencil off");
        check(stencilIs(unmask(cover), F::Equal, O::DecrementClamp, 2) &&
              stencilIs(unmask(card), F::Equal, O::DecrementClamp, 1),
              "each mask is undone by an unmask that decrements its value back to its parent's");
        const double lastOrder = static_cast<double>(picture->drawOrder());
        check(unmask(cover) && std::abs(unmask(cover)->drawOrder() - (photo->drawOrder() + 0.5)) < 1e-9,
              "the cover's unmask draws just after its last descendant (the photo + 0.5)");
        check(unmask(card) && unmask(avatar) && std::abs(unmask(card)->drawOrder() - (lastOrder + 0.5)) < 1e-9 &&
              std::abs(unmask(avatar)->drawOrder() - (lastOrder + 0.499)) < 1e-9,
              "masks ending on the same element unmask innermost first (0.499 before 0.5)");
        const auto* material = first(card) ? dynamic_cast<const Material*>(first(card)->material()) : nullptr;
        check(material && material->alphaMode() == AlphaMode::MASK && material->alphaCutoff() == 1.0f &&
              material->blendState() && !material->blendState()->redWrite() && !material->blendState()->alphaWrite() &&
              material->transparent() && material->depthState() && !material->depthState()->depthWrite(),
              "a mask draws no colour, only where its image is fully opaque (alpha test 1), in the sorted "
              "transparent sublayer and without writing depth");

        // The photo is 200 x 200 but shows only through the 100 x 40 cover: a click on it
        // outside the cover misses it.
        const Vector2 centre(150.0f, 75.0f);
        check(input->elementAt(camera, centre.x, centre.y) == photo, "a click inside the cover reaches the photo");
        check(input->elementAt(camera, centre.x + 70.0f, centre.y) == nullptr,
              "a click on the photo outside its mask does not");
        cover->setMask(false);
        input->syncElements();
        check(photo->maskedBy() == card && stencilIs(first(photo), F::Equal, O::Keep, 1) && !unmask(cover),
              "turning a mask off hands its children to the mask above, and drops its unmask");
        card->entity()->destroy();
        outside->entity()->destroy();
    }

    std::cout << "\ncustom material (upstream image element `material`)\n";
    {
        auto* e = static_cast<ElementComponent*>(addTo(screen, newEntity("overlay"))->addComponent<ElementComponent>());
        e->setup({.type = ElementType::Image, .anchor = Vector4(0, 0, 0, 0), .pivot = Vector2(0, 0),
                  .width = 20.0f, .height = 20.0f});
        engine->update(0.0f);
        input->syncElements();
        const auto instances = [&e]() { return visualInstances(e->entity()); };
        const Material* own = instances().empty() ? nullptr : instances()[0]->material();
        check(own && dynamic_cast<const StandardMaterial*>(own), "an image draws its own material by default");

        auto custom = std::make_shared<StandardMaterial>();
        custom->setEmissive(Color(0.1f, 0.2f, 0.3f, 1.0f));
        e->setMaterial(custom);
        e->setColor(Color(1.0f, 0.0f, 0.0f, 1.0f));
        input->syncElements();
        check(instances().size() == 1 && instances()[0]->material() == custom.get(),
              "a custom material draws the quad instead");
        check(custom->emissive().r == 0.1f && custom->emissive().g == 0.2f,
              "the element's colour does not touch it: colour and opacity are the material's");
        e->setMaterial(nullptr);
        input->syncElements();
        check(!instances().empty() && instances()[0]->material() != custom.get(),
              "clearing it draws the element's own material again");
        e->entity()->destroy();
    }

    std::cout << "\nButtonComponent defaults (upstream #addComponent)\n";
    {
        Entity* e = addTo(engine->root(), newEntity());
        ButtonComponent* b = addButton(e);
        check(b->enabled() && b->isActive() && b->imageEntity() == nullptr, "enabled, active, no image");
        check(b->hitPadding().getX() == 0.0f && b->hitPadding().getW() == 0.0f, "no hit padding");
        check(b->transitionMode() == ButtonTransitionMode::Tint, "tint mode");
        check(b->hoverTint() == Color(0.75f, 0.75f, 0.75f, 1.0f) && b->pressedTint() == Color(0.5f, 0.5f, 0.5f, 1.0f) &&
              b->inactiveTint() == Color(0.25f, 0.25f, 0.25f, 1.0f), "tints 0.75, 0.5, 0.25");
        check(b->fadeDuration() == 0.0f && !b->hoverSprite() && b->hoverSpriteFrame() == 0 && !b->pressedSprite() &&
              !b->inactiveSprite() && b->inactiveSpriteFrame() == 0, "no fade and no sprites");
        e->destroy();
    }

    std::cout << "\n#active\n";
    {
        TestButton t = createButton(engine->root());
        check(near(t.imageElement->color().r, 1.0f), "the image starts at its default white");
        t.component->setActive(false);
        check(near(t.imageElement->color().r, 0.25f) && near(t.imageElement->color().g, 0.25f) &&
              near(t.imageElement->color().b, 0.25f) && near(t.imageElement->opacity(), 1.0f),
            "deactivating applies the inactive tint");
        t.component->setInactiveTint(Color(0.2f, 0.4f, 0.6f, 1.0f));
        check(near(t.imageElement->color().r, 0.2f) && near(t.imageElement->color().g, 0.4f) &&
              near(t.imageElement->color().b, 0.6f), "and reapplies it when it changes while inactive");

        int clicks = 0;
        t.component->on("click", [&clicks]() { ++clicks; });
        ElementInputEvent event;
        event.element = t.button->findComponent<ElementComponent>();
        ElementInputEvent* pointer = &event;
        event.element->fire("click", pointer);
        check(clicks == 0, "an inactive button fires no events");
        t.component->setActive(true);
        event.element->fire("click", pointer);
        check(clicks == 1, "an active one does");
        t.button->destroy();
    }

    std::cout << "\n#transitionMode\n";
    {
        TestButton t = createButton(engine->root());
        t.component->setActive(false);
        t.component->setInactiveSpriteFrame(2);
        check(near(t.imageElement->color().r, 0.25f), "tint mode has applied the inactive tint");
        t.component->setTransitionMode(ButtonTransitionMode::Tint);
        check(near(t.imageElement->color().r, 0.25f), "setting the same mode changes nothing");
        t.component->setTransitionMode(ButtonTransitionMode::SpriteChange);
        check(near(t.imageElement->color().r, 1.0f), "switching to sprites restores the default tint");
        check(t.imageElement->spriteFrame() == 2, "and shows the inactive sprite frame");
        t.button->destroy();
    }

    std::cout << "\n#imageEntity\n";
    {
        Entity* image1 = addTo(engine->root(), newEntity("image1"));
        ElementComponent* element1 = addElement(image1, {.type = ElementType::Image});
        Entity* image2 = addTo(engine->root(), newEntity("image2"));
        ElementComponent* element2 = addElement(image2, {.type = ElementType::Image});
        Entity* e = addTo(engine->root(), newEntity());
        ButtonComponent* b = addButton(e);
        b->setImageEntity(image1);
        check(element1->hasEvent("set:color") && !element2->hasEvent("set:color"), "it follows image 1");
        b->setImageEntity(image2);
        check(b->imageEntity() == image2 && !element1->hasEvent("set:color") && element2->hasEvent("set:color"),
            "reassigned, it lets go of image 1 and follows image 2");
        b->setImageEntity(nullptr);
        check(b->imageEntity() == nullptr && !element2->hasEvent("set:color"), "null lets go of both");

        // An element added to the image entity AFTER the button names it (upstream element:add).
        Entity* late = addTo(engine->root(), newEntity("late"));
        b->setImageEntity(late);
        b->setActive(false);
        ElementComponent* lateElement = addElement(late, {.type = ElementType::Image});
        engine->update(0.0f);
        check(near(lateElement->color().r, 0.25f), "an image element added later is picked up on the next update");
        e->destroy();
        image1->destroy();
        image2->destroy();
        late->destroy();
    }

    std::cout << "\n#cloneComponent\n";
    {
        TestButton t = createButton(engine->root());
        auto sprite = std::make_shared<Sprite>();
        t.component->setActive(false);
        t.component->setHitPadding(Vector4(1, 2, 3, 4));
        t.component->setHoverTint(Color(0.1f, 0.2f, 0.3f, 0.4f));
        t.component->setPressedTint(Color(0.5f, 0.6f, 0.7f, 0.8f));
        t.component->setInactiveTint(Color(0.9f, 0.8f, 0.7f, 0.6f));
        t.component->setFadeDuration(100.0f);
        t.component->setHoverSprite(sprite);
        t.component->setHoverSpriteFrame(1);
        t.component->setPressedSpriteFrame(2);
        t.component->setInactiveSpriteFrame(3);
        Entity* clone = t.button->clone();
        engine->root()->addChild(clone);
        const auto* c = clone->findComponent<ButtonComponent>();
        check(c && !c->isActive() && c->hitPadding().getZ() == 3.0f && c->hoverTint() == Color(0.1f, 0.2f, 0.3f, 0.4f) &&
              c->pressedTint() == Color(0.5f, 0.6f, 0.7f, 0.8f) && c->inactiveTint() == Color(0.9f, 0.8f, 0.7f, 0.6f) &&
              c->fadeDuration() == 100.0f && c->hoverSprite() == sprite && c->hoverSpriteFrame() == 1 &&
              c->pressedSpriteFrame() == 2 && c->inactiveSpriteFrame() == 3, "every property is cloned");
        const GraphNode* cloneImage = clone->findByName("image");
        check(c && cloneImage && c->imageEntity() == cloneImage && cloneImage != t.image,
            "the image entity is remapped to the cloned child");
        t.button->destroy();
        clone->destroy();
    }

    std::cout << "\nstates through ElementInput\n";
    {
        // A tint button on the screen tinting itself, as the buttons example builds them.
        Entity* e = addTo(screen, newEntity("play"));
        ElementComponent* element = addElement(e, {.type = ElementType::Image, .anchor = Vector4(0, 0, 0, 0),
            .pivot = Vector2(0, 0), .width = 100.0f, .height = 40.0f, .useInput = true});
        element->setColor(Color(1.0f, 0.55f, 0.2f, 1.0f));
        e->setLocalPosition(100.0f, 50.0f, 0.0f);   // canvas rows 60..100
        ButtonComponent* b = addButton(e);
        b->setImageEntity(e);
        b->setHoverTint(Color(1.0f, 0.7f, 0.45f, 1.0f));
        b->setPressedTint(Color(0.8f, 0.4f, 0.1f, 1.0f));
        Recorder r;
        r.follow(b, {"hoverstart", "hoverend", "pressedstart", "pressedend", "click", "mouseenter"});

        input->onMouseMove(150.0f, 80.0f);
        check(near(element->color().g, 0.7f) && b->visualState() == ButtonComponent::VisualState::Hover,
            "hovering applies the hover tint");
        input->onMouseDown(150.0f, 80.0f, MouseButton::Left);
        check(near(element->color().g, 0.4f), "pressing applies the pressed tint");
        input->onMouseUp(150.0f, 80.0f, MouseButton::Left);
        check(near(element->color().g, 0.7f), "releasing goes back to hover");
        check(r.names == std::vector<std::string>{"hoverstart", "mouseenter", "hoverend", "pressedstart",
                                                  "pressedend", "hoverstart", "click"},
            "hoverstart/end, pressedstart/end and the click, in upstream's order");
        check(r.events.back().element == element, "the button re-fires the element's event");
        input->onMouseMove(10.0f, 10.0f);
        check(near(element->color().g, 0.55f), "leaving restores the image's own colour");

        element->setColor(Color(0.2f, 0.9f, 0.3f, 1.0f));
        input->onMouseMove(150.0f, 80.0f);
        input->onMouseMove(10.0f, 10.0f);
        check(near(element->color().g, 0.9f), "a colour the application sets becomes the default");

        b->setFadeDuration(100.0f);
        input->onMouseMove(150.0f, 80.0f);
        check(near(element->color().g, 0.9f), "with a fade the tint does not jump");
        engine->update(0.05f);
        check(near(element->color().g, 0.8f, 1e-4f), "half the fade later it is half way");
        engine->update(0.05f);
        check(near(element->color().g, 0.7f), "and at the end it is the hover tint");

        b->setEnabled(false);
        check(near(element->color().g, 0.9f), "disabling the button restores the default look");
        b->setEnabled(true);
        input->onMouseMove(1.0f, 1.0f);
        e->destroy();
    }

    std::cout << "\nsprite change through the states\n";
    {
        Entity* e = addTo(screen, newEntity("options"));
        ElementComponent* element = addElement(e, {.type = ElementType::Image, .anchor = Vector4(0, 0, 0, 0),
            .pivot = Vector2(0, 0), .width = 100.0f, .height = 40.0f, .useInput = true});
        auto states = std::make_shared<Sprite>();
        element->setSprite(states);
        e->setLocalPosition(100.0f, 50.0f, 0.0f);
        ButtonComponent* b = addButton(e);
        b->setImageEntity(e);
        b->setTransitionMode(ButtonTransitionMode::SpriteChange);
        b->setHoverSpriteFrame(1);
        b->setPressedSpriteFrame(2);
        b->setInactiveSpriteFrame(3);
        input->onMouseMove(150.0f, 80.0f);
        check(element->spriteFrame() == 1 && element->sprite() == states, "hover shows frame 1 of the same sprite");
        input->onMouseDown(150.0f, 80.0f, MouseButton::Left);
        check(element->spriteFrame() == 2, "pressed shows frame 2");
        input->onMouseUp(150.0f, 80.0f, MouseButton::Left);
        input->onMouseMove(10.0f, 10.0f);
        check(element->spriteFrame() == 0, "and the default frame comes back");
        b->setActive(false);
        check(element->spriteFrame() == 3, "inactive shows frame 3");
        e->destroy();
    }

    std::cout << (failures == 0 ? "\nAll element input tests passed\n" : "\nElement input tests FAILED\n");
    input.reset();
    engine.reset();
    return failures == 0 ? 0 : 1;
}
