// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream physics/raycast.
//
// Two rows of static physics shapes (box, capsule, cone, cylinder, sphere) at
// x = 1, 3, 5, 7, 9 and y = +2 / -2, green on a lavender clear colour, lit by one
// directional light at euler (45, 30, 0) and ambient 0.2. Every frame all shapes
// reset to green, then two horizontal rays from x = 0 to x = 10 bob with
// 1.2 * sin(t) around each row: the top one uses raycastFirst and paints only the
// nearest hit red, the bottom one uses raycastAll and paints every shape it passes
// through. White lines draw the rays and a blue line 0.3 long shows the surface
// normal at each hit. World-space labels "raycastFirst" and "raycastAll" sit above
// the rows.
//
// The raycasts go through a real PhysicsWorld (Jolt), supplied in configure(), as
// upstream's go through Ammo — not the CPU fallback over collision bounds.
//
// DEVIATIONS:
// - The physics seam has no cone shape, so the cone's collision volume is a
//   cylinder of the same radius (0.5) and height (1). The ray therefore hits the
//   cone slightly wider near its tip than upstream's btConeShape would.
// - Upstream draws the rays and normals with app.drawLine (1-pixel hardware lines).
//   This port has no immediate line drawing, so they are WideLines one pixel wide
//   in a WideLineRenderer. To match drawLine's one-frame lifetime, the normal lines
//   are removed from the renderer at the start of every frame and one is added per
//   hit, so the segment count changes from frame to frame.
// - ElementComponent::setFontSize takes an int, so upstream's 0.5 font size is
//   expressed as fontSize 64 on an entity scaled by 0.5 / 64 — the same size in
//   world units (see anisotropy-example.cpp).
//
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/input/elementInput.h"
#include "framework/physics/jolt/joltPhysicsWorld.h"
#include "scene/graphics/wideLine.h"
#include "scene/graphics/wideLineRenderer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kNormalLength = 0.3f;
    constexpr float kLineWidthPixels = 1.0f;
}

class RaycastExample final: public ExampleApp
{
public:
    RaycastExample(): ExampleApp({.title = "Raycast"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<CollisionComponentSystem>();
        options.registerComponentSystem<RigidBodyComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        options.physicsWorld = createJoltPhysicsWorld();

        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
    }

    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        _red = createMaterial(Color(1.0f, 0.0f, 0.0f, 1.0f));
        _green = createMaterial(Color(0.0f, 1.0f, 0.0f, 1.0f));

        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f));

        auto* camera = createCamera(Vector3(5.0f, 0.0f, 15.0f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setClearColor(Color(0.5f, 0.5f, 0.8f, 1.0f));
        }

        const std::vector<std::string> types = {"box", "capsule", "cone", "cylinder", "sphere"};
        for (size_t idx = 0; idx < types.size(); ++idx) {
            createPhysicalShape(types[idx], static_cast<float>(idx) * 2.0f + 1.0f, 2.0f, 0.0f);
        }
        for (size_t idx = 0; idx < types.size(); ++idx) {
            createPhysicalShape(types[idx], static_cast<float>(idx) * 2.0f + 1.0f, -2.0f, 0.0f);
        }

        _rigidbodySystem = dynamic_cast<RigidBodyComponentSystem*>(
            engine()->systems()->getById("rigidbody"));
        if (!_rigidbodySystem) {
            spdlog::error("RigidBodyComponentSystem not available");
            return false;
        }

        _lines = std::make_unique<WideLineRenderer>(engine(), device());
        _lines->setScreenSize(static_cast<float>(windowWidth()), static_cast<float>(windowHeight()));
        _lines->add(&_rayFirst);
        _lines->add(&_rayAll);

        // DEVIATION: upstream loads its arial atlas; this is Liberation Sans, which is
        // metric-compatible with Arial (identical advances) and OFL-licensed.
        _font = std::make_unique<Asset>("label-font", AssetType::FONT, assetPath("fonts/liberation-sans.json"));
        createText("raycastFirst", 0.5f, 3.75f, 0.0f, 0.0f);
        createText("raycastAll", 0.5f, -0.25f, 0.0f, 0.0f);

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Reset all shapes to green.
        for (auto* render : _renders) {
            render->setMaterial(_green.get());
        }

        // Last frame's normals go, as drawLine's would; onHit adds this frame's.
        for (size_t i = 0; i < _normalLinesInUse; ++i) {
            _lines->remove(_normalLines[i].get());
        }
        _normalLinesInUse = 0;

        const Color white(1.0f, 1.0f, 1.0f, 1.0f);

        float y = 2.0f + 1.2f * std::sin(_time);
        Vector3 start(0.0f, y, 0.0f);
        Vector3 end(10.0f, y, 0.0f);

        // Render the ray used in the raycast.
        _rayFirst.setPoints({start, end}, white, kLineWidthPixels);

        if (const auto result = _rigidbodySystem->raycastFirst(start, end); result.has_value()) {
            onHit(*result);
        }

        y = -2.0f + 1.2f * std::sin(_time);
        start = Vector3(0.0f, y, 0.0f);
        end = Vector3(10.0f, y, 0.0f);

        _rayAll.setPoints({start, end}, white, kLineWidthPixels);

        for (const auto& result : _rigidbodySystem->raycastAll(start, end)) {
            onHit(result);
        }

        _lines->update();
    }

    void preRender() override
    {
        _elementInput->syncTextElements();
    }

    void destroy() override
    {
        _lines.reset();
    }

private:
    static std::shared_ptr<StandardMaterial> createMaterial(const Color& color)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        return material;
    }

    void createPhysicalShape(const std::string& type, const float x, const float y, const float z)
    {
        // As upstream: the position is set before the static rigid body exists,
        // because static bodies are never moved after creation.
        auto* entity = createPrimitive(type.c_str(), _green.get(), Vector3(x, y, z));
        if (auto* render = entity->findComponent<RenderComponent>()) {
            _renders.push_back(render);
        }

        if (auto* body = static_cast<RigidBodyComponent*>(entity->addComponent<RigidBodyComponent>())) {
            body->setType(RigidBodyType::Static);
        }

        if (auto* collision = static_cast<CollisionComponent*>(entity->addComponent<CollisionComponent>())) {
            // DEVIATION: no cone collision shape; a cylinder stands in (see header).
            collision->setType(type == "cone" ? "cylinder" : type);
            collision->setHeight(type == "capsule" ? 2.0f : 1.0f);
        }
    }

    void onHit(const RaycastResult& result)
    {
        if (auto* render = result.entity ? result.entity->findComponent<RenderComponent>() : nullptr) {
            render->setMaterial(_red.get());
        }

        // Render the normal on the surface from the hit point.
        // The lines are pooled only so the renderer's raw pointers stay valid; the
        // pool grows to the most hits seen in one frame.
        if (_normalLinesInUse == _normalLines.size()) {
            _normalLines.push_back(std::make_unique<WideLine>());
        }
        WideLine* line = _normalLines[_normalLinesInUse++].get();
        line->setPoints({result.point, result.point + result.normal * kNormalLength},
            Color(0.0f, 0.0f, 1.0f, 1.0f), kLineWidthPixels);
        _lines->add(line);
    }

    void createText(const std::string& message, const float x, const float y, const float z, const float rot)
    {
        constexpr int kFontSize = 64;
        constexpr float kFontSizeWorld = 0.5f;
        const float scale = kFontSizeWorld / static_cast<float>(kFontSize);

        FontResource* fontResource = nullptr;
        if (const auto res = _font->resource();
            res.has_value() && std::holds_alternative<FontResource*>(*res)) {
            fontResource = std::get<FontResource*>(*res);
        }
        if (!fontResource) {
            spdlog::warn("liberation-sans.json failed to load; the labels will be missing");
            return;
        }

        auto* text = new Entity();
        text->setEngine(engine());
        if (auto* element = static_cast<ElementComponent*>(text->addComponent<ElementComponent>())) {
            element->setType(ElementType::Text);
            element->setAnchor(Vector4(0.5f, 0.5f, 0.5f, 0.5f));
            element->setFontResource(fontResource);
            element->setFontSize(kFontSize);
            // A single line is centred on the pivot's y only when height == fontSize.
            element->setHeight(static_cast<float>(kFontSize));
            element->setWidth(static_cast<float>(kFontSize) * 16.0f);
            element->setPivot(Vector2(0.0f, 0.5f));
            // Upstream's element sizes itself to the text (autoWidth), so pivot x 0
            // starts the text at the entity. This box is wider than the line, and
            // left alignment is what puts the text at the same place.
            element->setHorizontalAlign(ElementHorizontalAlign::Left);
            element->setText(message);
        }
        text->setLocalPosition(x, y, z);
        text->setLocalEulerAngles(0.0f, 0.0f, rot);
        text->setLocalScale(scale, scale, scale);
        root()->addChild(text);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _font;

    std::shared_ptr<StandardMaterial> _red;
    std::shared_ptr<StandardMaterial> _green;
    std::vector<RenderComponent*> _renders;

    std::unique_ptr<WideLineRenderer> _lines;
    WideLine _rayFirst;
    WideLine _rayAll;
    std::vector<std::unique_ptr<WideLine>> _normalLines;
    size_t _normalLinesInUse = 0;

    RigidBodyComponentSystem* _rigidbodySystem = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(RaycastExample)
