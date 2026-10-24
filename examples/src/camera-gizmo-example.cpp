// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Port of upstream gizmos/camera.
//
// The frustum of a second camera, faint where the scene hides it: a CameraGizmo on a subject
// camera at (-1.5, 1, 2.5) looking at a default box (near 0.5, far 4, fov 45, ortho height 1),
// which renders after the main camera into the bottom-right corner of the window (rect 0.7, 0.02,
// 0.28, 0.28, cleared to 0.2, 0.2, 0.25), framed and labelled "Subject camera". The handles drag
// its near and far clip and its field of view (or orthographic height); the camera stops orbiting
// while one is dragged (zoom 2..20).
//
// Keys stand in for upstream's controls panel:
//   P      main camera projection, perspective / orthographic
//   O      subject camera projection, perspective / orthographic
//   9 / 0  subject fov -/+ 5 (5..120)
//   H      handles on / off
//   R      readout on / off
//   I      icon on / off
//   S      snap on / off (lengths by 1, angles by 5 degrees)
//   [ / ]  line thickness -/+ 0.5 (0.5..8)
//
// DEVIATIONS:
// - No controls panel: keys toggle the settings, and the theme keeps its defaults.
// - The frame and label around the subject's view are UI elements, where upstream's are HTML;
//   the subject camera draws no UI layer, so the gizmo labels stay off its view.
// - The camera controls have no `sceneSize` here; with fly off it changes nothing upstream shows.
//
#include <algorithm>
#include <memory>

#include "../overlayGizmoExample.h"
#include "framework/gizmo/cameraGizmo.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class CameraGizmoExample final : public OverlayGizmoExample
{
public:
    CameraGizmoExample() : OverlayGizmoExample("Camera Gizmo") {}

protected:
    bool create() override
    {
        if (!createStage(4.0f, Vector2(2.0f, 20.0f))) {
            return false;
        }

        _boxMaterial = std::make_shared<StandardMaterial>();
        createPrimitive("box", _boxMaterial.get());

        // the second camera looking at the box, rendered after the main one into the corner
        _subject = new Entity();
        _subject->setEngine(engine());
        _subject->setName("subject");
        _subjectCamera = static_cast<CameraComponent*>(_subject->addComponent<CameraComponent>());
        Camera* camera = _subjectCamera->camera();
        camera->setClearColor(Color(0.2f, 0.2f, 0.25f, 1.0f));
        camera->setRect(kRect);
        camera->setNearClip(0.5f);
        camera->setFarClip(4.0f);
        camera->setFov(45.0f);
        camera->setOrthoHeight(1.0f);
        _subjectCamera->setPriority(1);
        _subjectCamera->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX, LAYERID_IMMEDIATE});
        root()->addChild(_subject);
        _subject->setPosition(Vector3(-1.5f, 1.0f, 2.5f));
        _subject->lookAt(Vector3(0.0f));

        createFrame();

        _gizmo = std::make_unique<CameraGizmo>(_camera, _layer);
        watch(_gizmo.get());
        _gizmo->attach(_subject);

        spdlog::info("Keys: P projection | O subject projection | 9 0 subject fov | H handles | R readout | "
                     "I icon | S snap | [ ] line thickness");
        return true;
    }

    void update(float /*dt*/) override
    {
        handleKeys();
        updateStage(_gizmo ? _gizmo->labels() : std::vector<GizmoReadout>{});
    }

    void destroy() override
    {
        _gizmo.reset();
        destroyStage();
    }

private:
    // the subject's view, measured from the bottom left
    const Vector4 kRect = Vector4(0.7f, 0.02f, 0.28f, 0.28f);

    // A 1-point border around the subject's view and a label in its top-left corner.
    void createFrame()
    {
        if (!_screen) {
            return;
        }
        const Vector4 anchor(kRect.getX(), kRect.getY(), kRect.getX() + kRect.getZ(), kRect.getY() + kRect.getW());
        ElementComponent* frame = createElement(engine(), _screen->entity(), {.type = ElementType::Group,
            .anchor = anchor, .pivot = Vector2(0.0f, 0.0f), .margin = Vector4(0.0f)});
        const Color border(1.0f, 1.0f, 1.0f, 1.0f);
        const auto edge = [&](const Vector4& edgeAnchor, const Vector2& pivot, const float width, const float height) {
            createElement(engine(), frame->entity(), {.type = ElementType::Image, .color = border, .opacity = 0.6f,
                .anchor = edgeAnchor, .pivot = pivot, .margin = Vector4(0.0f), .width = width, .height = height});
        };
        edge(Vector4(0.0f, 1.0f, 1.0f, 1.0f), Vector2(0.5f, 1.0f), 0.0f, 1.0f);
        edge(Vector4(0.0f, 0.0f, 1.0f, 0.0f), Vector2(0.5f, 0.0f), 0.0f, 1.0f);
        edge(Vector4(0.0f, 0.0f, 0.0f, 1.0f), Vector2(0.0f, 0.5f), 1.0f, 0.0f);
        edge(Vector4(1.0f, 0.0f, 1.0f, 1.0f), Vector2(1.0f, 0.5f), 1.0f, 0.0f);

        _fontAsset = std::make_unique<Asset>("frame-font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        if (FontResource* font = _fontAsset->resourceAs<FontResource>()) {
            ElementComponent* label = createElement(engine(), frame->entity(), {.type = ElementType::Image,
                .color = Color(0.0f, 0.0f, 0.0f, 1.0f), .opacity = 0.5f, .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f),
                .pivot = Vector2(0.0f, 1.0f), .width = 10.0f, .height = 10.0f});
            ElementComponent* text = createElement(engine(), label->entity(), {.type = ElementType::Text,
                .color = Color(1.0f, 1.0f, 1.0f, 1.0f), .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
                .pivot = Vector2(0.5f, 0.5f), .font = font, .text = "Subject camera", .fontSize = 12});
            label->setWidth(text->width() + 12.0f);
            label->setHeight(text->height() + 4.0f);
            label->entity()->setLocalPosition(6.0f, -6.0f, 0.0f);
        }
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        Camera* subject = _subjectCamera ? _subjectCamera->camera() : nullptr;
        if (!keyboard || !_gizmo || !subject) {
            return;
        }
        if (keyboard->wasPressed(Key::P)) toggleProjection();
        if (keyboard->wasPressed(Key::O)) {
            subject->setProjection(subject->projection() == ProjectionType::Perspective
                ? ProjectionType::Orthographic : ProjectionType::Perspective);
        }
        if (keyboard->wasPressed(Key::Digit9)) subject->setFov(std::max(subject->fov() - 5.0f, 5.0f));
        if (keyboard->wasPressed(Key::Digit0)) subject->setFov(std::min(subject->fov() + 5.0f, 120.0f));
        if (keyboard->wasPressed(Key::H)) _gizmo->handles = !_gizmo->handles;
        if (keyboard->wasPressed(Key::R)) _gizmo->readout = !_gizmo->readout;
        if (keyboard->wasPressed(Key::I)) _gizmo->icon = !_gizmo->icon;
        if (keyboard->wasPressed(Key::S)) _gizmo->snap = !_gizmo->snap;
        if (keyboard->wasPressed(Key::LeftBracket)) {
            _gizmo->setLineThickness(std::max(_gizmo->lineThickness() - 0.5f, 0.5f));
        }
        if (keyboard->wasPressed(Key::RightBracket)) {
            _gizmo->setLineThickness(std::min(_gizmo->lineThickness() + 0.5f, 8.0f));
        }
    }

    std::shared_ptr<StandardMaterial> _boxMaterial;
    std::unique_ptr<CameraGizmo> _gizmo;
    std::unique_ptr<Asset> _fontAsset;
    Entity* _subject = nullptr;
    CameraComponent* _subjectCamera = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(CameraGizmoExample)
