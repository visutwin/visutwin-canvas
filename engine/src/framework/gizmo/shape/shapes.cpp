// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// DEVIATION (all shapes): the unit primitives come from the engine's render primitives
// (primitiveGeometry.h), whose sphere has 48 bands where upstream's gizmo sphere has 32
// (render) and 16 (picking), and whose plane is one quad where upstream's has 5 x 5.
// Both are the same surfaces to within the tessellation.
//
#include "shapes.h"

#include <algorithm>

#include "framework/components/componentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/entity.h"
#include "framework/gizmo/gizmoMaterial.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    // ---------------------------------------------------------------- ArrowShape

    ArrowShape::ArrowShape(Engine* engine, const ShapeArgs& args)
        : Shape(engine, "arrow", args)
    {
        // intersect
        addTriData(createConeGeometry());
        addTriData(createCylinderGeometry(), 1);

        // render
        _head = createChild(_entity, std::string("head:") + gizmoAxisName(_axis));
        createRenderComponent(_head, {createMesh(createConeGeometry())});
        _line = createChild(_entity, std::string("line:") + gizmoAxisName(_axis));
        createRenderComponent(_line, {createMesh(createCylinderGeometry())});

        update();
    }

    void ArrowShape::update()
    {
        // intersect
        _triDataStore[0]->setTransform(Vector3(0.0f, _gap + _arrowLength * 0.5f + _lineLength, 0.0f), Quaternion(),
            Vector3(_arrowThickness, _arrowLength, _arrowThickness));
        _triDataStore[1]->setTransform(Vector3(0.0f, _gap + _lineLength * 0.5f, 0.0f), Quaternion(),
            Vector3(_lineThickness + _tolerance, _lineLength, _lineThickness + _tolerance));

        // render
        _head->setLocalPosition(0.0f, _gap + _arrowLength * 0.5f + _lineLength, 0.0f);
        _head->setLocalScale(_arrowThickness, _arrowLength, _arrowThickness);
        _line->setLocalPosition(0.0f, _gap + _lineLength * 0.5f, 0.0f);
        _line->setLocalScale(_lineThickness, _lineLength, _lineThickness);
    }

    // ---------------------------------------------------------------- ArcShape

    ArcShape::ArcShape(Engine* engine, const ShapeArgs& args, const ArcShapeArgs& arcArgs)
        : Shape(engine, "disk", args), _tubeRadius(arcArgs.tubeRadius), _ringRadius(arcArgs.ringRadius),
          _sectorAngle(arcArgs.sectorAngle)
    {
        // intersect: the sector and the full ring; only one is picked at a time
        _triDataCache[0] = addTriData(intersectGeometry(_sectorAngle));
        _triDataCache[1] = addTriData(intersectGeometry(360.0f));
        _triData = {_triDataCache[0]};

        // render
        createRenderComponent(_entity, {createMesh(renderGeometry(_sectorAngle)), createMesh(renderGeometry(360.0f))});
        show(Show::Sector);
    }

    PrimitiveGeometry ArcShape::intersectGeometry(const float sectorAngle) const
    {
        return createTorusGeometry(_tubeRadius + _tolerance, _ringRadius, sectorAngle, kIntersectSegments, 20);
    }

    PrimitiveGeometry ArcShape::renderGeometry(const float sectorAngle) const
    {
        return createTorusGeometry(_tubeRadius, _ringRadius, sectorAngle, kRenderSegments, 20);
    }

    void ArcShape::update()
    {
        // intersect
        _triDataCache[0]->fromGeometry(intersectGeometry(_sectorAngle));
        _triDataCache[1]->fromGeometry(intersectGeometry(360.0f));

        // render
        replaceMeshes(_entity, {createMesh(renderGeometry(_sectorAngle)), createMesh(renderGeometry(360.0f))});
    }

    void ArcShape::show(const Show state)
    {
        _shown = state;
        if (_meshInstances.size() < 2) {
            return;
        }
        switch (state) {
            case Show::Sector:
                _triData[0] = _triDataCache[0];
                _meshInstances[0]->setVisible(true);
                _meshInstances[1]->setVisible(false);
                break;
            case Show::Ring:
                _triData[0] = _triDataCache[1];
                _meshInstances[0]->setVisible(false);
                _meshInstances[1]->setVisible(true);
                break;
            case Show::None:
                _meshInstances[0]->setVisible(false);
                _meshInstances[1]->setVisible(false);
                break;
        }
        for (size_t i = 0; i < _occludedMeshInstances.size() && i < _meshInstances.size(); ++i) {
            _occludedMeshInstances[i]->setVisible(_meshInstances[i]->visible());
        }
    }

    // ---------------------------------------------------------------- BoxShape

    BoxShape::BoxShape(Engine* engine, const ShapeArgs& args)
        : Shape(engine, "boxCenter", args)
    {
        addTriData(createBoxGeometry(), 2);
        createRenderComponent(_entity, {createMesh(createBoxGeometry())});
        update();
    }

    void BoxShape::update()
    {
        _entity->setLocalScale(_size, _size, _size);
    }

    // ---------------------------------------------------------------- BoxLineShape

    BoxLineShape::BoxLineShape(Engine* engine, const ShapeArgs& args)
        : Shape(engine, "boxLine", args)
    {
        // intersect
        addTriData(createBoxGeometry());
        addTriData(createCylinderGeometry(), 1);

        // render
        _box = createChild(_entity, std::string("box:") + gizmoAxisName(_axis));
        createRenderComponent(_box, {createMesh(createBoxGeometry())});
        _line = createChild(_entity, std::string("line:") + gizmoAxisName(_axis));
        createRenderComponent(_line, {createMesh(createCylinderGeometry())});

        update();
    }

    void BoxLineShape::setFlipped(const bool value)
    {
        if (_flipped == value) {
            return;
        }
        _flipped = value;
        Vector3 euler;
        if (gizmoVectorEquals(_rotation, Vector3(0.0f))) {
            euler = Vector3(0.0f, 0.0f, _flipped ? 180.0f : 0.0f);
        } else {
            euler = _rotation * (_flipped ? -1.0f : 1.0f);
        }
        _line->setEnabled(!_flipped);
        _entity->setLocalEulerAngles(euler.getX(), euler.getY(), euler.getZ());
    }

    void BoxLineShape::update()
    {
        // intersect
        _triDataStore[0]->setTransform(Vector3(0.0f, _gap + _boxSize * 0.5f + _lineLength, 0.0f), Quaternion(),
            Vector3(_boxSize));
        _triDataStore[1]->setTransform(Vector3(0.0f, _gap + _lineLength * 0.5f, 0.0f), Quaternion(),
            Vector3(_lineThickness + _tolerance, _lineLength, _lineThickness + _tolerance));

        // render
        _box->setLocalPosition(0.0f, _gap + _boxSize * 0.5f + _lineLength, 0.0f);
        _box->setLocalScale(_boxSize, _boxSize, _boxSize);
        _line->setLocalPosition(0.0f, _gap + _lineLength * 0.5f, 0.0f);
        _line->setLocalScale(_lineThickness, _lineLength, _lineThickness);
    }

    // ---------------------------------------------------------------- PlaneShape

    namespace
    {
        // Upstream's PlaneShape declares `_cull = CULLFACE_NONE` as a class field, which
        // JavaScript initialises after the base constructor has read `args.cull`: a plane
        // is always double sided.
        ShapeArgs doubleSided(ShapeArgs args)
        {
            args.cull = CullMode::CULLFACE_NONE;
            return args;
        }
    }

    PlaneShape::PlaneShape(Engine* engine, const ShapeArgs& args)
        : Shape(engine, "plane", doubleSided(args))
    {
        addTriData(createPlaneGeometry(), 1);
        createRenderComponent(_entity, {createMesh(createPlaneGeometry())});

        // the outlines along the two outer edges
        _outlineMaterial = std::make_shared<GizmoMaterial>(_device);
        _outlineMaterial->setDepth(_depth > 0.0f ? _depth : -1.0f);
        _outlineMaterial->setCullMode(_cull);
        const auto mesh = createMesh(createBoxGeometry());
        for (size_t i = 0; i < _outlines.size(); ++i) {
            Entity* outline = createChild(_entity, std::string("outline:") + gizmoAxisName(_axis) + ":" +
                std::to_string(i));
            auto* render = static_cast<RenderComponent*>(outline->addComponent<RenderComponent>());
            if (!render) {
                continue;
            }
            render->setLayers(_layers);
            render->setCastShadows(false);
            render->setReceiveShadows(false);
            auto instance = std::make_unique<MeshInstance>(mesh, std::static_pointer_cast<Material>(_outlineMaterial),
                outline);
            instance->setCull(false);
            instance->setCastShadow(false);
            instance->setReceiveShadow(false);
            instance->setVisible(_visible);
            _outlines[i] = outline;
            _outlineMeshInstances[i] = render->addMeshInstance(std::move(instance));
        }

        update();
        hover(false);
    }

    void PlaneShape::setVisible(const bool value)
    {
        Shape::setVisible(value);
        for (MeshInstance* instance : _outlineMeshInstances) {
            if (instance) {
                instance->setVisible(value);
            }
        }
    }

    void PlaneShape::hover(const bool state)
    {
        _hovered = state;
        _material->setColor(stateColor(_opacity));
        if (_outlineMaterial) {
            _outlineMaterial->setColor(stateColor());
        }
    }

    void PlaneShape::setFlipped(const Vector3& value)
    {
        if (_flipped.distance(value) < 1e-6f) {
            return;
        }
        _flipped = value;
        update();
    }

    void PlaneShape::update()
    {
        const float offset = _size / 2.0f + _gap;
        _position = Vector3(_flipped.getX() != 0.0f ? -offset : offset,
                            _flipped.getY() != 0.0f ? -offset : offset,
                            _flipped.getZ() != 0.0f ? -offset : offset);
        _position = withGizmoComponent(_position, gizmoAxisIndex(_axis), 0.0f);
        _entity->setLocalPosition(_position);
        _entity->setLocalEulerAngles(_rotation.getX(), _rotation.getY(), _rotation.getZ());
        _entity->setLocalScale(_size, _size, _size);

        // the outlines run along the edges away from the centre
        const Vector3 outer = Quaternion::fromEulerAngles(_rotation.getX(), _rotation.getY(), _rotation.getZ())
            .invert() * _position;
        const float sx = outer.getX() < 0.0f ? -1.0f : 1.0f;
        const float sz = outer.getZ() < 0.0f ? -1.0f : 1.0f;
        const float t = _outlineThickness / _size;
        for (Entity* outline : _outlines) {
            if (outline) {
                outline->setEnabled(t > 0.0f);
            }
        }
        if (_outlines[0]) {
            _outlines[0]->setLocalPosition(sx * 0.25f * t, 0.0f, sz * 0.5f);
            _outlines[0]->setLocalScale(1.0f + 0.5f * t, t, t);
        }
        if (_outlines[1]) {
            _outlines[1]->setLocalPosition(sx * 0.5f, 0.0f, sz * 0.25f * t);
            _outlines[1]->setLocalScale(t, t, 1.0f + 0.5f * t);
        }
    }

    // ---------------------------------------------------------------- SphereShape

    SphereShape::SphereShape(Engine* engine, const ShapeArgs& args, const float radius)
        : Shape(engine, "sphereCenter", args), _radius(radius)
    {
        addTriData(createSphereGeometry(), 2);
        createRenderComponent(_entity, {createMesh(createSphereGeometry())});
        update();
    }

    void SphereShape::update()
    {
        const float scale = _radius * 2.0f;
        _entity->setLocalScale(scale, scale, scale);
    }

    // ---------------------------------------------------------------- CapsuleShape

    CapsuleShape::CapsuleShape(Engine* engine, const ShapeArgs& args, const float height)
        : Shape(engine, "capsule", args), _height(height)
    {
        addTriData(geometry());
        createRenderComponent(_entity, {createMesh(geometry())});
    }

    PrimitiveGeometry CapsuleShape::geometry() const
    {
        // radius 1, the height caps included
        const float height = std::max(_height, 2.0f);
        return createConeBaseGeometry(1.0f, 1.0f, height - 2.0f, 1, 32, true);
    }

    void CapsuleShape::update()
    {
        replaceMeshes(_entity, {createMesh(geometry())});
    }

    // ---------------------------------------------------------------- ConeShape

    ConeShape::ConeShape(Engine* engine, const ShapeArgs& args, const ConeShapeArgs& coneArgs)
        : Shape(engine, "cone", args), _peakRadius(coneArgs.peakRadius), _segments(coneArgs.segments)
    {
        // hit-testing uses the initial peak radius
        addTriData(geometry());
        _body = createChild(_entity, std::string("body:") + gizmoAxisName(_axis));
        _body->setLocalEulerAngles(0.0f, coneArgs.twist, 0.0f);
        createRenderComponent(_body, {createMesh(geometry())});
    }

    PrimitiveGeometry ConeShape::geometry() const
    {
        return createConeBaseGeometry(0.5f, 0.5f * _peakRadius, 1.0f, 1, _segments, false);
    }

    void ConeShape::update()
    {
        replaceMeshes(_body, {createMesh(geometry())});
    }

    // ---------------------------------------------------------------- GlyphShape

    namespace
    {
        ShapeArgs glyphArgs(ShapeArgs args)
        {
            if (!args.cull) {
                args.cull = CullMode::CULLFACE_NONE;
            }
            return args;
        }
    }

    GlyphShape::GlyphShape(Engine* engine, const ShapeArgs& args, Texture* texture)
        : Shape(engine, "glyph", glyphArgs(args))
    {
        createRenderComponent(_entity, {createMesh(createPlaneGeometry())});
        _material->setCoverageTexture(texture);
        if (_occludedMaterial) {
            _occludedMaterial->setCoverageTexture(texture);
        }
    }
}
