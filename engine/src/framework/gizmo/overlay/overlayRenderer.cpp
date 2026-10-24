// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "overlayRenderer.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/math/matrix4.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/gizmo/gizmo.h"
#include "framework/gizmo/gizmoMath.h"
#include "framework/gizmo/shape/shapes.h"
#include "scene/camera.h"
#include "scene/layer.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float kPi = std::numbers::pi_v<float>;
        constexpr float DEG_TO_RAD_F = kPi / 180.0f;
        constexpr float RAD_TO_DEG_F = 180.0f / kPi;

        // a line is split into pieces whose far end is at most this ratio deeper than the near
        constexpr float PIECE_RATIO = 1.5f;
        constexpr int MAX_PIECES = 32;

        // a ring's torus is rebuilt only once its tube is this fraction off
        constexpr float TUBE_TOLERANCE = 0.25f;

        // the dash period as a multiple of the line thickness
        constexpr float DASH_PERIOD = 4.0f;
        constexpr float DASH_FRACTION = 0.5f;
        constexpr int MAX_DASHES = 24;
        constexpr float MAX_LINE_DASHES = 256.0f;

        constexpr float SECTOR_TOLERANCE = 0.5f;
        constexpr float PEAK_TOLERANCE = 1e-3f;
        constexpr float FILL_DEPTH_BIAS = 2.0f;

        // placements count up from the start of their band, so a frame draws in placement
        // order with the fills first, whatever order the layer added the shapes in
        constexpr uint32_t FILL_DRAW_ORDER = 0;
        constexpr uint32_t SHAPE_DRAW_ORDER = 1u << 20u;

        struct ConeKind
        {
            int segments;
            float peakRadius;
            float twist;
        };
        constexpr std::array<ConeKind, static_cast<size_t>(OverlayCone::Count)> CONES = {{
            {32, 0.0f, 0.0f},   // cone
            {32, 1.0f, 0.0f},   // disk
            {32, 1.0f, 0.0f},   // cylinder
            {3, 0.0f, 0.0f},    // triangle
            {4, 0.0f, 45.0f},   // frustum
        }};

        Vector3 cameraPositionOf(const CameraComponent* camera)
        {
            return camera && camera->entity() ? camera->entity()->position() : Vector3(0.0f);
        }

        Vector3 cameraForwardOf(const CameraComponent* camera)
        {
            if (!camera || !camera->entity()) {
                return Vector3(0.0f, 0.0f, -1.0f);
            }
            return (-Vector3(camera->entity()->worldTransform().getColumn(2))).normalized();
        }

        Vector3 lerp(const Vector3& a, const Vector3& b, const float t)
        {
            return a + (b - a) * t;
        }
    }

    OverlayRenderer::OverlayRenderer(Gizmo* gizmo, const OverlayRendererTheme& theme, Entity* parent,
        std::shared_ptr<Layer> layer)
        : _gizmo(gizmo), _theme(theme), _parent(parent ? parent : gizmo->root()), _fixedLayer(std::move(layer))
    {
        _layer = _fixedLayer ? _fixedLayer : gizmo->layer();
        Engine* engine = gizmo->engine();
        const auto device = engine ? engine->graphicsDevice() : nullptr;
        _icons = std::make_unique<OverlayIcons>(this, gizmo);
        _iconTextures = getOverlayIconTextures(device);

        // shapes are created lazily, so they take the current layer
        const auto args = [this](const Color* base, const Color* occluded) {
            ShapeArgs a;
            if (_layer) {
                a.layers = {_layer->id()};
            }
            a.defaultColor = base;
            a.hoverColor = base;
            a.occludedColor = occluded;
            return a;
        };
        // filled shapes sit just behind their surface, so lines and matching meshes don't z-fight
        const auto filled = [](std::unique_ptr<Shape> shape) {
            shape->setDepthBias(FILL_DEPTH_BIAS);
            return shape;
        };

        _lines = std::make_unique<MeshLineBatch>(engine, _layer ? _layer->id() : 0, _parent, SHAPE_DRAW_ORDER);
        _rings = &pool([engine, args](const Color* base, const Color* occluded) {
            auto ring = std::make_unique<ArcShape>(engine, args(base, occluded), ArcShapeArgs{0.01f, 1.0f, 360.0f});
            ring->show(ArcShape::Show::Ring);
            return std::unique_ptr<Shape>(std::move(ring));
        }, SHAPE_DRAW_ORDER);
        _arcs = &pool([engine, args](const Color* base, const Color* occluded) {
            auto arc = std::make_unique<ArcShape>(engine, args(base, occluded), ArcShapeArgs{0.01f, 1.0f, 90.0f});
            arc->show(ArcShape::Show::Sector);
            return std::unique_ptr<Shape>(std::move(arc));
        }, SHAPE_DRAW_ORDER);
        _arrows = &pool([engine, args](const Color* base, const Color* occluded) {
            return std::unique_ptr<Shape>(std::make_unique<ArrowShape>(engine, args(base, occluded)));
        }, SHAPE_DRAW_ORDER);
        _boxes = &pool([engine, args, filled](const Color* base, const Color* occluded) {
            return filled(std::make_unique<BoxShape>(engine, args(base, occluded)));
        }, FILL_DRAW_ORDER);
        // bracket arms draw over the shapes behind them
        _arms = &pool([engine, args](const Color* base, const Color* occluded) {
            auto arm = std::make_unique<BoxShape>(engine, args(base, occluded));
            arm->setDepthWrite(false);
            return std::unique_ptr<Shape>(std::move(arm));
        }, SHAPE_DRAW_ORDER);
        _handles = &pool([engine, args, gizmo](const Color* base, const Color* occluded) {
            auto box = std::make_unique<BoxShape>(engine, args(base, occluded));
            gizmo->addIntersectShape(box.get());
            return std::unique_ptr<Shape>(std::move(box));
        }, SHAPE_DRAW_ORDER);
        _pickDiscs = &pool([engine, args, gizmo](const Color* base, const Color* occluded) {
            const ConeKind& disk = CONES[static_cast<size_t>(OverlayCone::Disk)];
            auto disc = std::make_unique<ConeShape>(engine, args(base, occluded),
                ConeShapeArgs{disk.peakRadius, disk.segments, disk.twist});
            // icons face the camera a pixel or so apart, finer than the depth buffer resolves far
            // away, so they don't write depth and draw in placement order instead
            disc->setDepthWrite(false);
            gizmo->addIntersectShape(disc.get());
            return std::unique_ptr<Shape>(std::move(disc));
        }, SHAPE_DRAW_ORDER);
        _spheres = &pool([engine, args, filled](const Color* base, const Color* occluded) {
            return filled(std::make_unique<SphereShape>(engine, args(base, occluded)));
        }, FILL_DRAW_ORDER);
        _capsules = &pool([engine, args, filled](const Color* base, const Color* occluded) {
            return filled(std::make_unique<CapsuleShape>(engine, args(base, occluded)));
        }, FILL_DRAW_ORDER);
        for (size_t i = 0; i < CONES.size(); ++i) {
            const ConeKind kind = CONES[i];
            _cones[i] = &pool([engine, args, filled, kind](const Color* base, const Color* occluded) {
                return filled(std::make_unique<ConeShape>(engine, args(base, occluded),
                    ConeShapeArgs{kind.peakRadius, kind.segments, kind.twist}));
            }, FILL_DRAW_ORDER);
        }
        for (size_t i = 0; i < kOverlayIconCount; ++i) {
            Texture* texture = _iconTextures ? _iconTextures->textures[i].get() : nullptr;
            _glyphs[i] = &pool([engine, args, texture](const Color* base, const Color* occluded) {
                auto glyph = std::make_unique<GlyphShape>(engine, args(base, occluded), texture);
                glyph->setDepthWrite(false);
                return std::unique_ptr<Shape>(std::move(glyph));
            }, SHAPE_DRAW_ORDER);
        }
    }

    OverlayRenderer::~OverlayRenderer()
    {
        destroy();
    }

    ShapePool& OverlayRenderer::pool(const ShapePool::Create& create, const uint32_t drawOrder)
    {
        _pools.push_back(std::make_unique<ShapePool>(create, _parent, drawOrder));
        return *_pools.back();
    }

    void OverlayRenderer::begin()
    {
        const CameraComponent* cameraComponent = _gizmo->camera();
        const Camera* camera = cameraComponent ? cameraComponent->camera() : nullptr;
        Engine* engine = _gizmo->engine();
        if (camera) {
            const auto [width, height] = engine ? engine->canvasSize() : std::pair<int, int>{1, 1};
            const Vector4& rect = camera->rect();
            _perspective = camera->projection() == ProjectionType::Perspective;
            const float span = _perspective ? 2.0f * std::tan(0.5f * camera->fov() * DEG_TO_RAD_F)
                : 2.0f * camera->orthoHeight();
            const float extent = _perspective && camera->horizontalFov()
                ? static_cast<float>(width) * rect.getZ() : static_cast<float>(height) * rect.getW();
            _pixel = span / std::max(extent, 1.0f);
            _lines->setView(cameraPositionOf(cameraComponent), cameraForwardOf(cameraComponent), !_perspective);
        }

        const std::shared_ptr<Layer> layer = _fixedLayer ? _fixedLayer : _gizmo->layer();
        if (layer != _layer) {
            moveTo(layer);
        }

        for (const auto& p : _pools) {
            p->count = 0;
        }
        _lines->begin();
        _lines->drawOrder = SHAPE_DRAW_ORDER;
        _order = 0;
        _labels.clear();
        _icons->begin();
    }

    void OverlayRenderer::moveTo(const std::shared_ptr<Layer>& layer)
    {
        _layer = layer;
        if (!layer) {
            return;
        }
        _lines->setLayer(layer->id());
        const std::vector<int> ids{layer->id()};
        for (const auto& p : _pools) {
            for (const auto& item : p->items()) {
                for (auto* render : item->entity->findComponents<RenderComponent>()) {
                    render->setLayers(ids);
                }
            }
        }
    }

    void OverlayRenderer::end()
    {
        for (const auto& p : _pools) {
            p->finish();
        }
        _lines->end();
    }

    void OverlayRenderer::clear()
    {
        for (const auto& p : _pools) {
            p->count = 0;
        }
        _lines->begin();
        _labels.clear();
        _icons->begin();
        end();
    }

    float OverlayRenderer::viewDepth(const Vector3& point) const
    {
        const CameraComponent* camera = _gizmo->camera();
        return (point - cameraPositionOf(camera)).dot(cameraForwardOf(camera));
    }

    float OverlayRenderer::depth(const Vector3& point) const
    {
        if (!_perspective) {
            return 1.0f;
        }
        const CameraComponent* camera = _gizmo->camera();
        const float nearClip = camera && camera->camera() ? camera->camera()->nearClip() : 0.0f;
        return std::max(viewDepth(point), nearClip);
    }

    Vector3 OverlayRenderer::local(const Vector3& point) const
    {
        return point - _parent->position();
    }

    void OverlayRenderer::colors(const float alpha, const bool volume, const Color* color)
    {
        const Color& fill = _theme.fill ? *_theme.fill : *_theme.base;
        _base = volume ? fill : *_theme.base;
        _base.a *= alpha;
        _occluded = *_theme.occluded;
        _occluded.a *= alpha * (volume ? fill.a : 1.0f);
        if (const Color* t = color ? color : tint) {
            _base.r = _occluded.r = t->r;
            _base.g = _occluded.g = t->g;
            _base.b = _occluded.b = t->b;
        }
    }

    Shape* OverlayRenderer::next(ShapePool& pool)
    {
        ShapePoolItem& item = pool.nextShape();
        item.drawOrder = static_cast<double>(pool.drawOrder + ++_order);

        // start a new line run, so later lines draw over this shape
        _lines->split();
        _lines->drawOrder = SHAPE_DRAW_ORDER + ++_order;
        if (!(item.base == _base) || !(item.occluded == _occluded)) {
            item.base = _base;
            item.occluded = _occluded;
            item.shape->hover(false);
        }
        return item.shape.get();
    }

    void OverlayRenderer::piece(const Vector3& from, const Vector3& to, const float weight)
    {
        const Vector3 mid = lerp(from, to, 0.5f);
        const float thickness = weight * lineThickness * _pixel * depth(mid);
        _lines->add(local(from), local(to), thickness, _base, _occluded);
    }

    float OverlayRenderer::screenSize(const Vector3& center, const float radius) const
    {
        return radius / (_pixel * depth(center));
    }

    float OverlayRenderer::pixels(const Vector3& point, const float count) const
    {
        return count * _pixel * depth(point);
    }

    void OverlayRenderer::line(const Vector3& from, const Vector3& to, const float weight, const float alpha, const Color* color)
    {
        Vector3 a = from;
        Vector3 b = to;

        // keep the part in front of the near plane
        if (_perspective) {
            const CameraComponent* camera = _gizmo->camera();
            const float nearClip = camera && camera->camera() ? camera->camera()->nearClip() : 0.0f;
            const float v0 = viewDepth(a);
            const float v1 = viewDepth(b);
            if (v0 < nearClip && v1 < nearClip) {
                return;
            }
            if (v0 < nearClip) {
                a = lerp(a, b, (nearClip - v0) / (v1 - v0));
            } else if (v1 < nearClip) {
                b = lerp(b, a, (nearClip - v1) / (v0 - v1));
            }
        }

        // split into pieces growing with depth, so the thickness stays constant on screen
        float d0 = depth(a);
        float d1 = depth(b);
        if (d0 > d1) {
            std::swap(a, b);
            std::swap(d0, d1);
        }
        const float ratio = d1 / d0;
        const int pieces = ratio < PIECE_RATIO ? 1 :
            std::min(MAX_PIECES, static_cast<int>(std::ceil(std::log(ratio) / std::log(PIECE_RATIO))));

        colors(alpha, false, color);
        for (int i = 0; i < pieces; ++i) {
            const float t0 = pieces == 1 ? 0.0f : (std::pow(ratio, static_cast<float>(i) / pieces) - 1.0f) / (ratio - 1.0f);
            const float t1 = pieces == 1 ? 1.0f :
                (std::pow(ratio, static_cast<float>(i + 1) / pieces) - 1.0f) / (ratio - 1.0f);
            piece(lerp(a, b, t0), lerp(a, b, t1), weight);
        }
    }

    void OverlayRenderer::dashed(const Vector3& a, const Vector3& b, const float weight, const float alpha,
        const Color* color)
    {
        const CameraComponent* camera = _gizmo->camera();
        if (!camera) {
            return;
        }
        Vector3 start = a;
        Vector3 end = b;
        float d0 = 1.0f;
        float d1 = 1.0f;

        // keep the part in front of the near plane
        if (_perspective) {
            const float nearClip = camera->camera() ? camera->camera()->nearClip() : 0.0f;
            d0 = viewDepth(start);
            d1 = viewDepth(end);
            if (d0 < nearClip && d1 < nearClip) {
                return;
            }
            if (d0 < nearClip) {
                start = lerp(start, end, (nearClip - d0) / (d1 - d0));
                d0 = nearClip;
            } else if (d1 < nearClip) {
                end = lerp(end, start, (nearClip - d1) / (d0 - d1));
                d1 = nearClip;
            }
        }

        const Vector3 s0 = camera->worldToScreen(start);
        const Vector3 s1 = camera->worldToScreen(end);
        const float length = std::hypot(s1.getX() - s0.getX(), s1.getY() - s0.getY());
        if (length < 1.0f) {
            return;
        }

        // dashes are spaced on screen from the start, so moving the end only changes the last
        // dash; screen fraction s maps to line fraction s d0 / (d1 - s (d1 - d0))
        const float period = std::max(DASH_PERIOD * lineThickness, length / MAX_LINE_DASHES) / length;
        const int dashes = static_cast<int>(std::ceil(1.0f / period));
        colors(alpha, false, color);
        for (int i = 0; i < dashes; ++i) {
            const float u0 = static_cast<float>(i) * period;
            const float u1 = std::min((static_cast<float>(i) + DASH_FRACTION) * period, 1.0f);
            const Vector3 from = lerp(start, end, u0 * d0 / (d1 - u0 * (d1 - d0)));
            const Vector3 to = lerp(start, end, u1 * d0 / (d1 - u1 * (d1 - d0)));
            piece(from, to, weight);
        }
    }

    float OverlayRenderer::tube(const Vector3& center, const float radius, const float weight) const
    {
        // the ring may surround the camera, so use the distance to its edge
        const CameraComponent* camera = _gizmo->camera();
        const float dist = std::hypot(center.distance(cameraPositionOf(camera)), radius);
        const float nearClip = camera && camera->camera() ? camera->camera()->nearClip() : 0.0f;
        const float d = _perspective ? std::max(dist, nearClip) : 1.0f;
        return 0.5f * weight * lineThickness * _pixel * d / radius;
    }

    void OverlayRenderer::arc(const Vector3& center, const Vector3& x, const Vector3& y, const float radius,
        float from, float to, const float weight, const float alpha, const Color* color)
    {
        if (to < from) {
            std::swap(from, to);
        }
        const float tubeRadius = tube(center, radius, weight);
        const float sector = (to - from) * RAD_TO_DEG_F;

        colors(alpha, false, color);
        auto* shape = static_cast<ArcShape*>(next(*_arcs));

        // rebuilding the torus is costly, so only rebuild once well off
        if (std::abs(shape->tubeRadius() - tubeRadius) > tubeRadius * TUBE_TOLERANCE) {
            shape->setTubeRadius(tubeRadius);
        }
        if (std::abs(shape->sectorAngle() - sector) > SECTOR_TOLERANCE) {
            shape->setSectorAngle(sector);
        }

        // the torus sweeps from its x axis towards its z axis, about its y axis
        const Vector3 start = x * std::cos(from) + y * std::sin(from);
        const Vector3 side = x * -std::sin(from) + y * std::cos(from);
        const Vector3 normal = side.cross(start);
        const float m[16] = {
            start.getX(), start.getY(), start.getZ(), 0.0f,
            normal.getX(), normal.getY(), normal.getZ(), 0.0f,
            side.getX(), side.getY(), side.getZ(), 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
        shape->entity()->setLocalPosition(local(center));
        shape->entity()->setLocalRotation(Quaternion::fromMatrix4(Matrix4::load(m)));
        shape->entity()->setLocalScale(radius, radius, radius);
    }

    void OverlayRenderer::ring(const Vector3& center, const Quaternion& rotation, const float radius,
        const float weight, const float alpha)
    {
        const float tubeRadius = tube(center, radius, weight);

        colors(alpha);
        auto* shape = static_cast<ArcShape*>(next(*_rings));

        // rebuilding the torus is costly, so only rebuild once well off
        if (std::abs(shape->tubeRadius() - tubeRadius) > tubeRadius * TUBE_TOLERANCE) {
            shape->setTubeRadius(tubeRadius);
        }
        shape->entity()->setLocalPosition(local(center));
        shape->entity()->setLocalRotation(rotation);
        shape->entity()->setLocalScale(radius, radius, radius);
    }

    void OverlayRenderer::circle(const Vector3& center, const Vector3& normal, const float radius,
        const float weight, const float alpha)
    {
        ring(center, gizmoRotationBetween(Vector3(0.0f, 1.0f, 0.0f), normal.normalized()), radius, weight, alpha);
    }

    void OverlayRenderer::dashedCircle(const Vector3& center, const Vector3& normal, const float radius,
        const float weight, const float alpha)
    {
        const Quaternion rotation = gizmoRotationBetween(Vector3(0.0f, 1.0f, 0.0f), normal.normalized());
        const Vector3 x = rotation * Vector3(1.0f, 0.0f, 0.0f);
        const Vector3 z = rotation * Vector3(0.0f, 0.0f, 1.0f);
        const float size = screenSize(center, radius);
        const int dashes = std::clamp(
            static_cast<int>(std::round(2.0f * kPi * size / (DASH_PERIOD * lineThickness))), 8, MAX_DASHES);
        const float step = 2.0f * kPi / static_cast<float>(dashes);
        colors(alpha);
        for (int i = 0; i < dashes; ++i) {
            const float a0 = static_cast<float>(i) * step;
            const float a1 = a0 + DASH_FRACTION * step;
            const Vector3 from = x * (std::cos(a0) * radius) + z * (std::sin(a0) * radius) + center;
            const Vector3 to = x * (std::cos(a1) * radius) + z * (std::sin(a1) * radius) + center;
            piece(from, to, weight);
        }
    }

    void OverlayRenderer::arrow(const Vector3& position, const Quaternion& rotation, const float scale)
    {
        colors(1.0f);
        Shape* shape = next(*_arrows);
        shape->entity()->setLocalPosition(local(position));
        shape->entity()->setLocalRotation(rotation);
        shape->entity()->setLocalScale(scale, scale, scale);
    }

    BoxShape* OverlayRenderer::handle(const Vector3& position, const Color* hover)
    {
        _base = hover ? *hover : *_theme.base;
        _base.a = _theme.base->a;
        _occluded = hover ? *hover : *_theme.occluded;
        _occluded.a = _theme.occluded->a;
        auto* box = static_cast<BoxShape*>(next(*_handles));
        box->entity()->setLocalPosition(local(position));
        box->entity()->setLocalRotation(Quaternion());
        box->setSize(handleSize * _pixel * depth(position));
        return box;
    }

    void OverlayRenderer::arm(const Vector3& corner, const int axis, const float sign, const float length,
        const float thickness)
    {
        const float t = pixels(corner, thickness);
        colors(1.0f);
        Shape* shape = next(*_arms);
        Vector3 pos = local(corner);
        pos = withGizmoComponent(pos, axis, pos[axis] - sign * 0.5f * (length - 0.5f * t));
        const float along = length + 0.5f * t;
        shape->entity()->setLocalPosition(pos);
        shape->entity()->setLocalRotation(Quaternion());
        shape->entity()->setLocalScale(axis == 0 ? along : t, axis == 1 ? along : t, axis == 2 ? along : t);
    }

    void OverlayRenderer::dot(const Vector3& position, const float radius)
    {
        colors(1.0f);
        auto* sphere = static_cast<SphereShape*>(next(*_spheres));
        sphere->entity()->setLocalPosition(local(position));
        sphere->setRadius(radius * _pixel * depth(position));
    }

    Shape* OverlayRenderer::box(const Vector3& center, const Quaternion& rotation, const Vector3& size,
        const float alpha, const bool volume)
    {
        colors(alpha, volume);
        Shape* shape = next(*_boxes);
        shape->entity()->setLocalPosition(local(center));
        shape->entity()->setLocalRotation(rotation);
        shape->entity()->setLocalScale(size);
        return shape;
    }

    void OverlayRenderer::sphere(const Vector3& center, const float radius, const float alpha)
    {
        colors(alpha, true);
        auto* shape = static_cast<SphereShape*>(next(*_spheres));
        shape->entity()->setLocalPosition(local(center));
        shape->setRadius(radius);
    }

    void OverlayRenderer::capsule(const Vector3& center, const Quaternion& rotation, const float radius,
        const float height, const float alpha)
    {
        colors(alpha, true);
        auto* shape = static_cast<CapsuleShape*>(next(*_capsules));
        const float ratio = height / std::max(radius, 1e-6f);
        if (std::abs(shape->height() - ratio) > PEAK_TOLERANCE * ratio) {
            shape->setHeight(ratio);
        }
        shape->entity()->setLocalPosition(local(center));
        shape->entity()->setLocalRotation(rotation);
        shape->entity()->setLocalScale(radius, radius, radius);
    }

    void OverlayRenderer::cone(const OverlayCone kind, const Vector3& center, const Quaternion& rotation,
        const Vector3& size, const float alpha, const bool volume, const float peak)
    {
        colors(alpha, volume);
        auto* shape = static_cast<ConeShape*>(next(*_cones[static_cast<size_t>(kind)]));
        if (kind == OverlayCone::Frustum && std::abs(shape->peakRadius() - peak) > PEAK_TOLERANCE * peak) {
            shape->setPeakRadius(peak);
        }
        shape->entity()->setLocalPosition(local(center));
        shape->entity()->setLocalRotation(rotation);
        shape->entity()->setLocalScale(size);
    }

    Shape* OverlayRenderer::disc(const Vector3& center, const Quaternion& rotation, const float size,
        const float alpha, const Color* color)
    {
        colors(alpha, false, color);
        Shape* shape = next(*_pickDiscs);
        shape->entity()->setLocalPosition(local(center));
        shape->entity()->setLocalRotation(rotation);
        shape->entity()->setLocalScale(size, 1e-3f * size, size);
        return shape;
    }

    Shape* OverlayRenderer::glyph(const OverlayIcon kind, const Vector3& center, const Quaternion& rotation,
        const float size)
    {
        colors(1.0f);
        Shape* shape = next(*_glyphs[static_cast<size_t>(kind)]);
        shape->entity()->setLocalPosition(local(center));
        shape->entity()->setLocalRotation(rotation);
        shape->entity()->setLocalScale(size, 1.0f, size);
        return shape;
    }

    void OverlayRenderer::label(const Vector3& position, const std::string& markup, const float below)
    {
        const CameraComponent* camera = _gizmo->camera();
        if (!camera) {
            return;
        }
        const float nearClip = camera->camera() ? camera->camera()->nearClip() : 0.0f;
        if (_perspective && viewDepth(position) < nearClip) {
            return;
        }
        const Vector3 screen = camera->worldToScreen(position);
        _labels.push_back({true, markup, screen.getX(), screen.getY() + below + kGizmoReadoutGap});
    }

    void OverlayRenderer::destroy()
    {
        if (_destroyed) {
            return;
        }
        _destroyed = true;
        Gizmo* gizmo = _gizmo;
        for (const auto& p : _pools) {
            p->destroy([gizmo](Shape* shape) { gizmo->removeIntersectShape(shape); });
        }
        _pools.clear();
        _lines->destroy();
        _labels.clear();
    }
}
