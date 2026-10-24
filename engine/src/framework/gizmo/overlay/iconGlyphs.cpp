// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "iconGlyphs.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <string>

#include "platform/graphics/constants.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float PI = std::numbers::pi_v<float>;

        // The stroke width, in glyph units.
        constexpr float STROKE = 1.6f;

        // The pixels per glyph unit of the textures, enough for high density screens.
        constexpr int ICON_SCALE = 4;

        // The samples per pixel along each axis.
        constexpr int SUPERSAMPLE = 4;

        enum class Cap
        {
            Butt,
            Square,
            Round
        };

        struct P
        {
            float x;
            float y;
        };

        // One part of a glyph, a point inside test in glyph units (y down).
        struct Part
        {
            enum class Kind
            {
                Segment,
                Arc,
                FillCircle,
                FillPolygon
            } kind;
            P a{}, b{};
            Cap capA = Cap::Butt;
            Cap capB = Cap::Butt;
            P center{};
            float radius = 0.0f;
            float from = 0.0f;    // an arc's angles, radians from +x toward +y (down)
            float to = 0.0f;
            std::vector<P> points;
        };

        Part segment(const P a, const P b, const Cap capA = Cap::Butt, const Cap capB = Cap::Butt)
        {
            Part part{Part::Kind::Segment};
            part.a = a;
            part.b = b;
            part.capA = capA;
            part.capB = capB;
            return part;
        }

        Part arc(const P center, const float radius, const float from, const float to, const Cap cap = Cap::Butt)
        {
            Part part{Part::Kind::Arc};
            part.center = center;
            part.radius = radius;
            part.from = from;
            part.to = to;
            part.capA = part.capB = cap;
            return part;
        }

        Part circle(const P center, const float radius)
        {
            return arc(center, radius, 0.0f, 2.0f * PI);
        }

        Part fillCircle(const P center, const float radius)
        {
            Part part{Part::Kind::FillCircle};
            part.center = center;
            part.radius = radius;
            return part;
        }

        Part fillPolygon(std::vector<P> points)
        {
            Part part{Part::Kind::FillPolygon};
            part.points = std::move(points);
            return part;
        }

        // A closed outline: each side a segment, each corner joined as `join` (Square makes the
        // mitre of a right angle).
        void outline(std::vector<Part>& parts, const std::vector<P>& points, const Cap join)
        {
            for (size_t i = 0; i < points.size(); ++i) {
                parts.push_back(segment(points[i], points[(i + 1) % points.size()], join, join));
            }
        }

        std::vector<Part> glyphParts(const OverlayIcon icon)
        {
            std::vector<Part> parts;
            switch (icon) {
                case OverlayIcon::Camera:
                    // an outlined body with a lens widening to the right
                    outline(parts, {{-8.0f, -5.0f}, {3.0f, -5.0f}, {3.0f, 5.0f}, {-8.0f, 5.0f}}, Cap::Square);
                    parts.push_back(fillPolygon({{4.0f, 0.0f}, {9.0f, -4.0f}, {9.0f, 4.0f}}));
                    break;
                case OverlayIcon::Light: {
                    // a sun, a dot with eight spokes
                    parts.push_back(fillCircle({0.0f, 0.0f}, 4.5f));
                    parts.push_back(segment({7.5f, 0.0f}, {10.5f, 0.0f}));
                    parts.push_back(segment({-7.5f, 0.0f}, {-10.5f, 0.0f}));
                    parts.push_back(segment({0.0f, 7.5f}, {0.0f, 10.5f}));
                    parts.push_back(segment({0.0f, -7.5f}, {0.0f, -10.5f}));
                    parts.push_back(segment({5.303f, 5.303f}, {7.425f, 7.425f}));
                    parts.push_back(segment({-5.303f, 5.303f}, {-7.425f, 7.425f}));
                    parts.push_back(segment({5.303f, -5.303f}, {7.425f, -7.425f}));
                    parts.push_back(segment({-5.303f, -5.303f}, {-7.425f, -7.425f}));
                    break;
                }
                case OverlayIcon::AudioListener:
                    // a microphone, its rounded head held in a cradle on a stand
                    parts.push_back(segment({-3.0f, -1.0f}, {-3.0f, -5.0f}));
                    parts.push_back(arc({0.0f, -5.0f}, 3.0f, PI, 2.0f * PI));
                    parts.push_back(segment({3.0f, -5.0f}, {3.0f, -1.0f}));
                    parts.push_back(arc({0.0f, -1.0f}, 3.0f, 0.0f, PI));
                    parts.push_back(arc({0.0f, -1.0f}, 6.0f, 0.0f, PI));
                    parts.push_back(segment({0.0f, 5.0f}, {0.0f, 8.5f}));
                    parts.push_back(segment({-4.0f, 8.5f}, {4.0f, 8.5f}));
                    break;
                case OverlayIcon::Sound: {
                    // a speaker, a box and its cone in one outline, with two sound waves
                    outline(parts, {{-7.5f, -3.0f}, {-4.0f, -3.0f}, {0.5f, -7.0f}, {0.5f, 7.0f}, {-4.0f, 3.0f},
                        {-7.5f, 3.0f}}, Cap::Round);
                    const float sweep = std::atan2(2.681f, 2.25f);
                    parts.push_back(arc({0.5f, 0.0f}, 3.5f, -sweep, sweep, Cap::Round));
                    parts.push_back(arc({0.5f, 0.0f}, 7.0f, -sweep, sweep, Cap::Round));
                    break;
                }
                case OverlayIcon::ParticleSystem:
                    // a cluster of dots around a larger one
                    parts.push_back(fillCircle({0.0f, 0.0f}, 3.2f));
                    parts.push_back(fillCircle({0.0f, -7.5f}, 2.2f));
                    parts.push_back(fillCircle({-6.495f, -3.75f}, 1.6f));
                    parts.push_back(fillCircle({-6.495f, 3.75f}, 2.2f));
                    parts.push_back(fillCircle({0.0f, 7.5f}, 1.6f));
                    parts.push_back(fillCircle({6.495f, 3.75f}, 2.2f));
                    parts.push_back(fillCircle({6.495f, -3.75f}, 1.6f));
                    break;
                case OverlayIcon::Anim:
                case OverlayIcon::Animation:
                    // a play button, a triangle in a circle
                    parts.push_back(circle({0.0f, 0.0f}, 9.0f));
                    parts.push_back(fillPolygon({{-2.5f, -4.5f}, {4.5f, 0.0f}, {-2.5f, 4.5f}}));
                    break;
                case OverlayIcon::Script:
                    // code brackets around a slash
                    parts.push_back(segment({-4.5f, -5.5f}, {-10.0f, 0.0f}, Cap::Butt, Cap::Square));
                    parts.push_back(segment({-10.0f, 0.0f}, {-4.5f, 5.5f}, Cap::Square, Cap::Butt));
                    parts.push_back(segment({4.5f, -5.5f}, {10.0f, 0.0f}, Cap::Butt, Cap::Square));
                    parts.push_back(segment({10.0f, 0.0f}, {4.5f, 5.5f}, Cap::Square, Cap::Butt));
                    parts.push_back(segment({1.8f, -8.0f}, {-1.8f, 8.0f}));
                    break;
                default:
                    break;
            }
            return parts;
        }

        float distance(const P a, const P b)
        {
            return std::hypot(a.x - b.x, a.y - b.y);
        }

        bool insideSegment(const Part& part, const P p)
        {
            constexpr float half = 0.5f * STROKE;
            const float dx = part.b.x - part.a.x;
            const float dy = part.b.y - part.a.y;
            const float length = std::hypot(dx, dy);
            if (length <= 0.0f) {
                return false;
            }
            const float ux = dx / length;
            const float uy = dy / length;
            const float along = (p.x - part.a.x) * ux + (p.y - part.a.y) * uy;
            const float across = std::abs(-(p.x - part.a.x) * uy + (p.y - part.a.y) * ux);
            const float start = part.capA == Cap::Square ? -half : 0.0f;
            const float end = length + (part.capB == Cap::Square ? half : 0.0f);
            if (across <= half && along >= start && along <= end) {
                return true;
            }
            return (part.capA == Cap::Round && distance(p, part.a) <= half) ||
                (part.capB == Cap::Round && distance(p, part.b) <= half);
        }

        bool insideArc(const Part& part, const P p)
        {
            constexpr float half = 0.5f * STROKE;
            const float r = distance(p, part.center);
            if (std::abs(r - part.radius) <= half) {
                float angle = std::atan2(p.y - part.center.y, p.x - part.center.x);
                while (angle < part.from) {
                    angle += 2.0f * PI;
                }
                if (angle <= part.to) {
                    return true;
                }
            }
            if (part.capA == Cap::Round) {
                const P a{part.center.x + part.radius * std::cos(part.from), part.center.y + part.radius * std::sin(part.from)};
                const P b{part.center.x + part.radius * std::cos(part.to), part.center.y + part.radius * std::sin(part.to)};
                return distance(p, a) <= half || distance(p, b) <= half;
            }
            return false;
        }

        bool insidePolygon(const std::vector<P>& points, const P p)
        {
            bool inside = false;
            for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
                const P& a = points[i];
                const P& b = points[j];
                if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
                    inside = !inside;
                }
            }
            return inside;
        }

        bool inside(const Part& part, const P p)
        {
            switch (part.kind) {
                case Part::Kind::Segment: return insideSegment(part, p);
                case Part::Kind::Arc: return insideArc(part, p);
                case Part::Kind::FillCircle: return distance(p, part.center) <= part.radius;
                case Part::Kind::FillPolygon: return insidePolygon(part.points, p);
            }
            return false;
        }
    }

    const char* overlayIconName(const OverlayIcon icon)
    {
        switch (icon) {
            case OverlayIcon::Camera: return "camera";
            case OverlayIcon::Light: return "light";
            case OverlayIcon::AudioListener: return "audiolistener";
            case OverlayIcon::Sound: return "sound";
            case OverlayIcon::ParticleSystem: return "particlesystem";
            case OverlayIcon::Anim: return "anim";
            case OverlayIcon::Animation: return "animation";
            case OverlayIcon::Script: return "script";
            default: return "";
        }
    }

    std::vector<uint8_t> rasterizeOverlayIcon(const OverlayIcon icon, const int size)
    {
        const std::vector<Part> parts = glyphParts(icon);
        std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 255);
        const float unit = 24.0f / static_cast<float>(size);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                int covered = 0;
                for (int sy = 0; sy < SUPERSAMPLE; ++sy) {
                    for (int sx = 0; sx < SUPERSAMPLE; ++sx) {
                        const P p{(static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / SUPERSAMPLE) * unit - 12.0f,
                            (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / SUPERSAMPLE) * unit - 12.0f};
                        for (const Part& part : parts) {
                            if (inside(part, p)) {
                                ++covered;
                                break;
                            }
                        }
                    }
                }
                const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4;
                pixels[index + 3] = static_cast<uint8_t>(
                    std::lround(255.0f * static_cast<float>(covered) / (SUPERSAMPLE * SUPERSAMPLE)));
            }
        }
        return pixels;
    }

    std::shared_ptr<OverlayIconTextures> getOverlayIconTextures(const std::shared_ptr<GraphicsDevice>& device)
    {
        static std::map<const GraphicsDevice*, std::weak_ptr<OverlayIconTextures>> cache;
        if (!device) {
            return nullptr;
        }
        std::weak_ptr<OverlayIconTextures>& slot = cache[device.get()];
        if (auto textures = slot.lock()) {
            return textures;
        }
        auto textures = std::make_shared<OverlayIconTextures>();
        constexpr int size = 24 * ICON_SCALE;
        for (size_t i = 0; i < kOverlayIconCount; ++i) {
            const auto icon = static_cast<OverlayIcon>(i);
            TextureOptions options;
            options.name = std::string("gizmo-icon-") + overlayIconName(icon);
            options.width = size;
            options.height = size;
            options.format = PixelFormat::PIXELFORMAT_RGBA8;
            options.mipmaps = true;
            options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;
            auto texture = std::make_shared<Texture>(device.get(), options);
            texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            const std::vector<uint8_t> pixels = rasterizeOverlayIcon(icon, size);
            texture->setLevelData(0, pixels.data(), pixels.size());
            texture->upload();
            textures->textures[i] = std::move(texture);
        }
        slot = textures;
        return textures;
    }
}
