// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The transform gizmo (framework/gizmo) on the CPU side:
//
// - TriData picking: the ray is carried into the triangles' space through the shape's
//   world matrix times the TriData transform, and the hit distance is measured back in
//   world units.
// - Shape geometry: the arc (a torus SECTOR of the ring in the shape's XZ plane), the
//   box-line's pick volumes, and the plane's quadrant offset.
// - The viewport scale that keeps the gizmo a constant size on screen.
// - Translate, rotate and scale drags through the gizmo's own pointer entry points,
//   against closed-form answers: a camera on +Z looking down -Z, a 100 x 100 canvas,
//   the attached box at the origin. Nothing renders (a stub device); the drags read
//   only the CPU state the gizmo keeps.
// - Lifetime: an engine destroyed before the gizmo, and an attached node destroyed
//   under it (run it under the sanitize preset).
//
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/gizmo/meshSector.h"
#include "framework/gizmo/rotateGizmo.h"
#include "framework/gizmo/scaleGizmo.h"
#include "framework/gizmo/shape/shapes.h"
#include "framework/gizmo/translateGizmo.h"
#include "framework/gizmo/triData.h"
#include "scene/camera.h"
#include "scene/layer.h"
#include "scene/meshInstance.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-3f;

    std::string str(const Vector3& v)
    {
        return "(" + std::to_string(v.getX()) + ", " + std::to_string(v.getY()) + ", " + std::to_string(v.getZ()) + ")";
    }

    Entity* addEntity(Engine& engine)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        engine.root()->addChild(std::move(owned));
        return entity;
    }

    struct GizmoScene
    {
        std::shared_ptr<Engine> engine;
        CameraComponent* camera = nullptr;
        std::shared_ptr<Layer> layer;
    };

    GizmoScene makeScene()
    {
        GizmoScene scene;
        scene.engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(
            std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.size = {100, 100}, .cpuBuffers = true}));

        Entity* cameraEntity = addEntity(*scene.engine);
        cameraEntity->setLocalPosition(0.0f, 0.0f, 10.0f);   // looking down -Z at the origin
        scene.camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
        scene.camera->camera()->setAspectRatio(1.0f);
        scene.camera->camera()->setFov(45.0f);
        scene.layer = Gizmo::createLayer(scene.engine.get());
        return scene;
    }

    // A point the gizmo sees, in canvas points.
    Vector3 screenOf(const CameraComponent* camera, const Vector3& world)
    {
        return camera->worldToScreen(world);
    }

    float expectedScale()
    {
        return std::tan(22.5f * std::numbers::pi_v<float> / 180.0f) * 10.0f * 0.3f;
    }

    void triDataPicking()
    {
        std::cout << "TriData picking\n";
        TriData box(createBoxGeometry(), 2);
        check(box.tris().size() == 12, "a unit box is twelve triangles");
        check(box.priority() == 2, "the priority is kept");

        // a unit box scaled 2 and lifted to y = 1: its +Z face at z = 1
        box.setTransform(Vector3(0.0f, 1.0f, 0.0f), Quaternion(), Vector3(2.0f));
        float dist = 0.0f;
        bool hit = box.intersect(Matrix4::identity(), Vector3(0.0f, 1.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f), dist);
        check(hit && near(dist, 9.0f, kTolerance), "a ray down -Z hits the scaled face at distance 9 (got " + std::to_string(dist) + ")");
        hit = box.intersect(Matrix4::identity(), Vector3(0.0f, 2.5f, 10.0f), Vector3(0.0f, 0.0f, -1.0f), dist);
        check(!hit, "a ray above the box misses it");

        // the parent world transform composes in front of the TriData transform
        const Matrix4 parent = Matrix4::trs(Vector3(5.0f, 0.0f, 0.0f), Quaternion(), Vector3(0.5f));
        hit = box.intersect(parent, Vector3(5.0f, 0.5f, 10.0f), Vector3(0.0f, 0.0f, -1.0f), dist);
        check(hit && near(dist, 9.5f, kTolerance), "under a parent (x 5, scale 0.5) the face is at z 0.5, distance 9.5 (got " +
            std::to_string(dist) + ")");
    }

    void arcGeometry()
    {
        std::cout << "arc shape geometry\n";
        // the drawn sector: tube 0.01, ring 0.5, half a ring, 80 segments, 20 sides
        const PrimitiveGeometry arc = createTorusGeometry(0.01f, 0.5f, 180.0f, ArcShape::kRenderSegments, 20);
        check(arc.positions.size() / 3 == static_cast<size_t>(21 * (ArcShape::kRenderSegments + 1)),
            "(sides + 1) x (segments + 1) vertices");
        bool onTube = true;
        bool inSector = true;
        for (size_t i = 0; i < arc.positions.size(); i += 3) {
            const float x = arc.positions[i];
            const float y = arc.positions[i + 1];
            const float z = arc.positions[i + 2];
            const float radial = std::sqrt(x * x + z * z) - 0.5f;
            onTube = onTube && near(std::sqrt(radial * radial + y * y), 0.01f, 1e-5f);
            inSector = inSector && z >= -1e-5f;
        }
        check(onTube, "every vertex lies on the tube around the ring");
        check(inSector, "a 180 degree sector covers the +Z half of the XZ plane only");

        // the picking sector is the same ring with the tube widened by the tolerance (0.05)
        TriData sector(createTorusGeometry(0.06f, 0.5f, 180.0f, ArcShape::kIntersectSegments, 20));
        TriData ring(createTorusGeometry(0.06f, 0.5f, 360.0f, ArcShape::kIntersectSegments, 20));
        float dist = 0.0f;
        check(sector.intersect(Matrix4::identity(), Vector3(0.0f, 1.0f, 0.5f), Vector3(0.0f, -1.0f, 0.0f), dist) &&
            near(dist, 0.94f, 2e-3f), "a ray down onto the sector's middle hits the widened tube top (0.94)");
        check(!sector.intersect(Matrix4::identity(), Vector3(0.0f, 1.0f, -0.5f), Vector3(0.0f, -1.0f, 0.0f), dist),
            "the other half of the ring is not part of the sector");
        check(ring.intersect(Matrix4::identity(), Vector3(0.0f, 1.0f, -0.5f), Vector3(0.0f, -1.0f, 0.0f), dist),
            "but is part of the full ring shown while dragging");
    }

    void boxLineAndPlaneShapes(Engine* engine)
    {
        std::cout << "box-line and plane shapes\n";
        ShapeArgs args;
        args.axis = GizmoAxis::Y;
        BoxLineShape line(engine, args);
        check(line.triData().size() == 2 && line.triData()[0]->priority() == 0 && line.triData()[1]->priority() == 1,
            "picked against a box and a priority-1 cylinder");
        const Matrix4& world = line.entity()->worldTransform();
        float dist = 0.0f;
        // box: centre at gap + boxSize / 2 + lineLength = 0.56, size 0.12
        check(line.triData()[0]->intersect(world, Vector3(10.0f, 0.56f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f), dist) &&
            near(dist, 10.0f - 0.06f, kTolerance), "the box sits at the end of the line (y 0.56)");
        // cylinder: radius (lineThickness + tolerance) / 2 = 0.06 around y in [0, 0.5]
        check(line.triData()[1]->intersect(world, Vector3(0.05f, 0.25f, 10.0f), Vector3(0.0f, 0.0f, -1.0f), dist),
            "the line is picked within its tolerance (0.05 off axis)");
        check(!line.triData()[1]->intersect(world, Vector3(0.07f, 0.25f, 10.0f), Vector3(0.0f, 0.0f, -1.0f), dist),
            "and missed beyond it (0.07 off axis)");

        ShapeArgs planeArgs;
        planeArgs.axis = GizmoAxis::Z;
        planeArgs.rotation = Vector3(90.0f, 0.0f, 0.0f);
        PlaneShape plane(engine, planeArgs);
        check(near(plane.position(), Vector3(0.08f, 0.08f, 0.0f), kTolerance),
            "the XY plane sits in the (+x, +y) quadrant");
        plane.setFlipped(Vector3(1.0f, 0.0f, 0.0f));
        check(near(plane.position(), Vector3(-0.08f, 0.08f, 0.0f), kTolerance), "and flips into (-x, +y)");
    }

    void viewportScale()
    {
        std::cout << "viewport scale\n";
        check(near(gizmoViewportScale(true, 45.0f, 10.0f, 0.0f, 1.0f), expectedScale(), 1e-6f),
            "perspective: tan(fov / 2) x forward distance x 0.3");
        check(near(gizmoViewportScale(true, 45.0f, 20.0f, 0.0f, 1.0f), 2.0f * expectedScale(), 1e-6f),
            "twice the distance, twice the world size: a constant screen size");
        check(near(gizmoViewportScale(false, 45.0f, 10.0f, 10.0f, 1.5f), 10.0f * 0.32f * 1.5f, 1e-6f),
            "orthographic: orthoHeight x 0.32 x size");
        check(gizmoViewportScale(true, 45.0f, -1.0f, 0.0f, 1.0f) == 1e-4f, "never below 1e-4");
    }

    void translateDrags()
    {
        std::cout << "translate gizmo\n";
        GizmoScene scene = makeScene();
        Entity* box = addEntity(*scene.engine);
        auto gizmo = std::make_unique<TranslateGizmo>(scene.camera, scene.layer);
        gizmo->attach(box);
        gizmo->prerender();
        const float s = gizmo->worldScale();
        check(near(s, expectedScale(), 1e-5f), "the gizmo's world scale follows the camera (" + std::to_string(s) + ")");
        check(gizmo->enabled(), "attaching enables the gizmo");

        int starts = 0;
        int moves = 0;
        int ends = 0;
        gizmo->on(TransformGizmo::EVENT_TRANSFORMSTART, [&]() { ++starts; });
        gizmo->on(TransformGizmo::EVENT_TRANSFORMMOVE, [&]() { ++moves; });
        gizmo->on(TransformGizmo::EVENT_TRANSFORMEND, [&]() { ++ends; });

        // the X arrow head: gap + arrowLength / 2 + lineLength = 0.59 along +X
        const Vector3 head = screenOf(scene.camera, Vector3(0.59f * s, 0.0f, 0.0f));
        MeshInstance* picked = gizmo->getSelection(head.getX(), head.getY());
        check(picked && gizmo->shape(GizmoAxis::X)->ownsMeshInstance(picked), "the X arrow head is picked");
        const Vector3 centre = screenOf(scene.camera, Vector3(0.0f));
        picked = gizmo->getSelection(centre.getX(), centre.getY());
        check(picked && gizmo->shape(GizmoAxis::XYZ)->ownsMeshInstance(picked),
            "the centre sphere is picked at the centre (priority 2 over the planes)");
        check(!gizmo->shape(GizmoAxis::Z)->entity()->enabledLocal(),
            "the Z arrow, pointing at the camera, is hidden (upstream's glance test)");

        // drag along X by one world unit
        gizmo->pointerDown(head.getX(), head.getY(), 0);
        check(gizmo->selectedAxis() == GizmoAxis::X && gizmo->dragging(), "pressing the arrow selects X");
        check(gizmo->shape(GizmoAxis::Y)->visible() == false && gizmo->shape(GizmoAxis::X)->visible(),
            "drag mode 'selected' leaves only the X arrow shown");
        Vector3 to = screenOf(scene.camera, Vector3(0.59f * s + 1.0f, 0.3f, 0.0f));
        gizmo->pointerMove(to.getX(), to.getY());
        check(near(box->position(), Vector3(1.0f, 0.0f, 0.0f), 2e-3f),
            "moving the pointer one unit right moves the box (1, 0, 0), the off-axis part dropped: " +
            str(box->position()));
        check(near(gizmo->root()->localPosition(), box->position(), 1e-5f), "the gizmo follows the node");

        // snapping to 0.5
        gizmo->snap = true;
        gizmo->snapIncrement = 0.5f;
        to = screenOf(scene.camera, Vector3(0.59f * s + 0.7f, 0.0f, 0.0f));
        gizmo->pointerMove(to.getX(), to.getY());
        check(near(box->position(), Vector3(0.5f, 0.0f, 0.0f), 2e-3f), "snapped: 0.7 rounds to 0.5: " + str(box->position()));
        gizmo->snap = false;

        gizmo->pointerUp(to.getX(), to.getY(), 0);
        check(starts == 1 && moves == 2 && ends == 1, "transform:start, two transform:move, transform:end");
        check(gizmo->shape(GizmoAxis::Y)->visible(), "every shape shows again after the drag");

        // the XY plane handle moves in X and Y together
        box->setPosition(0.0f, 0.0f, 0.0f);
        gizmo->update();
        gizmo->prerender();
        const auto* plane = static_cast<PlaneShape*>(gizmo->shape(GizmoAxis::XY));
        const Vector3 planeCentre = plane->position() * s;
        const Vector3 planeScreen = screenOf(scene.camera, planeCentre);
        picked = gizmo->getSelection(planeScreen.getX(), planeScreen.getY());
        check(picked && plane->ownsMeshInstance(picked), "the XY plane handle is picked at its centre");
        gizmo->pointerDown(planeScreen.getX(), planeScreen.getY(), 0);
        check(gizmo->selectedAxis() == GizmoAxis::Z && gizmo->selectedIsPlane(), "a plane reports its NORMAL axis (Z)");
        to = screenOf(scene.camera, planeCentre + Vector3(0.5f, -0.3f, 0.0f));
        gizmo->pointerMove(to.getX(), to.getY());
        check(near(box->position(), Vector3(0.5f, -0.3f, 0.0f), 2e-3f), "the plane drag moves the box (0.5, -0.3, 0): " +
            str(box->position()));
        gizmo->pointerUp(to.getX(), to.getY(), 0);

        // a disabled shape cannot be dragged
        gizmo->enableShape(GizmoAxis::X, false);
        const Vector3 head2 = screenOf(scene.camera, box->position() + Vector3(0.59f * s, 0.0f, 0.0f));
        gizmo->pointerDown(head2.getX(), head2.getY(), 0);
        check(gizmo->selectedAxis() == GizmoAxis::None, "a disabled shape is not selected");
        gizmo->pointerUp(head2.getX(), head2.getY(), 0);

        gizmo.reset();
        check(scene.camera->layers().back() != scene.layer->id(), "destroying the gizmo takes its layer off the camera");
    }

    void rotateDrag()
    {
        std::cout << "rotate gizmo\n";
        GizmoScene scene = makeScene();
        Entity* box = addEntity(*scene.engine);
        auto gizmo = std::make_unique<RotateGizmo>(scene.camera, scene.layer);
        gizmo->rotationMode = RotateGizmo::RotationMode::Orbit;
        gizmo->attach(box);
        gizmo->prerender();
        const float s = gizmo->worldScale();

        const auto* zArc = static_cast<ArcShape*>(gizmo->shape(GizmoAxis::Z));
        check(zArc->shown() == ArcShape::Show::Ring, "the Z ring faces the camera, so it shows whole");
        check(static_cast<ArcShape*>(gizmo->shape(GizmoAxis::X))->shown() == ArcShape::Show::Sector,
            "the X ring, edge on, shows its half facing the camera");

        // the view ring (radius 0.55) overlaps the Z ring's pick tolerance; take it out
        gizmo->enableShape(GizmoAxis::F, false);

        // press on the Z ring at 45 degrees, drag around it to 75 degrees
        const float r = 0.52f * s;
        const auto onRing = [&](const float degrees) {
            const float a = degrees * std::numbers::pi_v<float> / 180.0f;
            return screenOf(scene.camera, Vector3(r * std::cos(a), r * std::sin(a), 0.0f));
        };
        const Vector3 from = onRing(45.0f);
        MeshInstance* picked = gizmo->getSelection(from.getX(), from.getY());
        check(picked && zArc->ownsMeshInstance(picked), "the Z ring is picked on its rim");
        gizmo->pointerDown(from.getX(), from.getY(), 0);
        check(gizmo->selectedAxis() == GizmoAxis::Z, "pressing the ring selects Z");
        const Vector3 to = onRing(75.0f);
        gizmo->pointerMove(to.getX(), to.getY());

        const Quaternion expected = Quaternion::fromAxisAngle(Vector3(0.0f, 0.0f, 1.0f), 30.0f);
        const Quaternion got = box->rotation();
        check(std::abs(std::abs(got.dot(expected)) - 1.0f) < 1e-4f, "orbit mode: 30 degrees around the ring turn the box 30 degrees about Z");
        const Vector3 guideStart = gizmo->guideAngleStart();
        const Vector3 guideEnd = gizmo->guideAngleEnd();
        check(near(guideEnd, expected * guideStart, 1e-4f) && near(guideStart.length(), 0.5f, 1e-4f),
            "the angle guide ends where the start guide turns by the same angle");

        gizmo->snap = true;   // snap increment 5 degrees, the rotate default
        gizmo->pointerMove(onRing(57.0f).getX(), onRing(57.0f).getY());
        const Quaternion snapped = Quaternion::fromAxisAngle(Vector3(0.0f, 0.0f, 1.0f), 10.0f);
        check(std::abs(std::abs(box->rotation().dot(snapped)) - 1.0f) < 1e-4f, "snapped: 12 degrees round to 10");
        gizmo->pointerUp(to.getX(), to.getY(), 0);

        // absolute mode (the default): a ring facing the camera turns by the mouse
        // displacement projected on (+-1, +-1) / sqrt 2, picked by the quadrant of the press
        // (here right of and above the centre: (-1, -1)), one degree per point
        gizmo->snap = false;
        gizmo->rotationMode = RotateGizmo::RotationMode::Absolute;
        box->setRotation(Quaternion());
        gizmo->update();
        gizmo->pointerMove(from.getX() + 30.0f, from.getY() + 30.0f);   // hover away first
        gizmo->pointerDown(from.getX(), from.getY(), 0);
        gizmo->pointerMove(from.getX() + 10.0f, from.getY());
        const Quaternion absolute = Quaternion::fromAxisAngle(Vector3(0.0f, 0.0f, 1.0f), -10.0f / std::sqrt(2.0f));
        check(std::abs(std::abs(box->rotation().dot(absolute)) - 1.0f) < 1e-5f,
            "absolute mode: 10 points right turn the box -7.07 degrees about Z");
        gizmo->pointerUp(from.getX() + 10.0f, from.getY(), 0);
    }

    void scaleDrags()
    {
        std::cout << "scale gizmo\n";
        GizmoScene scene = makeScene();
        Entity* box = addEntity(*scene.engine);
        auto gizmo = std::make_unique<ScaleGizmo>(scene.camera, scene.layer);
        gizmo->attach(box);
        gizmo->prerender();
        const float s = gizmo->worldScale();
        check(gizmo->coordSpace() == GizmoSpace::Local, "the scale gizmo works in local space");
        gizmo->setCoordSpace(GizmoSpace::World);
        check(gizmo->coordSpace() == GizmoSpace::Local, "and refuses another");

        // the X box-line's box at 0.56 along +X; drag half a gizmo scale further out
        const Vector3 head = screenOf(scene.camera, Vector3(0.56f * s, 0.0f, 0.0f));
        gizmo->pointerDown(head.getX(), head.getY(), 0);
        check(gizmo->selectedAxis() == GizmoAxis::X, "the X box is picked");
        Vector3 to = screenOf(scene.camera, Vector3(1.06f * s, 0.0f, 0.0f));
        gizmo->pointerMove(to.getX(), to.getY());
        check(near(box->localScale(), Vector3(1.5f, 1.0f, 1.0f), 2e-3f),
            "moving out by half the gizmo scale scales X by 1.5: " + str(box->localScale()));
        gizmo->pointerUp(to.getX(), to.getY(), 0);

        // the centre box scales uniformly along the camera's up + right diagonal
        box->setLocalScale(1.0f, 1.0f, 1.0f);
        const Vector3 centre = screenOf(scene.camera, Vector3(0.0f));
        gizmo->pointerDown(centre.getX(), centre.getY(), 0);
        check(gizmo->selectedAxis() == GizmoAxis::XYZ, "the centre box is picked");
        const float a = 0.25f * s;
        to = screenOf(scene.camera, Vector3(a, a, 0.0f));
        gizmo->pointerMove(to.getX(), to.getY());
        const float expected = 1.0f + std::sqrt(2.0f) * a / s;
        check(near(box->localScale(), Vector3(expected), 2e-3f),
            "uniform: |d| along (up + right) / scale + 1 on every axis: " + str(box->localScale()));
        gizmo->pointerUp(to.getX(), to.getY(), 0);
    }

    // The drag, snapping, layer and enabled-state behaviours a single drag of each gizmo
    // does not show.
    // The transform readout and the rotate gizmo's angle sector.
    void readoutAndSector()
    {
        std::cout << "transform readout\n";
        const auto contains = [](const std::string& text, const std::string& part) {
            return text.find(part) != std::string::npos;
        };

        {
            GizmoScene scene = makeScene();
            Entity* box = addEntity(*scene.engine);
            auto gizmo = std::make_unique<TranslateGizmo>(scene.camera, scene.layer);
            gizmo->attach(box);
            gizmo->prerender();
            const float s = gizmo->worldScale();
            check(gizmo->readout() && !gizmo->readoutState().visible, "the readout is on, and hidden until a drag");

            int fired = 0;
            GizmoAxis lastAxis = GizmoAxis::None;
            bool lastPlane = true;
            Vector3 lastValue(9.0f);
            gizmo->on(TransformGizmo::EVENT_TRANSFORMREADOUT, [&](GizmoAxis axis, bool plane, const Vector3& value) {
                ++fired;
                lastAxis = axis;
                lastPlane = plane;
                lastValue = value;
            });

            const Vector3 head = screenOf(scene.camera, Vector3(0.59f * s, 0.0f, 0.0f));
            gizmo->pointerDown(head.getX(), head.getY(), 0);
            check(fired == 1 && lastAxis == GizmoAxis::X && !lastPlane && near(lastValue, Vector3(0.0f), 1e-6f),
                "the drag's start fires transform:readout with X, not a plane, and a zero translation");
            const Vector3 to = screenOf(scene.camera, Vector3(0.59f * s + 1.0f, 0.0f, 0.0f));
            gizmo->pointerMove(to.getX(), to.getY());
            check(fired == 2 && near(lastValue, Vector3(1.0f, 0.0f, 0.0f), 2e-3f),
                "a step fires it with the translation: " + str(lastValue));
            const GizmoReadout& readout = gizmo->readoutState();
            check(readout.visible && contains(readout.markup, "[color=\"#") &&
                      contains(readout.markup, "]X[/color] +1.00") && !contains(readout.markup, "]Y["),
                "the readout shows the X delta, signed, its label in the axis colour: " + readout.markup);
            const Vector3 boxScreen = screenOf(scene.camera, box->position());
            check(readout.y > boxScreen.getY() && std::abs(readout.x - screenOf(scene.camera,
                box->position() + Vector3(0.59f * s * 0.5f, 0.0f, 0.0f)).getX()) < 0.6f * s * 100.0f,
                "it sits below the dragged arrow");
            gizmo->pointerUp(to.getX(), to.getY(), 0);
            check(!gizmo->readoutState().visible, "and hides when the drag ends");

            // a plane moves along the other two axes; the readout shows those two
            box->setPosition(0.0f, 0.0f, 0.0f);
            gizmo->update();
            gizmo->prerender();
            const auto* plane = static_cast<PlaneShape*>(gizmo->shape(GizmoAxis::XY));
            const Vector3 planeCentre = plane->position() * s;
            const Vector3 planeScreen = screenOf(scene.camera, planeCentre);
            gizmo->pointerDown(planeScreen.getX(), planeScreen.getY(), 0);
            const Vector3 planeTo = screenOf(scene.camera, planeCentre + Vector3(0.5f, -0.3f, 0.0f));
            gizmo->pointerMove(planeTo.getX(), planeTo.getY());
            check(lastAxis == GizmoAxis::Z && lastPlane && contains(gizmo->readoutState().markup, "]X[/color] +0.50") &&
                      contains(gizmo->readoutState().markup, "]Y[/color] -0.30") &&
                      !contains(gizmo->readoutState().markup, "]Z["),
                "the XY plane's readout shows X and Y: " + gizmo->readoutState().markup);
            gizmo->pointerUp(planeTo.getX(), planeTo.getY(), 0);

            // off: the event still fires, nothing is shown
            gizmo->setReadout(false);
            box->setPosition(0.0f, 0.0f, 0.0f);
            gizmo->update();
            gizmo->prerender();
            const int before = fired;
            gizmo->pointerDown(head.getX(), head.getY(), 0);
            gizmo->pointerMove(to.getX(), to.getY());
            check(fired == before + 2 && !gizmo->readoutState().visible,
                "with the readout off, transform:readout still fires and nothing is shown");
            gizmo->pointerUp(to.getX(), to.getY(), 0);
        }

        {
            GizmoScene scene = makeScene();
            Entity* box = addEntity(*scene.engine);
            auto gizmo = std::make_unique<ScaleGizmo>(scene.camera, scene.layer);
            gizmo->attach(box);
            gizmo->prerender();
            const float s = gizmo->worldScale();
            Vector3 lastValue(9.0f);
            gizmo->on(TransformGizmo::EVENT_TRANSFORMREADOUT, [&](GizmoAxis, bool, const Vector3& value) {
                lastValue = value;
            });
            const Vector3 head = screenOf(scene.camera, Vector3(0.5f * s, 0.0f, 0.0f));
            gizmo->pointerDown(head.getX(), head.getY(), 0);
            check(near(lastValue, Vector3(1.0f), 1e-6f), "the scale gizmo's start reports a factor of 1");
            const Vector3 to = screenOf(scene.camera, Vector3(0.5f * s + 0.5f * s, 0.0f, 0.0f));
            gizmo->pointerMove(to.getX(), to.getY());
            check(near(lastValue.getX(), box->localScale().getX(), 1e-5f) &&
                      contains(gizmo->readoutState().markup, "]X[/color] \u00d7"),
                "a step reports the scale factor, shown as a multiplier: " + gizmo->readoutState().markup);
            gizmo->pointerUp(to.getX(), to.getY(), 0);
        }

        std::cout << "rotate angle sector\n";
        {
            const auto fan = MeshSector::fanPositions(Vector3(1.0f, 2.0f, 3.0f), Vector3(0.5f, 0.0f, 0.0f),
                Vector3(0.0f, 0.0f, 1.0f), 0.0f, 90.0f, 2.0f, 4);
            bool onCircle = fan.size() == 6;
            for (size_t i = 1; onCircle && i < fan.size(); ++i) {
                onCircle = near((fan[i] - fan[0]).length(), 1.0f, 1e-5f);
            }
            check(onCircle && near(fan[0], Vector3(1.0f, 2.0f, 3.0f), 0.0f) &&
                      near(fan[1], Vector3(2.0f, 2.0f, 3.0f), 1e-5f) && near(fan[5], Vector3(1.0f, 3.0f, 3.0f), 1e-5f),
                "a sector's fan: the centre, then segments + 1 points on the scaled circle from 0 to the sweep");
            const auto back = MeshSector::fanPositions(Vector3(0.0f), Vector3(1.0f, 0.0f, 0.0f),
                Vector3(0.0f, 0.0f, 1.0f), 30.0f, -60.0f, 1.0f, 2);
            check(near(back[1], Vector3(std::cos(0.5236f), std::sin(0.5236f), 0.0f), 1e-4f) &&
                      near(back[3], Vector3(std::cos(-0.5236f), std::sin(-0.5236f), 0.0f), 1e-4f),
                "it starts at `from` and sweeps by a negative angle the other way");

            GizmoScene scene = makeScene();
            Entity* box = addEntity(*scene.engine);
            auto gizmo = std::make_unique<RotateGizmo>(scene.camera, scene.layer);
            gizmo->rotationMode = RotateGizmo::RotationMode::Orbit;
            gizmo->attach(box);
            gizmo->prerender();
            gizmo->enableShape(GizmoAxis::F, false);
            float lastAngle = -1.0f;
            gizmo->on(TransformGizmo::EVENT_TRANSFORMREADOUT, [&](GizmoAxis, bool, const float angle) {
                lastAngle = angle;
            });
            const float r = 0.52f * gizmo->worldScale();
            const auto onRing = [&](const float degrees) {
                const float a = degrees * std::numbers::pi_v<float> / 180.0f;
                return screenOf(scene.camera, Vector3(r * std::cos(a), r * std::sin(a), 0.0f));
            };
            gizmo->pointerDown(onRing(45.0f).getX(), onRing(45.0f).getY(), 0);
            check(lastAngle == 0.0f && !gizmo->guideAngleSector(0)->entity()->enabled(),
                "the start reports 0 degrees, and no sector is filled yet");
            gizmo->pointerMove(onRing(75.0f).getX(), onRing(75.0f).getY());
            check(near(lastAngle, 30.0f, 0.05f) && near(gizmo->guideAngle(), lastAngle, 0.0f) &&
                      near(gizmo->guideAngleAxis(), Vector3(0.0f, 0.0f, 1.0f), 1e-5f),
                "a step reports the angle, which the guide sweeps around the ring's axis");
            check(gizmo->guideAngleSector(0)->entity()->enabled() && !gizmo->guideAngleSector(1)->entity()->enabled(),
                "the swept angle is filled; the rest of the circle only past a full turn");
            check(contains(gizmo->readoutState().markup, "]Z[/color] +30.0\u00b0"),
                "the readout shows the signed angle with the axis: " + gizmo->readoutState().markup);
            gizmo->pointerUp(onRing(75.0f).getX(), onRing(75.0f).getY(), 0);
            check(!gizmo->guideAngleSector(0)->entity()->enabled() && !gizmo->readoutState().visible,
                "the sector and the readout go when the drag ends");
        }
    }

    void hardening()
    {
        std::cout << "hardening\n";
        {
            // a drag ends whatever happened to the gizmo in between
            GizmoScene scene = makeScene();
            Entity* box = addEntity(*scene.engine);
            auto gizmo = std::make_unique<ScaleGizmo>(scene.camera, scene.layer);
            gizmo->attach(box);
            gizmo->prerender();
            const float s = gizmo->worldScale();
            const Vector3 head = screenOf(scene.camera, Vector3(0.56f * s, 0.0f, 0.0f));
            gizmo->pointerDown(head.getX(), head.getY(), 0);
            check(gizmo->selectedAxis() == GizmoAxis::X, "a drag starts on the X box");
            gizmo->setEnabled(false);
            gizmo->pointerUp(head.getX(), head.getY(), 0);
            check(gizmo->selectedAxis() == GizmoAxis::None, "and ends on release though the gizmo was disabled meanwhile");
        }
        {
            // the snap is in scale units: the delta is normalised by the gizmo's scale first
            GizmoScene scene = makeScene();
            Entity* box = addEntity(*scene.engine);
            auto gizmo = std::make_unique<ScaleGizmo>(scene.camera, scene.layer);
            gizmo->snap = true;
            gizmo->snapIncrement = 0.25f;
            gizmo->attach(box);
            gizmo->prerender();
            const float s = gizmo->worldScale();
            const Vector3 head = screenOf(scene.camera, Vector3(0.56f * s, 0.0f, 0.0f));
            gizmo->pointerDown(head.getX(), head.getY(), 0);
            const Vector3 to = screenOf(scene.camera, Vector3(1.16f * s, 0.0f, 0.0f));
            gizmo->pointerMove(to.getX(), to.getY());
            check(near(box->localScale(), Vector3(1.5f, 1.0f, 1.0f), 2e-3f),
                "0.6 of the gizmo scale snaps to 0.5 at an increment of 0.25: " + str(box->localScale()));
            gizmo->pointerUp(to.getX(), to.getY(), 0);
        }
        {
            // a uniform plane drag projects onto its diagonal, normalised
            GizmoScene scene = makeScene();
            Entity* box = addEntity(*scene.engine);
            auto gizmo = std::make_unique<ScaleGizmo>(scene.camera, scene.layer);
            gizmo->setUniform(true);
            gizmo->attach(box);
            gizmo->prerender();
            const float s = gizmo->worldScale();
            const float offset = (gizmo->axisPlaneSize() / 2.0f + gizmo->axisPlaneGap()) * s;
            const Vector3 plane = screenOf(scene.camera, Vector3(offset, offset, 0.0f));
            gizmo->pointerDown(plane.getX(), plane.getY(), 0);
            check(gizmo->selectedAxis() == GizmoAxis::Z && gizmo->selectedIsPlane(), "the XY plane is picked");
            const float a = 0.2f * s;
            const Vector3 to = screenOf(scene.camera, Vector3(offset + a, offset + a, 0.0f));
            gizmo->pointerMove(to.getX(), to.getY());
            const float expected = 1.0f + a / s;
            check(near(box->localScale().getX(), expected, 2e-3f) && near(box->localScale().getY(), expected, 2e-3f),
                "a diagonal move of (a, a) scales X and Y by 1 + a / scale: " + str(box->localScale()));
            gizmo->pointerUp(to.getX(), to.getY(), 0);
        }
        {
            // two gizmos on one camera share the layer; one going does not take it away
            GizmoScene scene = makeScene();
            auto first = std::make_unique<TranslateGizmo>(scene.camera, scene.layer);
            auto second = std::make_unique<RotateGizmo>(scene.camera, scene.layer);
            const auto has = [&] {
                const auto& ids = scene.camera->layers();
                return std::find(ids.begin(), ids.end(), scene.layer->id()) != ids.end();
            };
            first->destroy();
            check(has(), "the camera keeps a shared layer while a gizmo still uses it");
            second->destroy();
            check(!has(), "and drops it with the last");
        }
        {
            // a layer or camera change keeps a disabled gizmo disabled, and moves its meshes
            GizmoScene scene = makeScene();
            Entity* box = addEntity(*scene.engine);
            auto gizmo = std::make_unique<RotateGizmo>(scene.camera, scene.layer);
            gizmo->attach(box);
            gizmo->setEnabled(false);
            const auto other = Gizmo::createLayer(scene.engine.get(), "other");
            gizmo->setLayer(other);
            check(!gizmo->enabled(), "a layer change keeps the gizmo disabled");
            bool moved = true;
            for (auto* render : gizmo->root()->findComponents<RenderComponent>()) {
                moved = moved && render->layers() == std::vector<int>{other->id()};
            }
            check(moved, "and moves every mesh of the gizmo to the new layer");
            Entity* cameraEntity = addEntity(*scene.engine);
            cameraEntity->setLocalPosition(0.0f, 5.0f, 10.0f);   // not on the gizmo, which hides it
            auto* camera2 = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
            gizmo->setCamera(camera2);
            check(!gizmo->enabled(), "so does a camera change");
            gizmo->setEnabled(true);
            check(gizmo->enabled(), "and it enables again on request");
        }
    }

    void lifetime()
    {
        std::cout << "lifetime\n";
        {
            // the engine is destroyed while the gizmo lives on: its
            // destroy event tears the gizmo down, and the later destructor touches nothing
            GizmoScene scene = makeScene();
            Entity* box = addEntity(*scene.engine);
            auto gizmo = std::make_unique<RotateGizmo>(scene.camera, scene.layer);
            gizmo->attach(box);
            scene.engine->destroy();
            check(gizmo->root() == nullptr && gizmo->nodes().empty(), "the engine's destroy event releases the gizmo");
            gizmo.reset();
        }
        {
            // an attached node destroyed under the gizmo is let go
            GizmoScene scene = makeScene();
            Entity* box = addEntity(*scene.engine);
            auto gizmo = std::make_unique<TranslateGizmo>(scene.camera, scene.layer);
            gizmo->attach(box);
            auto owned = box->remove();
            owned.reset();
            check(gizmo->nodes().empty() && !gizmo->enabled(), "a destroyed node detaches the gizmo");
            gizmo->update();
        }
    }
}

int main()
{
    triDataPicking();
    arcGeometry();
    {
        GizmoScene scene = makeScene();
        boxLineAndPlaneShapes(scene.engine.get());
    }
    viewportScale();
    translateDrags();
    rotateDrag();
    scaleDrags();
    readoutAndSector();
    hardening();
    lifetime();

    return finish("gizmo");
}
