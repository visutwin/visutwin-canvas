// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// UI element layout and screens, as upstream's element component.test.mjs has them (the
// constructor defaults, screen binding, and every `position` case of #9525), plus the
// screen's scale and projection and a few anchor cases the upstream suite leaves implicit.
//
// An ElementComponent that stores anchor, pivot and margins without anything reading them
// fails every placement case here.
//
// A real engine on a stub device. With no window, Engine::canvasSize() is the device's
// size, which is how a test resizes a screen-space screen: change the stub's size and
// let an engine update deliver it.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/appOptions.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"

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

    bool near(const float a, const float b, const float eps = 1e-4f) { return std::abs(a - b) <= eps; }

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

    std::shared_ptr<StubDevice> device;
    std::shared_ptr<Engine> engine;

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

    ElementComponent* addElement(Entity* e, const ElementDesc& desc = {})
    {
        auto* element = static_cast<ElementComponent*>(e->addComponent<ElementComponent>());
        element->setup(desc);
        return element;
    }

    ScreenComponent* addScreen(Entity* e, const bool screenSpace)
    {
        auto* screen = static_cast<ScreenComponent*>(e->addComponent<ScreenComponent>());
        screen->setScreenSpace(screenSpace);
        return screen;
    }

    Entity* screenSpaceScreen()
    {
        Entity* screen = addTo(engine->root(), newEntity("screen"));
        addScreen(screen, true);
        return screen;
    }

    bool contains(const std::vector<ElementComponent*>& list, const ElementComponent* e)
    {
        return std::find(list.begin(), list.end(), e) != list.end();
    }

    void resizeCanvas(const int w, const int h)
    {
        device->setResolution(w, h);
        engine->update(0.0f);
    }

    float posX(Entity* e) { return e->localPosition().getX(); }
    float posY(Entity* e) { return e->localPosition().getY(); }
}

int main()
{
    std::cout << std::unitbuf;

    device = std::make_shared<StubDevice>();
    engine = std::make_shared<Engine>(nullptr);
    AppOptions options;
    options.graphicsDevice = device;
    options.registerComponentSystem<ScreenComponentSystem>();
    options.registerComponentSystem<ElementComponentSystem>();
    engine->init(options);

    std::cout << "a default element (upstream #constructor)\n";
    {
        Entity* e = newEntity();
        auto* el = addElement(e);
        check(el->anchor().getX() == 0.0f && el->anchor().getW() == 0.0f, "anchor 0, 0, 0, 0");
        check(el->pivot().x == 0.0f && el->pivot().y == 0.0f, "pivot 0, 0");
        check(el->calculatedWidth() == 32.0f && el->calculatedHeight() == 32.0f && el->width() == 32.0f &&
              el->height() == 32.0f, "32 x 32");
        check(el->left() == 0.0f && el->bottom() == 0.0f && el->right() == -32.0f && el->top() == -32.0f,
            "margins 0, 0, -32, -32");
        check(el->screen() == nullptr && el->type() == ElementType::Group, "no screen, a group");
        const auto& sc = el->screenCorners();
        const auto& cc = el->canvasCorners();
        check(sc[2].getX() == 0.0f && sc[2].getY() == 0.0f && cc[2].x == 0.0f, "screen and canvas corners zero");
        const auto& wc = el->worldCorners();
        check(near(wc[0].getX(), 0) && near(wc[1].getX(), 32) && near(wc[2].getX(), 32) && near(wc[2].getY(), 32) &&
              near(wc[3].getY(), 32) && near(wc[3].getX(), 0), "world corners 0,0  32,0  32,32  0,32");
        delete e;
    }

    std::cout << "\nscreen binding\n";
    {
        Entity* screenEntity = screenSpaceScreen();
        auto* screen = screenEntity->findComponent<ScreenComponent>();
        Entity* e = addTo(screenEntity, newEntity());
        auto* el = addElement(e);
        check(contains(screen->elements(), el) && el->screen() == screenEntity, "an element under a screen binds to it");
        engine->root()->addChild(e->remove());
        check(!contains(screen->elements(), el) && el->screen() == nullptr, "and unbinds on reparent");
        screenEntity->addChild(e->remove());
        check(contains(screen->elements(), el), "rebinds when put back");
        e->destroy();
        check(!contains(screen->elements(), el), "and unbinds on destroy");
        auto ownedElement = e->remove();   // frees it
        ownedElement.reset();
        screenEntity->destroy();
        auto ownedScreen = screenEntity->remove();
    }
    {
        // upstream #1151: reparented after its screen was destroyed
        Entity* screenEntity = screenSpaceScreen();
        Entity* e = addTo(screenEntity, newEntity());
        auto* el = addElement(e);
        auto detached = e->remove();
        screenEntity->destroy();
        auto deadScreen = screenEntity->remove();
        deadScreen.reset();
        check(el->screen() == nullptr, "a destroyed screen clears the element's reference (#1151)");
        Entity* newParent = addTo(engine->root(), newEntity("newParent"));
        newParent->addChild(std::move(detached));
        check(el->screen() == nullptr, "and it can be reparented afterwards");
        newParent->destroy();
        auto ownedParent = newParent->remove();
    }

    std::cout << "\nposition (upstream #9525)\n";
    Entity* screen = screenSpaceScreen();
    const auto place = [&](const std::string& what, Entity* e, const float x, const float y) {
        check(near(posX(e), x) && near(posY(e), y),
            what + " (" + std::to_string(posX(e)) + ", " + std::to_string(posY(e)) + ")");
    };
    {
        Entity* e = addTo(screen, newEntity());
        e->setLocalPosition(0, -40, 0);
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0.5f, 1, 0.5f, 1), .pivot = Vector2(0.5f, 1)});
        place("keeps the position of an entity already under a screen", e, 0, -40);
    }
    {
        Entity* e = addTo(screen, newEntity());
        e->setLocalPosition(0, -40, 0);
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0.5f, 1, 0.5f, 1), .pivot = Vector2(0.5f, 1),
                       .width = 32.0f, .height = 32.0f});
        place("with the default size given", e, 0, -40);
    }
    {
        Entity* e = newEntity();
        e->setLocalPosition(10, 20, 0);
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f)});
        addTo(screen, e);
        place("added to a screen afterwards", e, 10, 20);
    }
    {
        Entity* e = newEntity();
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f)});
        e->setLocalPosition(10, 20, 0);
        addTo(screen, e);
        place("a position set before it is added to a screen", e, 10, 20);
    }
    {
        Entity* panel = newEntity("panel");
        addElement(panel, {.type = ElementType::Group, .anchor = Vector4(0, 0, 1, 1), .margin = Vector4(0, 0, 0, 0)});
        Entity* e = addTo(panel, newEntity());
        e->setLocalPosition(0, -40, 0);
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0.5f, 1, 0.5f, 1), .pivot = Vector2(0.5f, 1)});
        addTo(screen, panel);
        place("added to a screen with its parent", e, 0, -40);
    }
    {
        Entity* ui = addTo(engine->root(), newEntity("ui"));
        Entity* e = addTo(ui, newEntity());
        e->setLocalPosition(0, -40, 0);
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0.5f, 1, 0.5f, 1), .pivot = Vector2(0.5f, 1)});
        addScreen(ui, true);
        place("whose screen is added afterwards", e, 0, -40);
        check(e->findComponent<ElementComponent>()->screen() == ui, "and the later screen owns it");
    }
    {
        Entity* e = addTo(screen, newEntity());
        e->setLocalPosition(0, -40, 0);
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0.5f, 1, 0.5f, 1), .pivot = Vector2(0.5f, 1)});
        Entity* clone = e->clone();
        addTo(screen, clone);
        place("a clone", clone, 0, -40);
    }
    {
        Entity* e = addTo(screen, newEntity());
        e->setLocalPosition(0, -40, 0);
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0, 1, 1, 1), .pivot = Vector2(0.5f, 1),
                       .left = 0.0f, .right = 0.0f});
        check(near(posY(e), -40), "keeps the vertical position when only horizontal margins are given");
    }
    {
        Entity* e = addTo(screen, newEntity());
        e->setLocalPosition(25, 0, 0);
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0, 0, 0, 1), .pivot = Vector2(0, 0.5f),
                       .bottom = 0.0f, .top = 0.0f});
        check(near(posX(e), 25), "keeps the horizontal position when only vertical margins are given");
    }
    {
        Entity* e = addTo(screen, newEntity());
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0.5f, 1, 0.5f, 1), .pivot = Vector2(0.5f, 1)});
        e->setLocalPosition(0, -40, 0);
        resizeCanvas(640, 320);
        place("a position set after it is added survives a screen resize", e, 0, -40);
        resizeCanvas(300, 150);
    }
    {
        Entity* e = addTo(screen, newEntity());
        addElement(e, {.type = ElementType::Image, .anchor = Vector4(0.5f, 1, 0.5f, 1), .pivot = Vector2(0.5f, 1)});
        e->translateLocal(0, -40, 0);
        e->worldTransform();   // the next frame's sync, which updates the margins
        resizeCanvas(640, 320);
        place("and so does a translation", e, 0, -40);
        resizeCanvas(300, 150);
    }
    {
        Entity* e = addTo(screen, newEntity());
        auto* el = addElement(e, {.type = ElementType::Image, .anchor = Vector4(0, 0, 0, 0), .pivot = Vector2(0, 0),
                                  .margin = Vector4(10, 20, -42, -52)});
        place("places the element with the margins it is given", e, 10, 20);
        check(el->calculatedWidth() == 32.0f && el->calculatedHeight() == 32.0f, "32 x 32 from those margins");
    }

    std::cout << "\nanchors, stretching and screens\n";
    {
        // A panel stretched over the whole screen follows its size.
        Entity* panel = addTo(screen, newEntity("fill"));
        auto* el = addElement(panel, {.anchor = Vector4(0, 0, 1, 1), .margin = Vector4(10, 10, 10, 10)});
        check(near(el->calculatedWidth(), 280) && near(el->calculatedHeight(), 130),
            "split anchors: the screen less the margins (280 x 130)");
        resizeCanvas(640, 320);
        check(near(el->calculatedWidth(), 620) && near(el->calculatedHeight(), 300),
            "and it stretches with the screen (620 x 300)");
        const auto& cc = el->canvasCorners();
        check(near(cc[0].x, 10) && near(cc[0].y, 310) && near(cc[2].x, 630) && near(cc[2].y, 10),
            "canvas corners: y down from the top of the canvas");
        resizeCanvas(300, 150);
    }
    {
        // Scale: blend, in log space, as upstream.
        Entity* s = addTo(engine->root(), newEntity("scaled"));
        auto* sc = addScreen(s, true);
        sc->setScaleMode(ScreenScaleMode::Blend);
        sc->setReferenceResolution(Vector2(150, 150));
        sc->setScaleBlend(0.5f);
        check(near(sc->scale(), std::sqrt(2.0f * 1.0f)), "blend 0.5 over 2x and 1x: scale sqrt(2)");
        sc->setScaleBlend(0.0f);
        check(near(sc->scale(), 2.0f), "blend 0: width only, scale 2");
        sc->setScaleMode(ScreenScaleMode::None);
        check(near(sc->scale(), 1.0f) && sc->referenceResolution().x == sc->resolution().x,
            "mode None: scale 1, and the reference reads back as the resolution");
        // A screen-space screen's projection maps its top-left corner to NDC (-1, 1).
        const Vector3 topLeft = sc->screenMatrix().transformPoint(Vector3(0, 0, 0));
        const Vector3 bottomRight = sc->screenMatrix().transformPoint(Vector3(300, -150, 0));
        check(near(topLeft.getX(), -1) && near(topLeft.getY(), 1) && near(bottomRight.getX(), 1) &&
              near(bottomRight.getY(), -1), "screen matrix: top-left to (-1, 1), bottom-right to (1, -1)");
        sc->setScreenSpace(false);
        sc->setResolution(Vector2(4, 2));
        check(sc->resolution().x == 4.0f && sc->resolution().y == 2.0f, "a world-space screen keeps the resolution it is given");
        sc->setScaleMode(ScreenScaleMode::Blend);
        check(sc->scaleMode() == ScreenScaleMode::None, "and refuses a scale mode");
    }
    {
        // A screen-space element's world transform is in clip space: its pivot lands at NDC.
        Entity* e = addTo(screen, newEntity("centred"));
        addElement(e, {.anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f), .margin =
                       Vector4(-16, -16, -16, -16)});
        const Vector3 ndc = e->position();
        check(near(ndc.getX(), 0) && near(ndc.getY(), 0), "a centred screen-space element's pivot is at NDC (0, 0)");
        e->setPosition(Vector3(0.5f, 0.5f, 0.0f));
        check(near(posX(e), 75) && near(posY(e), 37.5f), "setPosition takes NDC back into screen units (75, 37.5)");
    }

    std::cout << "\ndraw order (upstream ScreenComponent._processDrawOrderSync)\n";
    {
        // Depth-first from 1, so a child draws over its parent and a later sibling over an
        // earlier one; the screen's priority takes the top 8 bits. Resolved on update.
        Entity* orderScreen = screenSpaceScreen();
        Entity* panel = addTo(orderScreen, newEntity("panel"));
        ElementComponent* panelElement = addElement(panel);
        Entity* label = addTo(panel, newEntity("label"));
        ElementComponent* labelElement = addElement(label);
        Entity* bar = addTo(panel, newEntity("bar"));
        ElementComponent* barElement = addElement(bar);
        Entity* sibling = addTo(orderScreen, newEntity("sibling"));
        ElementComponent* siblingElement = addElement(sibling);

        check(orderScreen->findComponent<ScreenComponent>()->drawOrderDirty(), "binding elements queues a sync");
        engine->update(0.0f);
        check(!orderScreen->findComponent<ScreenComponent>()->drawOrderDirty(), "the update resolves it");
        check(panelElement->drawOrder() == 1 && labelElement->drawOrder() == 2 &&
              barElement->drawOrder() == 3 && siblingElement->drawOrder() == 4,
              "parent, its children in order, then the next sibling: 1, 2, 3, 4");

        orderScreen->findComponent<ScreenComponent>()->setPriority(2);
        engine->update(0.0f);
        check(panelElement->drawOrder() == (2 << 24) + 1 && siblingElement->drawOrder() == (2 << 24) + 4,
              "priority 2 goes in the top 8 bits");

        // Moving the label out of the panel to the end of the screen re-derives the order.
        orderScreen->addChild(panel->removeChild(label));
        engine->update(0.0f);
        const int p = 2 << 24;
        check(panelElement->drawOrder() == p + 1 && barElement->drawOrder() == p + 2 &&
              siblingElement->drawOrder() == p + 3 && labelElement->drawOrder() == p + 4,
              "after moving the label to the end: panel, bar, sibling, label");
    }

    std::cout << (failures == 0 ? "\nAll element layout tests passed\n" : "\nElement layout tests FAILED\n");
    engine.reset();
    return failures == 0 ? 0 : 1;
}
