// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The gizmo shapes: arrow, arc, box, box line, plane and sphere.
//
#pragma once

#include <array>

#include "framework/components/render/primitiveGeometry.h"
#include "framework/gizmo/shape/shape.h"

namespace visutwin::canvas
{
    /// A cylinder line from `gap` out to `gap + lineLength`, capped
    /// by a cone head. Picked against the cone and a cylinder fattened by `tolerance`.
    class ArrowShape final : public Shape
    {
    public:
        ArrowShape(Engine* engine, const ShapeArgs& args);

        float gap() const { return _gap; }
        void setGap(float value) { _gap = value; update(); }
        float lineThickness() const { return _lineThickness; }
        void setLineThickness(float value) { _lineThickness = value; update(); }
        float lineLength() const { return _lineLength; }
        void setLineLength(float value) { _lineLength = value; update(); }
        float arrowThickness() const { return _arrowThickness; }
        void setArrowThickness(float value) { _arrowThickness = value; update(); }
        float arrowLength() const { return _arrowLength; }
        void setArrowLength(float value) { _arrowLength = value; update(); }
        float tolerance() const { return _tolerance; }
        void setTolerance(float value) { _tolerance = value; update(); }

    protected:
        void update() override;

    private:
        float _gap = 0.0f;
        float _lineThickness = 0.02f;
        float _lineLength = 0.5f;
        float _arrowThickness = 0.12f;
        float _arrowLength = 0.18f;
        float _tolerance = 0.1f;
        Entity* _head = nullptr;
        Entity* _line = nullptr;
    };

    struct ArcShapeArgs
    {
        float tubeRadius = 0.01f;
        float ringRadius = 0.5f;
        float sectorAngle = 360.0f;
    };

    /// A torus segment (`sectorAngle` degrees of a ring in the shape's
    /// XZ plane) and the full ring, of which one shows at a time.
    class ArcShape final : public Shape
    {
    public:
        /// Torus segment counts for rendering and for intersection.
        static constexpr int kRenderSegments = 80;
        static constexpr int kIntersectSegments = 20;

        ArcShape(Engine* engine, const ShapeArgs& args, const ArcShapeArgs& arcArgs = {});

        enum class Show
        {
            Sector,
            Ring,
            None
        };

        float tubeRadius() const { return _tubeRadius; }
        void setTubeRadius(float value) { _tubeRadius = value; update(); }
        float ringRadius() const { return _ringRadius; }
        void setRingRadius(float value) { _ringRadius = value; update(); }
        float sectorAngle() const { return _sectorAngle; }
        float tolerance() const { return _tolerance; }
        void setTolerance(float value) { _tolerance = value; update(); }

        void show(Show state);
        Show shown() const { return _shown; }

        /// The picking torus (tube widened by the tolerance) and the drawn one.
        PrimitiveGeometry intersectGeometry(float sectorAngle) const;
        PrimitiveGeometry renderGeometry(float sectorAngle) const;

    protected:
        void update() override;

    private:
        float _tubeRadius = 0.01f;
        float _ringRadius = 0.5f;
        float _sectorAngle = 360.0f;
        float _tolerance = 0.05f;
        Show _shown = Show::Sector;
        std::array<TriData*, 2> _triDataCache{};
    };

    /// The scale gizmo's centre box.
    class BoxShape final : public Shape
    {
    public:
        BoxShape(Engine* engine, const ShapeArgs& args);

        float size() const { return _size; }
        void setSize(float value) { _size = value; update(); }

    protected:
        void update() override;

    private:
        float _size = 0.06f;
    };

    /// The scale gizmo's axis handle, a line capped by a box.
    class BoxLineShape final : public Shape
    {
    public:
        BoxLineShape(Engine* engine, const ShapeArgs& args);

        float gap() const { return _gap; }
        void setGap(float value) { _gap = value; update(); }
        float lineThickness() const { return _lineThickness; }
        void setLineThickness(float value) { _lineThickness = value; update(); }
        float lineLength() const { return _lineLength; }
        void setLineLength(float value) { _lineLength = value; update(); }
        float boxSize() const { return _boxSize; }
        void setBoxSize(float value) { _boxSize = value; update(); }
        float tolerance() const { return _tolerance; }
        void setTolerance(float value) { _tolerance = value; update(); }

        bool flipped() const { return _flipped; }
        void setFlipped(bool value);

    protected:
        void update() override;

    private:
        float _gap = 0.0f;
        float _lineThickness = 0.02f;
        float _lineLength = 0.5f;
        float _boxSize = 0.12f;
        float _tolerance = 0.1f;
        bool _flipped = false;
        Entity* _box = nullptr;
        Entity* _line = nullptr;
    };

    /// The two-axis handle, a square offset from the centre into the
    /// quadrant `flipped` selects. Drawn double sided.
    class PlaneShape final : public Shape
    {
    public:
        PlaneShape(Engine* engine, const ShapeArgs& args);

        bool isPlane() const override { return true; }

        float size() const { return _size; }
        void setSize(float value) { _size = value; update(); }
        float gap() const { return _gap; }
        void setGap(float value) { _gap = value; update(); }

        /// Per component, 1 flips the offset along that axis.
        const Vector3& flipped() const { return _flipped; }
        void setFlipped(const Vector3& value);

        /// The local position the plane is placed at.
        const Vector3& position() const { return _position; }

    protected:
        void update() override;

    private:
        float _size = 0.16f;
        float _gap = 0.0f;
        Vector3 _flipped = Vector3(0.0f);
    };

    /// The translate gizmo's centre and the rotate gizmo's orbit sphere.
    class SphereShape final : public Shape
    {
    public:
        SphereShape(Engine* engine, const ShapeArgs& args, float radius = 0.03f);

        float radius() const { return _radius; }
        void setRadius(float value) { _radius = value; update(); }

    protected:
        void update() override;

    private:
        float _radius = 0.03f;
    };
}
