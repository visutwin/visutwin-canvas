// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/wide-lines-dynamic.
//
// A globe-scale network of thick, blooming lines with world-space widths, all drawn by one
// WideLineRenderer in a single instanced draw. 48 cities lie on a Fibonacci sphere just
// above a planet of radius 5 (a clearcoated, metallic blue sphere). 144 connections of 28
// points each travel between cities: each draws the stretch from 38% behind its head to the
// head along a great-circle arc lifted off the surface, its width swelling in the middle,
// and on arrival launches towards a new destination with a new arc height, speed and colour
// from a four-colour HDR palette. Every position is rewritten every frame.
//
// The sky is the space panorama turned into a 1024 cubemap, drawn as an infinite sky, with
// ambient (0.16, 0.2, 0.3) and exposure 1.05. A blue key light and a pink rim light shine
// from two sides. The camera orbits the planet from (10.5, 6.5, 13.5) through a camera frame
// (4x MSAA, ACES tone mapping, bloom 0.035 over 8 levels) whose HDR target lets the line
// colours, well above 1, bloom.
//
// The random routes come from upstream's own LCG with its seed, so the network is
// upstream's.
//
// DEVIATION: upstream's space.webp is decoded losslessly to space.png (there is no webp
// loader). Upstream rebuilds the skybox after device loss; there is no device loss here.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/graphics/envLighting.h"
#include "scene/graphics/wideLine.h"
#include "scene/graphics/wideLineRenderer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kSphereRadius = 5.0f;
    constexpr int kCityCount = 48;
    constexpr int kConnectionCount = 144;
    constexpr int kPointCount = 28;
    constexpr double kCityRadius = kSphereRadius + 0.055;

    using Vec3d = std::array<double, 3>;

    double dot(const Vec3d& a, const Vec3d& b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    // An HDR colour per destination, so a line's colour says where it is going
    const std::array<Vec3d, 4> kPalette = {{
        {0.2, 2.8, 5.5},
        {2.2, 0.5, 5.5},
        {5.5, 0.3, 1.8},
        {5.5, 2.3, 0.25},
    }};
}

class WideLinesDynamicExample final: public ExampleApp
{
public:
    WideLinesDynamicExample(): ExampleApp({.title = "Wide Lines Dynamic"}) {}

protected:
    bool create() override
    {
        _sky = std::make_unique<Asset>("sky", AssetType::TEXTURE, assetPath("hdri/space.png"),
            AssetData{.mipmaps = false});
        Texture* skyTexture = _sky->resourceAs<Texture>();
        if (!skyTexture) {
            spdlog::error("Failed to load the space panorama");
            return false;
        }

        // The skybox, generated on the GPU from the panorama
        _skybox.reset(EnvLighting::generateSkyboxCubemap(device().get(), skyTexture, 1024));
        scene()->setSkybox(_skybox.get());
        scene()->setSkyType(SKYTYPE_INFINITE);
        scene()->setAmbientLight(0.16f, 0.2f, 0.3f);
        scene()->setExposure(1.05f);

        // The planet
        _planetMaterial = std::make_shared<StandardMaterial>();
        _planetMaterial->setDiffuse(Color(0.035f, 0.18f, 0.3f, 1.0f));
        _planetMaterial->setEmissive(Color(0.002f, 0.008f, 0.018f, 1.0f));
        _planetMaterial->setMetalness(0.52f);
        _planetMaterial->setUseMetalness(true);
        _planetMaterial->setGloss(0.72f);
        _planetMaterial->setClearCoat(0.25f);
        _planetMaterial->setClearCoatGloss(0.85f);

        auto* planet = createPrimitive("sphere", _planetMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(kSphereRadius * 2.0f, kSphereRadius * 2.0f, kSphereRadius * 2.0f));
        planet->setName("Network Planet");

        // The lights
        auto* keyLight = createDirectionalLight(Vector3(35.0f, 135.0f, 0.0f), Color(0.55f, 0.72f, 1.0f, 1.0f), 1.15f);
        keyLight->setName("Key Light");
        auto* rimLight = createDirectionalLight(Vector3(-25.0f, -55.0f, 0.0f), Color(1.0f, 0.25f, 0.5f, 1.0f), 0.65f);
        rimLight->setName("Rim Light");

        // The camera
        auto* camera = createCamera(Vector3(10.5f, 6.5f, 13.5f));
        camera->setName("Camera");
        camera->lookAt(0.0f, 0.0f, 0.0f);
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        if (!cameraComponent || !cameraComponent->camera()) {
            return false;
        }
        cameraComponent->camera()->setClearColor(Color(0.003f, 0.006f, 0.018f, 1.0f));
        cameraComponent->camera()->setNearClip(0.1f);
        cameraComponent->camera()->setFarClip(100.0f);
        cameraComponent->setToneMapping(TONEMAP_ACES);

        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-85.0f, 85.0f));
            controls->setZoomRange(Vector2(10.0f, 68.0f));
            controls->storeResetState();
        }

        // The camera frame: an RGBA16F scene target, multisampled, with bloom; tone mapping
        // is the camera's ACES
        RenderingSettings rendering = cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.035f;
        rendering.bloomBlurLevel = 8;
        cameraComponent->setRendering(rendering);

        // The cities, on a Fibonacci sphere of unit radius
        const double goldenAngle = std::numbers::pi * (3.0 - std::sqrt(5.0));
        for (int i = 0; i < kCityCount; ++i) {
            const double y = 1.0 - ((i + 0.5) * 2.0) / kCityCount;
            const double radial = std::sqrt(1.0 - y * y);
            const double angle = i * goldenAngle;
            _cities[i] = {std::cos(angle) * radial, y, std::sin(angle) * radial};
        }

        // The lines
        _renderer = std::make_unique<WideLineRenderer>(engine(), device());
        _renderer->setLayers({LAYERID_IMMEDIATE});
        _renderer->setWidthUnits(LineWidthUnits::World);
        syncScreenSize();

        _connections.reserve(kConnectionCount);
        for (int i = 0; i < kConnectionCount; ++i) {
            std::vector<float> positions(kPointCount * 3, 0.0f);
            std::vector<float> colors(kPointCount * 3, 0.0f);
            std::vector<float> widths(kPointCount);
            const double baseWidth = 0.04 + random() * 0.06;

            for (int point = 0; point < kPointCount; ++point) {
                const double t = static_cast<double>(point) / (kPointCount - 1);
                widths[point] = static_cast<float>(baseWidth * (0.25 + std::sin(t * std::numbers::pi) * 0.75));
            }
            widths[0] = 0.0f;

            auto& connection = _connections.emplace_back();
            connection.line = std::make_unique<WideLine>();
            connection.line->setPoints(positions, colors, widths);
            connection.line->setCap(LineCap::Round);
            connection.line->setJoin(LineJoin::Round);
            _renderer->add(connection.line.get());

            connection.positions = std::move(positions);
            connection.colors = std::move(colors);

            const int source = static_cast<int>(std::floor(random() * kCityCount));
            connection.progress = random();
            connection.source = source;
            connection.start = _cities[source];
            connection.end = _cities[source];
            const int destination = chooseDestination(source);
            configureRoute(connection, source, destination);
        }

        // Positions for the first frame
        writeConnections(0.0f);
        _renderer->update();
        return true;
    }

    void update(const float dt) override
    {
        writeConnections(dt);
        syncScreenSize();
        if (_renderer) {
            _renderer->update();
        }
    }

    void destroy() override
    {
        _renderer.reset();
        if (scene()) {
            scene()->setSkybox(nullptr);
        }
    }

private:
    struct Connection
    {
        std::unique_ptr<WideLine> line;
        std::vector<float> positions;
        std::vector<float> colors;
        double progress = 0.0;
        int source = 0;
        int destination = 0;
        Vec3d start{};
        Vec3d end{};
        double angle = 0.0;
        double sinAngle = 0.0;
        double height = 0.0;
        double speed = 0.0;
    };

    // Upstream's LCG, from its seed: the same sequence, so the same routes
    double random()
    {
        _randomState = _randomState * 1664525u + 1013904223u;
        return static_cast<double>(_randomState) / 4294967296.0;
    }

    // Any other city that is not nearly on the far side of the globe
    int chooseDestination(const int source)
    {
        int destination;
        do {
            destination = static_cast<int>(std::floor(random() * kCityCount));
        } while (destination == source || dot(_cities[source], _cities[destination]) < -0.82);
        return destination;
    }

    void configureRoute(Connection& connection, const int source, const int destination)
    {
        connection.source = source;
        connection.destination = destination;
        connection.start = _cities[source];
        connection.end = _cities[destination];
        connection.angle = std::acos(std::max(-1.0, std::min(1.0, dot(connection.start, connection.end))));
        connection.sinAngle = std::sin(connection.angle);
        connection.height = 0.55 + connection.angle * 0.55 + random() * 0.45;
        connection.speed = 0.16 + random() * 0.14;

        // Dim at the tail, full brightness at the head
        const Vec3d& color = kPalette[destination % kPalette.size()];
        for (int point = 0; point < kPointCount; ++point) {
            const double t = static_cast<double>(point) / (kPointCount - 1);
            const double brightness = 0.25 + t * 0.75;
            const int offset = point * 3;
            connection.colors[offset] = static_cast<float>(color[0] * brightness);
            connection.colors[offset + 1] = static_cast<float>(color[1] * brightness);
            connection.colors[offset + 2] = static_cast<float>(color[2] * brightness);
        }
        connection.line->setColors(connection.colors);
    }

    // The point at t along the great circle from start to end, lifted by a sine arch
    static void writeArcPoint(Connection& connection, const int point, const double t)
    {
        double x;
        double y;
        double z;

        if (connection.sinAngle > 0.0001) {
            const double startWeight = std::sin((1.0 - t) * connection.angle) / connection.sinAngle;
            const double endWeight = std::sin(t * connection.angle) / connection.sinAngle;
            x = connection.start[0] * startWeight + connection.end[0] * endWeight;
            y = connection.start[1] * startWeight + connection.end[1] * endWeight;
            z = connection.start[2] * startWeight + connection.end[2] * endWeight;
        } else {
            x = connection.start[0] + (connection.end[0] - connection.start[0]) * t;
            y = connection.start[1] + (connection.end[1] - connection.start[1]) * t;
            z = connection.start[2] + (connection.end[2] - connection.start[2]) * t;
        }

        const double radius = kCityRadius + std::sin(t * std::numbers::pi) * connection.height;
        const int offset = point * 3;
        connection.positions[offset] = static_cast<float>(x * radius);
        connection.positions[offset + 1] = static_cast<float>(y * radius);
        connection.positions[offset + 2] = static_cast<float>(z * radius);
    }

    // Advances every connection by dt and rewrites its points: from 0.38 behind the head to
    // the head, relaunching a connection that arrived towards a new destination
    void writeConnections(const float dt)
    {
        for (auto& connection : _connections) {
            connection.progress += dt * connection.speed;

            if (connection.progress >= 1.0) {
                connection.progress = std::fmod(connection.progress, 1.0);
                const int source = connection.destination;
                const int destination = chooseDestination(source);
                configureRoute(connection, source, destination);
            }

            const double head = connection.progress;
            const double tail = std::max(0.0, head - 0.38);
            for (int point = 0; point < kPointCount; ++point) {
                const double t = tail + ((head - tail) * point) / (kPointCount - 1);
                writeArcPoint(connection, point, t);
            }
            connection.line->setPositions(connection.positions);
        }
    }

    // The renderer is told the back buffer size only when it changes, since telling it
    // forces a rebuild.
    void syncScreenSize()
    {
        if (!_renderer || !device()) {
            return;
        }
        const auto size = device()->size();
        if (size != _screenSize) {
            _screenSize = size;
            _renderer->setScreenSize(static_cast<float>(size.first), static_cast<float>(size.second));
        }
    }

    std::unique_ptr<Asset> _sky;
    std::unique_ptr<Texture> _skybox;
    std::shared_ptr<StandardMaterial> _planetMaterial;

    std::unique_ptr<WideLineRenderer> _renderer;
    std::array<Vec3d, kCityCount> _cities{};
    std::vector<Connection> _connections;
    uint32_t _randomState = 0x5f3759dfu;
    std::pair<int, int> _screenSize{0, 0};
};

VISUTWIN_EXAMPLE_MAIN(WideLinesDynamicExample)
