// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.07.2026
//
// Port of upstream graphics/lights. A statue on a large grey metallic ground box, lit by
// animated lights of three types, one of each to begin with. Lighting is CLUSTERED (the
// default) with cookies turned on, so every spot and omni light is shaded through the
// cluster grid, its shadow comes from the clustered shadow atlas and its cookie from the
// cookie atlas; only the directional lights go through the forward light array.
//   * SPOT (white, orange, blue)       — orbit high above the statue, aimed at it, each
//                                        projecting the alpha of heart.png as a COOKIE
//                                        and casting a shadow; an emissive cone marks
//                                        each one.
//   * OMNI (yellow, green, pink)       — orbit low and fast the other way, spinning, each
//                                        projecting the christmas cubemap COOKIE and
//                                        casting a cubemap shadow; an emissive sphere
//                                        marks each one. A clustered omni cookie is
//                                        world-aligned, so the spin turns the marker,
//                                        not the cookie.
//   * DIRECTIONAL (cyan, orange, pink) — sweep their yaw at different elevations, two
//                                        2048 cascades over 300 m.
// Lights can be added and removed per type, the colours repeating after the third. The
// lights of a type share a turn round the statue equally, and the shape and extent of
// every light (range sphere, cone, direction arrow) is drawn as thin lines in the
// light's own colour. The local lights' shadow and cookie resolutions are those of the
// two atlases, split between the lights that need a slot, not each light's own
// shadow resolution.
//
// Keys, standing in for upstream's control panel:
//   1 / 2 / 3      toggle the omni / spot / directional lights
//   Z / X / C      select omni / spot / directional for the keys below
//   = / -          add / remove a light of the selected type
//   I / K          intensity of the selected type up / down by 0.1
//   O / L          shadow intensity of the selected type up / down by 0.1
//   P / ;          cookie intensity of the selected type up / down by 0.1 (not directional)
//   G              toggle the light shapes
//   R reset camera | F1 HUD | Esc quit | LMB/RMB orbit, Shift/MMB pan, Wheel zoom
//
// DEVIATION: no flat-shading toggle (upstream's MATERIAL panel). The port has no
// material flag that shades by the geometric normal.
// DEVIATION: at most TWO directional lights per layer are shadowed
// (ShadowParams::kMaxDirectionalShadows), where upstream shadows every one; a third
// directional light lights the scene unshadowed, and the renderer warns once.
// DEVIATION: the light shapes are WideLineRenderer segments one pixel wide on the
// Immediate layer, standing in for upstream's WireRenderer, which this port does not
// have. Each segment is clipped to the near plane first: a wide line is expanded in
// screen space, and the omni ranges enclose the camera.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/graphics/wideLine.h"
#include "scene/graphics/wideLineRenderer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    // Omni light cookie: a cubemap asset of six faces, in the engine's cube order
    // +X, -X, +Y, -Y, +Z, -Z.
    const std::array<const char*, 6> xmasFaceFiles = {
        "xmas_posx", "xmas_negx", "xmas_posy", "xmas_negy", "xmas_posz", "xmas_negz"
    };

    enum LightKind : int { Omni = 0, Spot = 1, Directional = 2 };

    const std::array<const char*, 3> kindNames = {"omni", "spot", "directional"};

    // A palette per type, so several lights of one type are told apart by colour as well
    // as by where their light and shadows fall. The first keeps the type's usual colour.
    const std::array<std::array<Color, 3>, 3> palettes = {{
        {Color(1.0f, 1.0f, 0.0f), Color(0.4f, 1.0f, 0.5f), Color(1.0f, 0.4f, 0.5f)},   // omni
        {Color(1.0f, 1.0f, 1.0f), Color(1.0f, 0.6f, 0.3f), Color(0.5f, 0.8f, 1.0f)},   // spot
        {Color(0.0f, 1.0f, 1.0f), Color(1.0f, 0.5f, 0.2f), Color(1.0f, 0.3f, 1.0f)},   // directional
    }};

    // Segments approximating a full circle of a light shape.
    constexpr int kCircleSteps = 20;

    constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;
}

class LightsExample final: public ExampleApp
{
public:
    LightsExample(): ExampleApp({.title = "Lights Example", .width = 1100, .height = 750}) {}

protected:
    bool create() override
    {
        spdlog::info("*** Lights Example Started ***");

        // Enable cookies, which are off by default for clustered lighting
        scene()->lighting().cookiesEnabled = true;

        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // -----------------------------------------------------------------------
        // Cookie textures.
        // -----------------------------------------------------------------------
        // Spot light cookie: the heart's ALPHA channel masks the beam.
        _heartAsset = std::make_unique<Asset>(
            "heart", AssetType::TEXTURE, assetPath("textures/heart.png"),
            AssetData{.mipmaps = true});
        _heartCookie = _heartAsset->resourceAs<Texture>();
        if (!_heartCookie) {
            spdlog::warn("heart.png failed to load — the spot lights keep a plain beam");
        }

        // The omni cookie: a 'cubemap' asset of six face images.
        AssetData xmasData{.mipmaps = true};
        for (size_t i = 0; i < xmasFaceFiles.size(); ++i) {
            xmasData.faces[i] = assetPath("cubemaps/xmas_faces/" + std::string(xmasFaceFiles[i]) + ".png");
        }
        _xmasAsset = std::make_unique<Asset>("xmas_cubemap", AssetType::CUBEMAP, "", xmasData);
        _xmasCookie = _xmasAsset->resourceAs<Texture>();
        if (!_xmasCookie) {
            spdlog::warn("xmas cubemap failed to build — the omni lights keep a plain falloff");
        }

        // -----------------------------------------------------------------------
        // Statue.
        // -----------------------------------------------------------------------
        _statueAsset = std::make_unique<Asset>(
            "statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        ContainerResource* statueContainer = _statueAsset->resourceAs<ContainerResource>();
        if (!statueContainer) {
            spdlog::error("statue.glb failed to load");
            return false;
        }
        auto* statue = statueContainer->instantiateRenderEntity();
        if (!statue) {
            spdlog::error("statue.glb instantiate failed");
            return false;
        }
        statue->setEngine(engine());
        root()->addChild(statue);

        // -----------------------------------------------------------------------
        // Camera at (0, 15, 35), orbiting the centre of the statue's bounds: the orbit
        // pivots on whatever is renderable when it starts, which is the statue alone,
        // since the ground and the lights are added after the camera.
        // -----------------------------------------------------------------------
        _camera = createCamera(Vector3(0.0f, 15.0f, 35.0f));
        if (auto* cameraComp = _camera->findComponent<CameraComponent>();
            cameraComp && cameraComp->camera()) {
            cameraComp->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        }

        _controls = addOrbitControls(_camera, kStatueCenter);
        _controls->setZoomRange(Vector2(1.0f, 500.0f));
        _controls->storeResetState();

        // -----------------------------------------------------------------------
        // Ground.
        // -----------------------------------------------------------------------
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setName("ground");
        _groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.5f, 1.0f));
        _groundMaterial->setAmbient(Color(0.5f, 0.5f, 0.5f, 1.0f));
        _groundMaterial->setGloss(0.5f);
        _groundMaterial->setMetalness(0.5f);
        _groundMaterial->setUseMetalness(true);

        auto* ground = createPrimitive("box", _groundMaterial.get(), Vector3(0.0f, -0.5f, 0.0f),
            Vector3(70.0f, 1.0f, 70.0f));
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setCastShadows(true);
            render->setReceiveShadows(true);
        }

        // -----------------------------------------------------------------------
        // Light shapes.
        // -----------------------------------------------------------------------
        _wire = std::make_unique<WideLineRenderer>(engine(), device());
        _wire->setLayers({LAYERID_IMMEDIATE});

        // Start with one light of each type
        addLight(Spot);
        addLight(Omni);
        addLight(Directional);

        spdlog::info("Keys: 1/2/3 toggle omni/spot/directional | Z/X/C select a type | =/- add/remove");
        spdlog::info("      I/K intensity | O/L shadow intensity | P/; cookie intensity | G light shapes");
        spdlog::info("      R reset | Esc quit | LMB/RMB orbit, Shift/MMB pan, Wheel zoom");

        return true;
    }

    void update(const float dt) override
    {
        handleKeys();

        // The lights of a type share a turn round the statue equally, and the directional
        // lights differ in elevation too, so however many there are, their light and
        // shadows are told apart.
        _angleRad += 0.3f * dt;
        constexpr float turn = 2.0f * std::numbers::pi_v<float>;

        const auto& spots = _lights[Spot];
        for (size_t i = 0; i < spots.size(); ++i) {
            const float angle = _angleRad + static_cast<float>(i) / static_cast<float>(spots.size()) * turn;
            // Aim, then roll the node so its -Y (the emission axis) points down the view
            // direction, then move it: the aim trails the position by one frame.
            spots[i].entity->lookAt(Vector3(0.0f, -5.0f, 0.0f));
            spots[i].entity->rotateLocal(90.0f, 0.0f, 0.0f);
            spots[i].entity->setLocalPosition(15.0f * std::sin(angle), 25.0f, 15.0f * std::cos(angle));
        }

        const auto& omnis = _lights[Omni];
        for (size_t i = 0; i < omnis.size(); ++i) {
            const float angle = -2.0f * _angleRad + static_cast<float>(i) / static_cast<float>(omnis.size()) * turn;
            omnis[i].entity->setLocalPosition(5.0f * std::sin(angle), 10.0f, 5.0f * std::cos(angle));
            omnis[i].entity->rotate(0.0f, 50.0f * dt, 0.0f);
        }

        const auto& directionals = _lights[Directional];
        for (size_t i = 0; i < directionals.size(); ++i) {
            const float yaw = -60.0f * _angleRad
                + static_cast<float>(i) / static_cast<float>(directionals.size()) * 360.0f;
            const float pitch = 45.0f + static_cast<float>(static_cast<int>(i % 3) - 1) * 12.0f;
            directionals[i].entity->setLocalEulerAngles(pitch, yaw, 0.0f);
        }
    }

    // The shapes are built here, after the camera controls have moved the camera this
    // frame, so the near-plane clip uses the view that renders.
    void preRender() override
    {
        updateLightShapes();
    }

    void destroy() override
    {
        // The line renderer owns an entity under the engine root; release it while the
        // engine is alive.
        _wire.reset();
        spdlog::info("*** Lights Example Finished ***");
    }

private:
    struct LightEntry
    {
        Entity* entity = nullptr;
        LightComponent* light = nullptr;
        std::shared_ptr<StandardMaterial> markerMaterial;
    };

    // The settings of a type apply to every light of that type.
    struct TypeSettings
    {
        bool enabled = true;
        float intensity = 0.8f;
        float cookieIntensity = 1.0f;
        float shadowIntensity = 1.0f;
    };

    struct Segment
    {
        Vector3 a;
        Vector3 b;
        Color color;
    };

    // A light of the given type. Its index picks its colour and, in the update loop,
    // where it sits or points.
    void addLight(const LightKind kind)
    {
        auto& list = _lights[kind];
        const size_t index = list.size();
        const Color color = palettes[kind][index % palettes[kind].size()];
        const TypeSettings& settings = _settings[kind];

        LightEntry entry;
        entry.entity = new Entity();
        entry.entity->setName(std::string(kindNames[kind]) + " " + std::to_string(index));
        entry.entity->setEngine(engine());
        entry.light = static_cast<LightComponent*>(entry.entity->addComponent<LightComponent>());
        LightComponent* light = entry.light;

        switch (kind) {
        case Spot: {
            light->setType(LightType::LIGHTTYPE_SPOT);
            light->setColor(color);
            light->setInnerConeAngle(30.0f);
            light->setOuterConeAngle(31.0f);
            light->setRange(100.0f);
            light->setCastShadows(true);
            light->setShadowBias(0.05f);
            light->setShadowNormalBias(0.03f);
            light->setShadowResolution(2048);
            light->setCookie(_heartCookie);
            light->setCookieChannel(CookieChannel::COOKIE_CHANNEL_A);
            light->setCookieIntensity(settings.cookieIntensity);

            // Emissive cone marking the light itself.
            entry.markerMaterial = std::make_shared<StandardMaterial>();
            entry.markerMaterial->setName("spot-marker");
            entry.markerMaterial->setEmissive(color);
            auto* cone = new Entity();
            cone->setEngine(engine());
            if (auto* render = static_cast<RenderComponent*>(cone->addComponent<RenderComponent>())) {
                render->setType("cone");
                render->setMaterial(entry.markerMaterial.get());
                render->setCastShadows(false);
            }
            entry.entity->addChild(cone);
            break;
        }

        case Omni: {
            light->setType(LightType::LIGHTTYPE_OMNI);
            light->setColor(color);
            light->setCastShadows(true);
            light->setShadowBias(0.05f);
            light->setShadowNormalBias(0.03f);
            light->setShadowType(SHADOW_PCF3_32F);
            light->setShadowResolution(256);
            light->setRange(111.0f);
            light->setCookie(_xmasCookie);
            light->setCookieChannel(CookieChannel::COOKIE_CHANNEL_RGB);
            light->setCookieIntensity(settings.cookieIntensity);

            // The marker sphere sits on the light entity itself.
            entry.markerMaterial = std::make_shared<StandardMaterial>();
            entry.markerMaterial->setName("omni-marker");
            entry.markerMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
            entry.markerMaterial->setEmissive(color);
            if (auto* render = static_cast<RenderComponent*>(entry.entity->addComponent<RenderComponent>())) {
                render->setType("sphere");
                render->setMaterial(entry.markerMaterial.get());
                render->setCastShadows(false);
            }
            break;
        }

        case Directional:
            light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            light->setColor(color);
            light->setRange(100.0f);
            light->setShadowDistance(300.0f);
            light->setNumCascades(2);
            light->setShadowResolution(2048);
            light->setCastShadows(true);
            light->setShadowBias(0.1f);
            light->setShadowNormalBias(0.2f);
            // A directional light shines from everywhere, so its position only places the
            // arrow showing its direction: above the statue, out of the way of the shadows.
            entry.entity->setLocalPosition(0.0f, 22.0f, 0.0f);
            break;
        }

        light->setIntensity(settings.intensity);
        light->setShadowIntensity(settings.shadowIntensity);
        // The type's 'enabled' setting reaches a new light through its component; toggling
        // it later switches the light entities, markers included.
        light->setEnabled(settings.enabled);

        root()->addChild(entry.entity);
        list.push_back(std::move(entry));
        spdlog::info("{} lights: {}", kindNames[kind], list.size());
    }

    void removeLight(const LightKind kind)
    {
        auto& list = _lights[kind];
        if (list.empty()) {
            return;
        }
        Entity* entity = list.back().entity;
        list.pop_back();
        // The parent owns the node: destroy tears the components down, remove frees it.
        entity->destroy();
        auto removed = entity->remove();
        spdlog::info("{} lights: {}", kindNames[kind], list.size());
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }

        // Toggle the lights of a type
        if (keyboard->wasPressed(Key::Digit1)) toggleType(Omni);
        if (keyboard->wasPressed(Key::Digit2)) toggleType(Spot);
        if (keyboard->wasPressed(Key::Digit3)) toggleType(Directional);

        if (keyboard->wasPressed(Key::Z)) selectType(Omni);
        if (keyboard->wasPressed(Key::X)) selectType(Spot);
        if (keyboard->wasPressed(Key::C)) selectType(Directional);

        if (keyboard->wasPressed(Key::Equals)) addLight(_selected);
        if (keyboard->wasPressed(Key::Minus)) removeLight(_selected);

        TypeSettings& settings = _settings[_selected];
        if (keyboard->wasPressed(Key::I)) adjust(settings.intensity, 0.1f, "intensity");
        if (keyboard->wasPressed(Key::K)) adjust(settings.intensity, -0.1f, "intensity");
        if (keyboard->wasPressed(Key::O)) adjust(settings.shadowIntensity, 0.1f, "shadow intensity");
        if (keyboard->wasPressed(Key::L)) adjust(settings.shadowIntensity, -0.1f, "shadow intensity");
        if (_selected != Directional) {
            if (keyboard->wasPressed(Key::P)) adjust(settings.cookieIntensity, 0.1f, "cookie intensity");
            if (keyboard->wasPressed(Key::Semicolon)) adjust(settings.cookieIntensity, -0.1f, "cookie intensity");
        }

        if (keyboard->wasPressed(Key::G)) {
            _showLightShapes = !_showLightShapes;
            spdlog::info("light shapes: {}", _showLightShapes ? "on" : "off");
        }
    }

    void toggleType(const LightKind kind)
    {
        TypeSettings& settings = _settings[kind];
        settings.enabled = !settings.enabled;
        for (const LightEntry& entry : _lights[kind]) {
            entry.entity->setEnabled(settings.enabled);
        }
        spdlog::info("{} lights: {}", kindNames[kind], settings.enabled ? "ON" : "OFF");
    }

    void selectType(const LightKind kind)
    {
        _selected = kind;
        const TypeSettings& s = _settings[kind];
        spdlog::info("selected {}: intensity {:.1f}, shadow intensity {:.1f}, cookie {:.1f}, count {}",
            kindNames[kind], s.intensity, s.shadowIntensity, s.cookieIntensity, _lights[kind].size());
    }

    // Steps one setting of the selected type within 0..1, the range of the sliders these
    // keys stand in for, and applies it to every light of that type.
    void adjust(float& value, const float step, const char* name)
    {
        value = std::clamp(std::round((value + step) * 10.0f) / 10.0f, 0.0f, 1.0f);
        const TypeSettings& settings = _settings[_selected];
        for (const LightEntry& entry : _lights[_selected]) {
            entry.light->setIntensity(settings.intensity);
            entry.light->setShadowIntensity(settings.shadowIntensity);
            if (_selected != Directional) {
                entry.light->setCookieIntensity(settings.cookieIntensity);
            }
        }
        spdlog::info("{} {}: {:.1f}", kindNames[_selected], name, value);
    }

    // -----------------------------------------------------------------------
    // Light shapes: a range sphere for an omni, the cone for a spot, an arrow
    // along the direction for a directional light, each in the light's colour.
    // -----------------------------------------------------------------------

    // An orthonormal pair perpendicular to `dir` (normalized), from the world axis least
    // aligned with it.
    static void buildBasis(const Vector3& dir, Vector3& u, Vector3& v)
    {
        u = std::abs(dir.getX()) < 0.5f ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 1.0f, 0.0f);
        v = dir.cross(u).normalized();
        u = v.cross(dir).normalized();
    }

    void addArc(const Vector3& center, const Vector3& u, const Vector3& v, const float radius, const Color& color)
    {
        constexpr float step = 2.0f * std::numbers::pi_v<float> / static_cast<float>(kCircleSteps);
        Vector3 previous = center + u * radius;
        for (int i = 1; i <= kCircleSteps; ++i) {
            const float angle = step * static_cast<float>(i);
            const Vector3 next = center + u * (std::cos(angle) * radius) + v * (std::sin(angle) * radius);
            _segments.push_back({previous, next, color});
            previous = next;
        }
    }

    void addSphere(const Vector3& center, const float radius, const Color& color)
    {
        const Vector3 x(1.0f, 0.0f, 0.0f);
        const Vector3 y(0.0f, 1.0f, 0.0f);
        const Vector3 z(0.0f, 0.0f, 1.0f);
        addArc(center, x, y, radius, color);
        addArc(center, x, z, radius, color);
        addArc(center, y, z, radius, color);
    }

    // `angle` is the cone's HALF-angle in degrees, `length` the apex-to-base distance.
    void addCone(const Vector3& apex, const Vector3& direction, const float angle, const float length,
        const Color& color)
    {
        Vector3 u;
        Vector3 v;
        buildBasis(direction, u, v);
        const float radius = length * std::tan(std::min(angle, 89.9f) * kDegToRad);
        const Vector3 base = apex + direction * length;
        addArc(base, u, v, radius, color);
        _segments.push_back({apex, base + u * radius, color});
        _segments.push_back({apex, base + v * radius, color});
        _segments.push_back({apex, base - u * radius, color});
        _segments.push_back({apex, base - v * radius, color});
    }

    void addArrow(const Vector3& from, const Vector3& to, const Color& color)
    {
        const Vector3 delta = to - from;
        const float length = delta.length();
        if (length < 1e-6f) {
            return;
        }
        const Vector3 dir = delta * (1.0f / length);
        Vector3 u;
        Vector3 v;
        buildBasis(dir, u, v);
        _segments.push_back({from, to, color});

        // The head is a small cone, which still reads as an arrow from a distance.
        const float headLength = length * 0.2f;
        const float headRadius = headLength * 0.5f;
        const Vector3 head = to - dir * headLength;
        addArc(head, u, v, headRadius, color);
        _segments.push_back({to, head + u * headRadius, color});
        _segments.push_back({to, head + v * headRadius, color});
        _segments.push_back({to, head - u * headRadius, color});
        _segments.push_back({to, head - v * headRadius, color});
    }

    void updateLightShapes()
    {
        if (!_wire) {
            return;
        }

        _segments.clear();
        if (_showLightShapes) {
            for (const LightKind kind : {Omni, Spot, Directional}) {
                for (const LightEntry& entry : _lights[kind]) {
                    if (!entry.entity->enabled()) {
                        continue;
                    }
                    const Vector3 position = entry.entity->position();
                    // A light shines down the negative Y axis of its entity.
                    const Vector3 direction =
                        (Vector3(entry.entity->worldTransform().getColumn(1)) * -1.0f).normalized();
                    const Color& color = entry.light->color();
                    switch (kind) {
                    case Omni:
                        addSphere(position, entry.light->range(), color);
                        break;
                    case Spot:
                        addCone(position, direction, entry.light->outerConeAngle(), entry.light->range(), color);
                        break;
                    case Directional:
                        addArrow(position, position + direction * 8.0f, color);
                        break;
                    }
                }
            }
        }

        const auto [width, height] = device()->size();
        _wire->setScreenSize(static_cast<float>(width), static_cast<float>(height));

        // A wide line is expanded in screen space, so a point behind the camera would fold
        // the segment back across the screen. Clip each to just in front of the near plane.
        const Vector3 cameraPos = _camera->position();
        const Vector3 forward = (Vector3(_camera->worldTransform().getColumn(2)) * -1.0f).normalized();
        float nearClip = 0.0f;
        if (auto* cameraComp = _camera->findComponent<CameraComponent>(); cameraComp && cameraComp->camera()) {
            nearClip = cameraComp->camera()->nearClip() * 1.01f;
        }

        size_t count = 0;
        for (const Segment& segment : _segments) {
            Vector3 a = segment.a;
            Vector3 b = segment.b;
            const float da = (a - cameraPos).dot(forward) - nearClip;
            const float db = (b - cameraPos).dot(forward) - nearClip;
            if (da < 0.0f && db < 0.0f) {
                continue;
            }
            if (da < 0.0f) {
                a = a + (b - a) * (da / (da - db));
            } else if (db < 0.0f) {
                b = b + (a - b) * (db / (db - da));
            }

            if (count >= _wireLines.size()) {
                _wireLines.emplace_back();
            }
            WideLine& line = _wireLines[count];
            line.setPoints({a, b}, segment.color, 1.0f);
            if (count >= _wireShown) {
                _wire->add(&line);
            }
            ++count;
        }
        for (size_t i = count; i < _wireShown; ++i) {
            _wire->remove(&_wireLines[i]);
        }
        _wireShown = count;
        _wire->update();
    }

    std::unique_ptr<Asset> _statueAsset;
    std::unique_ptr<Asset> _heartAsset;
    std::unique_ptr<Asset> _xmasAsset;
    Texture* _heartCookie = nullptr;
    Texture* _xmasCookie = nullptr;

    std::shared_ptr<StandardMaterial> _groundMaterial;

    Entity* _camera = nullptr;
    CameraControls* _controls = nullptr;

    std::array<std::vector<LightEntry>, 3> _lights;
    std::array<TypeSettings, 3> _settings{};
    LightKind _selected = Omni;

    std::unique_ptr<WideLineRenderer> _wire;
    // A deque, so the lines the renderer holds pointers to never move as it grows.
    std::deque<WideLine> _wireLines;
    size_t _wireShown = 0;
    std::vector<Segment> _segments;
    bool _showLightShapes = true;

    const Vector3 kStatueCenter{0.173f, 7.523f, 0.018f};
    float _angleRad = 1.0f;
};

VISUTWIN_EXAMPLE_MAIN(LightsExample)
