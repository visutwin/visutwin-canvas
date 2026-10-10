// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream test/material-texture-transforms.
//
// A visual test of per-map texture transforms on StandardMaterial. Three procedural
// 256x256 textures share one asymmetric arrow (point at the top, shaft at the bottom):
// the diffuse texture colours the arrow in four quadrants (red, green, blue, yellow)
// on a near-black ground, strokes its outline 14 px wide and labels it "UV"; the opacity
// texture is the arrow in white on black, sampled through its red channel; the
// emissive texture is a dim arrow with a white dot near its tip. Four double-sided
// vertical cards stand on a grey ground plane: the diffuse source, the opacity source
// (alpha test 0.5), the emissive source (black diffuse, emissive intensity 2) and the
// combined output, which carries all three maps. Every map has the same tiling
// (0.94, 0.92), offset (0.04, 0.05) and rotation (4 degrees), so the combined card must
// show the three inputs exactly aligned: the cut-out edge on the stroke, the dot inside
// the tip. Only the combined card casts a shadow, from a directional light at
// (48, -32, 0), intensity 2.2, shadow distance 18 at 2048. The camera is fixed at
// (0, 4.1, 10.8) looking at (0, 1.4, 0), fov 48, far clip 40.
//
// Keys, standing in for upstream's control panel:
//   1 same transforms on / off: off adds i x 0.0001 to the x offset of map i
//     (diffuse 0, opacity 1, emissive 2), which upstream groups into separate transform
//     uniforms; both modes must produce the same aligned result
//   2 animate on / off: the tiling, offset x and offset y follow slow sine waves
//   3 shader trace on / off (see the DEVIATION below)
//   F1 HUD | Esc quit
//
// DEVIATIONS:
// - upstream draws the textures on an HTML canvas. Here they are rasterised on the CPU
//   with 4x4 supersampling: the arrow polygon, its 14 px mitred stroke, the quadrants
//   and the dot follow upstream's coordinates exactly, but the "UV" label is not a font:
//   it is two hand-built glyph shapes sized like upstream's bold 34 px sans-serif,
//   centred at x 128 on baseline 175.
// - upstream's "Trace shaders" toggle enables its TRACEID_SHADER_ALLOC trace, which this
//   engine does not have. Key 3 instead logs every change of the device's live shader
//   count, which is what that trace would report. Here every map's transform is a
//   uniform of its own, so changing the grouping allocates no shader either: the
//   count must stay put in both modes, with or without animation.
// - upstream's HTML overlay is a log line, printed at start and on every toggle.
// - upstream caps the device pixel ratio at 2; the examples here render at 1.
//
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "../exampleApp.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr uint32_t kTextureSize = 256;

    // Rasterisation samples per pixel along each axis.
    constexpr int kSamples = 4;

    struct Point
    {
        float x;
        float y;
    };

    // The asymmetric arrow every texture shares, in canvas pixels (y down).
    constexpr std::array<Point, 7> kArrow = {{
        {128.0f, 18.0f}, {224.0f, 112.0f}, {174.0f, 112.0f}, {174.0f, 224.0f},
        {82.0f, 224.0f}, {82.0f, 112.0f}, {32.0f, 112.0f},
    }};

    constexpr float kStrokeWidth = 14.0f;

    struct Rgb
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    constexpr Rgb kDiffuseGround{0x14, 0x17, 0x1f};
    constexpr Rgb kInk{0x10, 0x18, 0x20};
    constexpr std::array<Rgb, 4> kQuadrantColors = {{
        {0xe7, 0x4c, 0x3c}, {0x2e, 0xcc, 0x71}, {0x34, 0x98, 0xdb}, {0xf1, 0xc4, 0x0f},
    }};

    using Polygon = std::vector<Point>;

    bool insidePolygon(const Polygon& polygon, const float x, const float y)
    {
        bool inside = false;
        for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
            const Point& a = polygon[i];
            const Point& b = polygon[j];
            if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) {
                inside = !inside;
            }
        }
        return inside;
    }

    // The polygon moved `distance` outward along every edge normal (inward when
    // negative), with mitred corners: the outline of a stroke of width 2 x |distance|.
    Polygon offsetPolygon(const Polygon& polygon, const float distance)
    {
        float area = 0.0f;
        for (size_t i = 0; i < polygon.size(); ++i) {
            const Point& a = polygon[i];
            const Point& b = polygon[(i + 1) % polygon.size()];
            area += a.x * b.y - b.x * a.y;
        }

        // With a positive shoelace sum the interior lies on the (-dy, dx) side of every edge.
        auto outwardNormal = [area](const Point& a, const Point& b) {
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            return area > 0.0f ? Point{dy / length, -dx / length} : Point{-dy / length, dx / length};
        };

        Polygon result;
        result.reserve(polygon.size());
        const size_t count = polygon.size();
        for (size_t i = 0; i < count; ++i) {
            const Point& previous = polygon[(i + count - 1) % count];
            const Point& current = polygon[i];
            const Point& next = polygon[(i + 1) % count];
            const Point n0 = outwardNormal(previous, current);
            const Point n1 = outwardNormal(current, next);
            const float scale = distance / (1.0f + n0.x * n1.x + n0.y * n1.y);
            result.push_back({current.x + (n0.x + n1.x) * scale, current.y + (n0.y + n1.y) * scale});
        }
        return result;
    }

    // The "UV" label: bold 34 px sans-serif, centred at x 128 on baseline 175, cap
    // height 24.4 px and stems 5.2 px wide.
    bool insideLabel(const float x, const float y)
    {
        constexpr float baseline = 175.0f;
        constexpr float capTop = baseline - 24.4f;
        if (y < capTop || y > baseline) {
            return false;
        }

        // U: two stems joined by a half ring.
        constexpr float halfStem = 2.6f;
        constexpr float leftStem = 109.4f;
        constexpr float rightStem = 123.9f;
        constexpr float bowlX = 0.5f * (leftStem + rightStem);
        constexpr float bowlRadius = 0.5f * (rightStem - leftStem);
        constexpr float bowlY = baseline - halfStem - bowlRadius;
        if (y <= bowlY) {
            if (std::abs(x - leftStem) <= halfStem || std::abs(x - rightStem) <= halfStem) {
                return true;
            }
        } else {
            const float distance = std::hypot(x - bowlX, y - bowlY);
            if (std::abs(distance - bowlRadius) <= halfStem) {
                return true;
            }
        }

        // V: two diagonals meeting in a flat foot at the baseline.
        constexpr float vCentre = 140.25f;
        constexpr float slope = 0.324f;
        const float depth = y - capTop;
        const float outer = 10.95f - slope * depth;
        const float inner = 5.25f - slope * depth;
        const float offset = std::abs(x - vCentre);
        return offset <= outer && offset >= inner;
    }

    // Fills an RGBA8 image, row 0 at the top, averaging `paint` over kSamples x kSamples
    // points per pixel.
    template <typename Paint>
    std::vector<uint8_t> rasterise(Paint paint)
    {
        std::vector<uint8_t> pixels(kTextureSize * kTextureSize * 4);
        for (uint32_t py = 0; py < kTextureSize; ++py) {
            for (uint32_t px = 0; px < kTextureSize; ++px) {
                uint32_t sum[3] = {0, 0, 0};
                for (int sy = 0; sy < kSamples; ++sy) {
                    for (int sx = 0; sx < kSamples; ++sx) {
                        const float x = static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) / kSamples;
                        const float y = static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) / kSamples;
                        const Rgb color = paint(x, y);
                        sum[0] += color.r;
                        sum[1] += color.g;
                        sum[2] += color.b;
                    }
                }
                constexpr uint32_t count = kSamples * kSamples;
                uint8_t* pixel = &pixels[(py * kTextureSize + px) * 4];
                for (int c = 0; c < 3; ++c) {
                    pixel[c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
                }
                pixel[3] = 255;
            }
        }
        return pixels;
    }

    // Repeating, linearly filtered and without mipmaps, as upstream's. The base colour and
    // emissive maps are decoded from sRGB in the shader and the opacity map is read as
    // stored, which are upstream's srgb flags.
    std::shared_ptr<Texture> createTexture(GraphicsDevice* device, const char* name,
        const std::vector<uint8_t>& pixels)
    {
        TextureOptions options;
        options.name = name;
        options.width = kTextureSize;
        options.height = kTextureSize;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_shared<Texture>(device, options);
        texture->setAddressU(AddressMode::ADDRESS_REPEAT);
        texture->setAddressV(AddressMode::ADDRESS_REPEAT);
        texture->setLevelData(0, pixels.data(), pixels.size());
        texture->upload();
        return texture;
    }

    // The transform every map starts from, and the per-slot x offset of separate mode.
    constexpr float kBaseTilingX = 0.94f;
    constexpr float kBaseTilingY = 0.92f;
    constexpr float kBaseOffsetX = 0.04f;
    constexpr float kBaseOffsetY = 0.05f;
    constexpr float kBaseRotation = 4.0f;
    constexpr float kSeparateOffset = 0.0001f;

    enum MapSlot
    {
        SlotDiffuse = 0,
        SlotOpacity = 1,
        SlotEmissive = 2,
        SlotCount = 3,
    };

    void setMapTransform(StandardMaterial& material, const MapSlot slot, const Vector2& tiling,
        const Vector2& offset, const float rotation)
    {
        switch (slot) {
        case SlotDiffuse:
            material.setDiffuseMapTiling(tiling);
            material.setDiffuseMapOffset(offset);
            material.setDiffuseMapRotation(rotation);
            break;
        case SlotOpacity:
            material.setOpacityMapTiling(tiling);
            material.setOpacityMapOffset(offset);
            material.setOpacityMapRotation(rotation);
            break;
        case SlotEmissive:
            material.setEmissiveMapTiling(tiling);
            material.setEmissiveMapOffset(offset);
            material.setEmissiveMapRotation(rotation);
            break;
        default:
            break;
        }
    }
}

class MaterialTextureTransformsExample final: public ExampleApp
{
public:
    MaterialTextureTransformsExample(): ExampleApp({.title = "Material Texture Transforms"}) {}

protected:
    bool create() override
    {
        createTextures();
        createMaterials();

        // The ground.
        Entity* ground = createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(12.0f, 1.0f, 10.0f));
        ground->setName("Ground");
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
            render->setReceiveShadows(true);
        }

        // The four cards: the three sources and the combined output.
        const std::array<std::pair<const char*, StandardMaterial*>, 4> displays = {{
            {"Diffuse Source", _diffuseMaterial.get()},
            {"Opacity Source", _opacityMaterial.get()},
            {"Emissive Source", _emissiveMaterial.get()},
            {"Combined Output", _combinedMaterial.get()},
        }};
        constexpr std::array<float, 4> positions = {-3.6f, -1.4f, 0.8f, 3.4f};
        for (size_t i = 0; i < displays.size(); ++i) {
            const auto& [name, material] = displays[i];
            Entity* card = createPrimitive("plane", material, Vector3(positions[i], 1.75f, 0.0f),
                Vector3(1.7f, 1.0f, 2.3f));
            card->setName(name);
            if (auto* render = card->findComponent<RenderComponent>()) {
                render->setCastShadows(material == _combinedMaterial.get());
                render->setReceiveShadows(false);
            }
            // The plane's v = 0 edge is at -Z, so a +90 degree turn about X stands the
            // texture's top row at the top of the card, facing +Z.
            card->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        }

        Entity* light = createDirectionalLight(Vector3(48.0f, -32.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f),
            2.2f, true);
        light->setName("Directional Light");
        if (auto* lightComponent = light->findComponent<LightComponent>()) {
            lightComponent->setShadowDistance(18.0f);
            lightComponent->setShadowResolution(2048);
        }

        Entity* camera = createCamera(Vector3(0.0f, 4.1f, 10.8f));
        camera->setName("Camera");
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.08f, 0.09f, 0.12f, 1.0f));
            cameraComponent->camera()->setFarClip(40.0f);
            cameraComponent->camera()->setFov(48.0f);
        }
        camera->lookAt(Vector3(0.0f, 1.4f, 0.0f));

        applyTransforms(_sameTransforms, _animationTime);
        logState();
        spdlog::info("Keys: 1 same / separate transforms, 2 animate, 3 shader trace, Esc quit.");
        return true;
    }

    void update(const float dt) override
    {
        if (const auto* keyboard = engine()->keyboard()) {
            bool changed = false;
            if (keyboard->wasPressed(Key::Digit1)) {
                _sameTransforms = !_sameTransforms;
                changed = true;
            }
            if (keyboard->wasPressed(Key::Digit2)) {
                _animate = !_animate;
                changed = true;
            }
            if (keyboard->wasPressed(Key::Digit3)) {
                _traceShaderAlloc = !_traceShaderAlloc;
                _liveShaders = -1;
                spdlog::info("Shader trace: {}", _traceShaderAlloc ? "ON" : "OFF");
            }

            if (changed) {
                _animationTime = 0.0f;
                applyTransforms(_sameTransforms, _animationTime);
                logState();
            } else if (_animate) {
                _animationTime += dt;
                applyTransforms(_sameTransforms, _animationTime);
            }
        }

        // Shaders are created while a frame renders, so a change shows up at the next update.
        if (_traceShaderAlloc) {
            const int shaders = device()->liveResourceCounts().shaders;
            if (_liveShaders >= 0 && shaders != _liveShaders) {
                spdlog::info("Shader trace: live shaders {} -> {}", _liveShaders, shaders);
            }
            _liveShaders = shaders;
        }
    }

private:
    void createTextures()
    {
        const Polygon arrow(kArrow.begin(), kArrow.end());
        const Polygon strokeOuter = offsetPolygon(arrow, 0.5f * kStrokeWidth);
        const Polygon strokeInner = offsetPolygon(arrow, -0.5f * kStrokeWidth);

        // Quadrant colours clipped to the arrow, then its stroke and the label on top.
        const auto diffusePixels = rasterise([&](const float x, const float y) {
            if (insideLabel(x, y)) {
                return kInk;
            }
            if (insidePolygon(strokeOuter, x, y) && !insidePolygon(strokeInner, x, y)) {
                return kInk;
            }
            if (insidePolygon(arrow, x, y)) {
                const float half = 0.5f * static_cast<float>(kTextureSize);
                const int quadrant = (x >= half ? 1 : 0) + (y >= half ? 2 : 0);
                return kQuadrantColors[quadrant];
            }
            return kDiffuseGround;
        });

        // The arrow in white on black.
        const auto opacityPixels = rasterise([&](const float x, const float y) {
            return insidePolygon(arrow, x, y) ? Rgb{255, 255, 255} : Rgb{0, 0, 0};
        });

        // A dim arrow with a white dot of radius 20 at (128, 58).
        const auto emissivePixels = rasterise([&](const float x, const float y) {
            if (std::hypot(x - 128.0f, y - 58.0f) <= 20.0f) {
                return Rgb{255, 255, 255};
            }
            return insidePolygon(arrow, x, y) ? Rgb{0x20, 0x20, 0x20} : Rgb{0, 0, 0};
        });

        _diffuseTexture = createTexture(device().get(), "transform-diffuse", diffusePixels);
        _opacityTexture = createTexture(device().get(), "transform-opacity", opacityPixels);
        _emissiveTexture = createTexture(device().get(), "transform-emissive", emissivePixels);
    }

    void createMaterials()
    {
        const Color white(1.0f, 1.0f, 1.0f, 1.0f);
        const Color black(0.0f, 0.0f, 0.0f, 1.0f);

        _diffuseMaterial = std::make_shared<StandardMaterial>();
        _diffuseMaterial->setName("Diffuse Source Material");
        _diffuseMaterial->setDiffuse(white);
        _diffuseMaterial->setDiffuseMap(_diffuseTexture.get());
        _diffuseMaterial->setCullMode(CullMode::CULLFACE_NONE);

        // setAlphaMode resets the blend, the depth state and the transparent flag, so it
        // goes first.
        _opacityMaterial = std::make_shared<StandardMaterial>();
        _opacityMaterial->setAlphaMode(AlphaMode::MASK);
        _opacityMaterial->setAlphaCutoff(0.5f);
        _opacityMaterial->setName("Opacity Source Material");
        _opacityMaterial->setDiffuse(white);
        _opacityMaterial->setOpacityMap(_opacityTexture.get());
        _opacityMaterial->setOpacityMapChannel(MapChannel::MAP_CHANNEL_R);
        _opacityMaterial->setCullMode(CullMode::CULLFACE_NONE);

        _emissiveMaterial = std::make_shared<StandardMaterial>();
        _emissiveMaterial->setName("Emissive Source Material");
        _emissiveMaterial->setDiffuse(black);
        _emissiveMaterial->setEmissive(white);
        _emissiveMaterial->setEmissiveMap(_emissiveTexture.get());
        _emissiveMaterial->setEmissiveIntensity(2.0f);
        _emissiveMaterial->setCullMode(CullMode::CULLFACE_NONE);

        _combinedMaterial = std::make_shared<StandardMaterial>();
        _combinedMaterial->setAlphaMode(AlphaMode::MASK);
        _combinedMaterial->setAlphaCutoff(0.5f);
        _combinedMaterial->setName("Combined Transform Grouping Material");
        _combinedMaterial->setDiffuse(white);
        _combinedMaterial->setDiffuseMap(_diffuseTexture.get());
        _combinedMaterial->setOpacityMap(_opacityTexture.get());
        _combinedMaterial->setOpacityMapChannel(MapChannel::MAP_CHANNEL_R);
        _combinedMaterial->setEmissive(white);
        _combinedMaterial->setEmissiveMap(_emissiveTexture.get());
        _combinedMaterial->setEmissiveIntensity(2.0f);
        _combinedMaterial->setCullMode(CullMode::CULLFACE_NONE);

        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.42f, 0.44f, 0.48f, 1.0f));
        _groundMaterial->setGloss(0.25f);
    }

    // Gives every map of the combined material, and the one map of its source material,
    // the transform for `time`; separate mode moves map i by i x kSeparateOffset in x.
    void applyTransforms(const bool sameTransforms, const float time)
    {
        const float scaleWave = std::sin(time * 0.8f) * 0.025f;
        const float offsetX = std::sin(time * 0.55f) * 0.025f;
        const float offsetY = std::cos(time * 0.7f) * 0.02f;

        const std::array<StandardMaterial*, SlotCount> sources = {
            _diffuseMaterial.get(), _opacityMaterial.get(), _emissiveMaterial.get()};
        for (int i = 0; i < SlotCount; ++i) {
            const auto slot = static_cast<MapSlot>(i);
            const float materialOffset = sameTransforms ? 0.0f : static_cast<float>(i) * kSeparateOffset;
            const Vector2 tiling(kBaseTilingX + scaleWave, kBaseTilingY + scaleWave);
            const Vector2 offset(kBaseOffsetX + offsetX + materialOffset, kBaseOffsetY + offsetY);
            setMapTransform(*_combinedMaterial, slot, tiling, offset, kBaseRotation);
            setMapTransform(*sources[i], slot, tiling, offset, kBaseRotation);
        }
    }

    void logState() const
    {
        const int groupCount = _sameTransforms ? 1 : SlotCount;
        spdlog::info("DIFFUSE + OPACITY + EMISSIVE = COMBINED | MODE: {} ({} transform group{}) | "
                     "ANIMATION: {} | both modes should produce the same aligned result; separate "
                     "mode offsets channels by {}",
            _sameTransforms ? "SHARED" : "SEPARATE", groupCount, groupCount == 1 ? "" : "s",
            _animate ? "ON" : "OFF", kSeparateOffset);
    }

    std::shared_ptr<Texture> _diffuseTexture;
    std::shared_ptr<Texture> _opacityTexture;
    std::shared_ptr<Texture> _emissiveTexture;

    std::shared_ptr<StandardMaterial> _diffuseMaterial;
    std::shared_ptr<StandardMaterial> _opacityMaterial;
    std::shared_ptr<StandardMaterial> _emissiveMaterial;
    std::shared_ptr<StandardMaterial> _combinedMaterial;
    std::shared_ptr<StandardMaterial> _groundMaterial;

    bool _sameTransforms = true;
    bool _animate = false;
    bool _traceShaderAlloc = true;
    float _animationTime = 0.0f;

    // The live shader count at the last update, -1 before the first.
    int _liveShaders = -1;
};

VISUTWIN_EXAMPLE_MAIN(MaterialTextureTransformsExample)
