// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream debug/lines.
//
// The WireRenderer's line forms at scale. A 40 x 40 wave field is one linesPacked submission
// a frame (3,120 segments, coloured trough to crest); six orbiting markers are tied together by
// a loop, spokes fading to the hub (lines with a colour per point) and a mast (line); ten comets
// weave through them, each trailing a 200-point polyline whose alpha fades along it. Trails are
// sampled on a fixed time step, so they cover the same span of motion at any frame rate, and are
// seeded from the path already travelled so they are whole on the first frame. The markers and
// comets are emissive, so the scene needs no lights.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/extras/wireRenderer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kGrid = 40;
    constexpr float kStep = 1.5f;
    constexpr float kOrigin = -((kGrid - 1) * kStep) / 2.0f;
    // A grid of kGrid x kGrid vertices has this many edges
    constexpr int kSegments = 2 * kGrid * (kGrid - 1);

    constexpr int kMarkers = 6;
    constexpr float kOrbitRadius = 15.0f;
    const Vector3 kHub(0.0f, 17.0f, 0.0f);

    constexpr int kComets = 10;
    constexpr int kTrail = 200;
    constexpr float kTrailInterval = 0.02f;
    constexpr float kPi = 3.14159265358979f;

    // Height ramp, from a deep trough through the mid tone to a warm crest
    constexpr std::array<float, 3> kTrough = {0.03f, 0.09f, 0.28f};
    constexpr std::array<float, 3> kMid = {0.08f, 0.55f, 0.66f};
    constexpr std::array<float, 3> kCrest = {1.0f, 0.84f, 0.5f};

    float elevation(const float x, const float z, const float time)
    {
        const float radius = std::hypot(x, z);
        return std::sin(x * 0.22f + time) * 0.8f + std::cos(z * 0.19f - time * 0.7f) * 0.8f +
            std::sin(radius * 0.45f - time * 2.2f) * 2.4f / (1.0f + radius * 0.14f);
    }

    Vector3 markerPosition(const int index, const float t)
    {
        const float offset = static_cast<float>(index) * kPi * 2.0f / kMarkers;
        return Vector3(std::sin(t * 0.25f + offset) * kOrbitRadius, 10.0f + std::sin(t * 1.2f + offset * 2.0f) * 2.5f,
            std::cos(t * 0.25f + offset) * kOrbitRadius);
    }

    struct Comet
    {
        Entity* entity = nullptr;
        float fx, fy, fz, ax, ay, az, yc, phase;
        std::vector<Vector3> store;     // ring buffer of past positions
        std::vector<Vector3> points;    // ordered copy handed to polyline
        std::vector<Color> colors;      // fixed alpha ramp along the trail
        int head = 0;

        Vector3 position(const float t) const
        {
            return Vector3(std::sin(t * fx + phase) * ax, yc + std::sin(t * fy + phase * 1.7f) * ay,
                std::cos(t * fz + phase) * az);
        }
    };
}

class LinesExample final : public ExampleApp
{
public:
    LinesExample() : ExampleApp({.title = "Lines"}) {}

protected:
    bool create() override
    {
        auto* camera = createCamera(Vector3(33.0f, 41.0f, 33.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.015f, 0.02f, 0.03f, 1.0f));
            component->camera()->setFarClip(400.0f);
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 7.0f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-80.0f, 80.0f));
            controls->setZoomRange(Vector2(12.0f, 140.0f));
            controls->storeResetState();
        }

        _wire.emplace(scene().get());
        _gridPositions.resize(static_cast<size_t>(kSegments) * 6);
        _gridColors.resize(static_cast<size_t>(kSegments) * 8);
        _heights.resize(static_cast<size_t>(kGrid) * kGrid);

        // The orbiting markers: emissive only, so they read against the wires unlit
        const Color markerColor(0.55f, 0.62f, 0.75f, 1.0f);
        auto markerMaterial = material(markerColor, 1.5f);
        for (int i = 0; i < kMarkers; ++i) {
            _markers.push_back(createPrimitive(i % 2 ? "sphere" : "box", markerMaterial.get(), Vector3(0.0f),
                Vector3(0.85f, 0.85f, 0.85f)));
            // Each spoke runs from its marker to the hub, fading out as it goes
            _spokeColors.push_back(markerColor);
            _spokeColors.push_back(Color(markerColor.r, markerColor.g, markerColor.b, 0.0f));
        }
        _ringPoints.resize(kMarkers);
        _spokePoints.resize(kMarkers * 2);

        // The comets, each on a path of its own: frequencies that do not divide evenly, and
        // amplitudes that keep them on different shells
        for (int i = 0; i < kComets; ++i) {
            const float hue = static_cast<float>(i) / kComets;
            const Color color(0.5f + 0.5f * std::sin(hue * kPi * 2.0f), 0.5f + 0.5f * std::sin(hue * kPi * 2.0f + 2.1f),
                0.5f + 0.5f * std::sin(hue * kPi * 2.0f + 4.2f), 1.0f);
            Comet comet;
            comet.entity = createPrimitive("sphere", material(color, 4.0f).get(), Vector3(0.0f), Vector3(1.1f, 1.1f, 1.1f));
            comet.fx = 0.34f + static_cast<float>(i) * 0.058f;
            comet.fy = 0.7f + static_cast<float>(i % 3) * 0.21f;
            comet.fz = 0.25f + static_cast<float>((i * 7) % 10) * 0.037f;
            comet.ax = 12.0f + static_cast<float>(i % 4) * 4.0f;
            comet.ay = 2.8f + static_cast<float>(i % 3) * 1.7f;
            comet.az = 12.0f + static_cast<float>((i + 2) % 4) * 4.0f;
            comet.yc = 6.5f + static_cast<float>(i % 3) * 2.2f;
            comet.phase = static_cast<float>(i) * 0.63f;
            comet.store.resize(kTrail);
            comet.points.resize(kTrail);
            for (int t = 0; t < kTrail; ++t) {
                // Squared, so the head stays bright while the tail disappears
                const float k = static_cast<float>(t) / (kTrail - 1);
                comet.colors.emplace_back(color.r, color.g, color.b, k * k);
            }
            _comets.push_back(std::move(comet));
        }
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // The wave field, one packed submission
        for (int ix = 0; ix < kGrid; ++ix) {
            const float x = kOrigin + static_cast<float>(ix) * kStep;
            for (int iz = 0; iz < kGrid; ++iz) {
                _heights[static_cast<size_t>(ix * kGrid + iz)] = elevation(x, kOrigin + static_cast<float>(iz) * kStep, _time);
            }
        }
        int vertex = 0;
        for (int ix = 0; ix < kGrid; ++ix) {
            for (int iz = 0; iz < kGrid; ++iz) {
                if (ix > 0) {
                    writeVertex(vertex++, ix - 1, iz);
                    writeVertex(vertex++, ix, iz);
                }
                if (iz > 0) {
                    writeVertex(vertex++, ix, iz - 1);
                    writeVertex(vertex++, ix, iz);
                }
            }
        }
        _wire->linesPacked(_gridPositions, _gridColors);

        // The markers, and the shapes tying them together
        for (int i = 0; i < kMarkers; ++i) {
            Entity* entity = _markers[i];
            entity->setLocalPosition(markerPosition(i, _time));
            entity->rotate(18.0f * dt, 30.0f * dt, 42.0f * dt);
            const Vector3 position = entity->position();
            _ringPoints[i] = position;
            _spokePoints[i * 2] = position;
            _spokePoints[i * 2 + 1] = kHub;
        }
        // The ring is closed, so a loop rather than a line per pair
        _wire->setColor(Color(0.95f, 0.35f, 0.9f, 1.0f));
        _wire->loop(_ringPoints);
        _wire->lines(_spokePoints, _spokeColors);
        _wire->setColor(Color(0.5f, 0.55f, 0.65f, 1.0f));
        _wire->line(Vector3(0.0f, -3.5f, 0.0f), kHub);

        // The comets and their trails
        for (Comet& comet : _comets) {
            comet.entity->setLocalPosition(comet.position(_time));
        }
        if (!_trailSeeded) {
            for (Comet& comet : _comets) {
                for (int t = 0; t < kTrail; ++t) {
                    comet.store[t] = comet.position(_time - static_cast<float>(kTrail - 1 - t) * kTrailInterval);
                }
            }
            _trailSeeded = true;
        }
        _trailTimer += dt;
        int steps = static_cast<int>(std::floor(_trailTimer / kTrailInterval));
        _trailTimer -= static_cast<float>(steps) * kTrailInterval;
        // After a long stall there is no point replacing the buffer more than once over
        steps = std::min(steps, kTrail);
        for (int s = steps - 1; s >= 0; --s) {
            const float sampleTime = _time - _trailTimer - static_cast<float>(s) * kTrailInterval;
            for (Comet& comet : _comets) {
                comet.store[comet.head] = comet.position(sampleTime);
                comet.head = (comet.head + 1) % kTrail;
            }
        }
        for (Comet& comet : _comets) {
            // Oldest first
            for (int t = 0; t < kTrail; ++t) {
                comet.points[t] = comet.store[(comet.head + t) % kTrail];
            }
            _wire->polyline(comet.points, comet.colors);
        }
    }

private:
    std::shared_ptr<StandardMaterial> material(const Color& emissive, const float intensity)
    {
        auto m = std::make_shared<StandardMaterial>();
        m->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        m->setEmissive(emissive);
        m->setEmissiveIntensity(intensity);
        _materials.push_back(m);
        return m;
    }

    void writeVertex(const int vertex, const int ix, const int iz)
    {
        const float height = _heights[static_cast<size_t>(ix * kGrid + iz)];
        const size_t p = static_cast<size_t>(vertex) * 3;
        _gridPositions[p] = kOrigin + static_cast<float>(ix) * kStep;
        _gridPositions[p + 1] = height;
        _gridPositions[p + 2] = kOrigin + static_cast<float>(iz) * kStep;

        // 0 at the deepest trough, 1 at the highest crest
        const float t = std::clamp((height + 2.5f) / 5.5f, 0.0f, 1.0f);
        const bool lower = t < 0.5f;
        const auto& from = lower ? kTrough : kMid;
        const auto& to = lower ? kMid : kCrest;
        const float k = lower ? t * 2.0f : (t - 0.5f) * 2.0f;
        const size_t c = static_cast<size_t>(vertex) * 4;
        for (int i = 0; i < 3; ++i) {
            _gridColors[c + i] = from[i] + (to[i] - from[i]) * k;
        }
        _gridColors[c + 3] = 1.0f;
    }

    std::optional<WireRenderer> _wire;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<float> _gridPositions;
    std::vector<float> _gridColors;
    std::vector<float> _heights;
    std::vector<Entity*> _markers;
    std::vector<Vector3> _ringPoints;
    std::vector<Vector3> _spokePoints;
    std::vector<Color> _spokeColors;
    std::vector<Comet> _comets;
    bool _trailSeeded = false;
    float _trailTimer = 0.0f;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(LinesExample)
