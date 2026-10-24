// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The gizmo shapes: arrow, arc, box, box line, plane, sphere, capsule, cone and icon glyph.
//
#pragma once

#include <array>
#include <memory>

#include "framework/components/render/primitiveGeometry.h"
#include "framework/gizmo/shape/shape.h"

namespace visutwin::canvas
{
    class Texture;

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
        void setSectorAngle(float value) { _sectorAngle = value; update(); }
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
    /// quadrant `flipped` selects, drawn translucent and double sided, with an outline along
    /// its two outer edges.
    class PlaneShape final : public Shape
    {
    public:
        PlaneShape(Engine* engine, const ShapeArgs& args);

        bool isPlane() const override { return true; }

        float size() const { return _size; }
        void setSize(float value) { _size = value; update(); }
        float gap() const { return _gap; }
        void setGap(float value) { _gap = value; update(); }

        /// A factor of the fill's alpha (the outline keeps the full colour). 0.35 by default.
        float opacity() const { return _opacity; }
        void setOpacity(float value) { _opacity = value; hover(_hovered); }
        /// The outline thickness in the gizmo's units; 0 hides it. 0.01 by default.
        float outlineThickness() const { return _outlineThickness; }
        void setOutlineThickness(float value) { _outlineThickness = value; update(); }

        void setVisible(bool value) override;
        void hover(bool state) override;

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
        float _opacity = 0.35f;
        float _outlineThickness = 0.01f;
        std::shared_ptr<GizmoMaterial> _outlineMaterial;
        std::array<Entity*, 2> _outlines{};
        std::array<MeshInstance*, 2> _outlineMeshInstances{};
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

    /// A capsule along y of radius 1, `height` (caps included, at least 2) its height in
    /// radii: the entity's scale sets the radius.
    class CapsuleShape final : public Shape
    {
    public:
        CapsuleShape(Engine* engine, const ShapeArgs& args, float height = 4.0f);

        float height() const { return _height; }
        void setHeight(float value) { _height = value; update(); }

    protected:
        void update() override;

    private:
        PrimitiveGeometry geometry() const;

        float _height = 4.0f;
    };

    struct ConeShapeArgs
    {
        /// The peak radius as a fraction of the base radius: 0 a cone, 1 a cylinder.
        float peakRadius = 0.0f;
        /// The divisions around: 32 a round cone, 3 a triangle, 4 a square pyramid.
        int segments = 32;
        /// A turn about y, in degrees (45 sets a square pyramid's sides along the axes).
        float twist = 0.0f;
    };

    /// A unit cone along y, its base of radius 0.5 at y = -0.5. Picking keeps the initial
    /// peak radius.
    class ConeShape final : public Shape
    {
    public:
        ConeShape(Engine* engine, const ShapeArgs& args, const ConeShapeArgs& coneArgs = {});

        float peakRadius() const { return _peakRadius; }
        void setPeakRadius(float value) { _peakRadius = value; update(); }

    protected:
        void update() override;

    private:
        PrimitiveGeometry geometry() const;

        float _peakRadius = 0.0f;
        int _segments = 32;
        Entity* _body = nullptr;
    };

    /// A unit square on the xz plane drawing a texture's coverage in the shape's colour, its
    /// image's top row toward -z. Double sided, and not picked by itself.
    class GlyphShape final : public Shape
    {
    public:
        GlyphShape(Engine* engine, const ShapeArgs& args, Texture* texture);
    };
}
