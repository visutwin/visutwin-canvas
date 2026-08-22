// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Reflection probe demo: a chrome sphere + polished floor inside a colored
// "room" cubemap. The probe is box-projected against the room volume, so the
// floor reflects the colored walls parallax-correctly. Auto-toggles box
// projection ON/OFF every 3 s — with box OFF the cubemap acts like an infinite
// environment (direction-only), so the flat floor reflects a nearly uniform
// color; with box ON each floor point reflects the wall its ray actually hits.
// Esc quits.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// Build a colored-room cubemap: each face a distinct wall color with a bright
// centered marker + grid so the parallax shift of reflections is visible. Full
// mip chain (box-downsampled per face) so roughness maps to a mip LOD.
std::shared_ptr<Texture> makeRoomCubemap(GraphicsDevice* device, const int size)
{
    // Base color + marker color per face (0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z).
    const float wall[6][3] = {
        {0.85f, 0.15f, 0.15f},  // +X red
        {0.15f, 0.75f, 0.25f},  // -X green
        {0.9f, 0.9f, 0.95f},    // +Y ceiling (light)
        {0.1f, 0.1f, 0.12f},    // -Y floor (dark)
        {0.2f, 0.4f, 0.9f},     // +Z blue
        {0.9f, 0.8f, 0.2f},     // -Z yellow
    };

    int levels = 1;
    while ((size >> levels) >= 1) ++levels;

    TextureOptions options;
    options.name = "roomCubemap";
    options.width = static_cast<uint32_t>(size);
    options.height = static_cast<uint32_t>(size);
    options.format = PixelFormat::PIXELFORMAT_RGBA8;
    options.cubemap = true;
    options.mipmaps = true;
    options.numLevels = static_cast<uint32_t>(levels);
    options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
    options.magFilter = FilterMode::FILTER_LINEAR;
    auto texture = std::make_shared<Texture>(device, options);

    const auto toByte = [](const float v) {
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };

    for (int face = 0; face < 6; ++face) {
        // Level 0: base wall color, darker grid lines, bright centered disc.
        std::vector<uint8_t> level0(static_cast<size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float u = (x + 0.5f) / size - 0.5f;
                const float v = (y + 0.5f) / size - 0.5f;
                float r = wall[face][0], g = wall[face][1], b = wall[face][2];
                // Grid lines.
                if (((x * 8 / size) % 2) ^ ((y * 8 / size) % 2)) {
                    r *= 0.7f; g *= 0.7f; b *= 0.7f;
                }
                // Bright centered disc marker.
                if (std::sqrt(u * u + v * v) < 0.18f) {
                    r = std::min(1.0f, r + 0.6f);
                    g = std::min(1.0f, g + 0.6f);
                    b = std::min(1.0f, b + 0.6f);
                }
                uint8_t* px = &level0[(static_cast<size_t>(y) * size + x) * 4];
                px[0] = toByte(r); px[1] = toByte(g); px[2] = toByte(b); px[3] = 255;
            }
        }
        texture->setLevelData(0, level0.data(), level0.size(), static_cast<uint32_t>(face));

        // Downsample successive mips (box filter, per-face).
        std::vector<uint8_t> prev = std::move(level0);
        int prevSize = size;
        for (int level = 1; level < levels; ++level) {
            const int mipSize = std::max(1, prevSize / 2);
            std::vector<uint8_t> mip(static_cast<size_t>(mipSize) * mipSize * 4);
            for (int y = 0; y < mipSize; ++y) {
                for (int x = 0; x < mipSize; ++x) {
                    int acc[4] = {0, 0, 0, 0};
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(prevSize - 1, x * 2 + dx);
                            const int sy = std::min(prevSize - 1, y * 2 + dy);
                            const uint8_t* s = &prev[(static_cast<size_t>(sy) * prevSize + sx) * 4];
                            for (int c = 0; c < 4; ++c) acc[c] += s[c];
                        }
                    }
                    uint8_t* d = &mip[(static_cast<size_t>(y) * mipSize + x) * 4];
                    for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(acc[c] / 4);
                }
            }
            texture->setLevelData(static_cast<uint32_t>(level), mip.data(), mip.size(),
                static_cast<uint32_t>(face));
            prev = std::move(mip);
            prevSize = mipSize;
        }
    }

    texture->upload();
    return texture;
}

class ReflectionProbeExample final: public ExampleApp
{
public:
    ReflectionProbeExample(): ExampleApp({.title = "Reflection Probe"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.18f, 0.18f, 0.2f);

        _roomCube = makeRoomCubemap(device().get(), 256);
        applyProbe();

        _camera = createCamera(Vector3(0.0f, 3.2f, 9.0f), Vector3(-14.0f, 0.0f, 0.0f));
        if (auto* cameraComponent = _camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.08f, 0.08f, 0.1f, 1.0f));
            cameraComponent->camera()->setFov(55.0f);
        }

        createDirectionalLight(Vector3(50.0f, -30.0f, 0.0f));

        // Polished floor (the parallax money shot).
        _floorMaterial = std::make_shared<StandardMaterial>();
        _floorMaterial->setDiffuse(Color(0.15f, 0.15f, 0.18f, 1.0f));
        _floorMaterial->setMetalness(0.9f);
        _floorMaterial->setGlossInvert(true);
        _floorMaterial->setGloss(0.08f);   // low roughness → sharp reflections
        createPrimitive("plane", _floorMaterial.get(), Vector3(0.0f, -1.0f, 0.0f),
            Vector3(12.0f, 1.0f, 12.0f));

        // Chrome sphere.
        _chromeMaterial = std::make_shared<StandardMaterial>();
        _chromeMaterial->setDiffuse(Color(0.95f, 0.95f, 0.95f, 1.0f));
        _chromeMaterial->setMetalness(1.0f);
        _chromeMaterial->setGlossInvert(true);
        _chromeMaterial->setGloss(0.05f);
        createPrimitive("sphere", _chromeMaterial.get(), Vector3(0.0f, 0.6f, 0.0f),
            Vector3(2.4f, 2.4f, 2.4f));

        spdlog::info("Reflection probe: chrome sphere + polished floor in a colored room cubemap.");
        spdlog::info("Auto-toggles box projection ON/OFF every 3 s. Esc quits.");

        return true;
    }

    void update(const float dt) override
    {
        _toggleTimer += dt;
        if (_toggleTimer >= 3.0f) {
            _toggleTimer = 0.0f;
            _boxProjection = !_boxProjection;
            applyProbe();
        }

        // Gentle camera sway so reflections read as 3D.
        const float elapsed = elapsedTime();
        _camera->setLocalPosition(std::sin(elapsed * 0.3f) * 2.5f, 3.2f, 9.0f);
        _camera->setLocalEulerAngles(-14.0f, std::sin(elapsed * 0.3f) * 10.0f, 0.0f);
    }

private:
    void applyProbe() const
    {
        scene()->setReflectionProbe(_roomCube.get(), Vector3(0.0f, 3.0f, 0.0f),
            _boxMin, _boxMax, _boxProjection, 1.0f);
        spdlog::info("Box projection: {}", _boxProjection ? "ON (parallax-corrected)" : "OFF (infinite env)");
    }

    // Room bounds — the reflection probe's box volume.
    const Vector3 _boxMin{-6.0f, -1.0f, -6.0f};
    const Vector3 _boxMax{6.0f, 8.0f, 6.0f};

    std::shared_ptr<Texture> _roomCube;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::shared_ptr<StandardMaterial> _chromeMaterial;
    Entity* _camera = nullptr;

    bool _boxProjection = true;
    float _toggleTimer = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ReflectionProbeExample)
