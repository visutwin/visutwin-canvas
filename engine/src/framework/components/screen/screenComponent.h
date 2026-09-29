// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A UI screen (upstream framework/components/screen/component.js): the rectangle its
// descendant elements are laid out in, and the projection that carries them to clip space
// (screen space) or onto a plane in the world (world space).
//
// Screen units: the screen is `resolution / scale` units across, and an element's model
// space has its origin at the screen's TOP-LEFT corner with y UP (so a position inside the
// screen has y in [-height, 0]); `screenMatrix()` is upstream's
// `ortho(0, w, -h, 0, 1, -1)`, scaled by (w/2, h/2) for a world-space screen.
//
// A screen-space screen takes its resolution from Engine::canvasSize() — window POINTS, the
// unit of upstream's canvas at pixel ratio 1 and of the mouse — and follows it as the
// window resizes (ScreenComponentSystem polls it each update; upstream listens to the
// device's `resizecanvas`).
//
#pragma once

#include <vector>

#include "core/math/matrix4.h"
#include "core/math/vector2.h"
#include "framework/components/component.h"

namespace visutwin::canvas
{
    class ElementComponent;

    /// Upstream SCALEMODE_NONE / SCALEMODE_BLEND.
    enum class ScreenScaleMode
    {
        None,
        Blend
    };

    class ScreenComponent : public Component
    {
    public:
        ScreenComponent(IComponentSystem* system, Entity* entity);
        ~ScreenComponent() override;

        /// Binds the elements already below this entity that have no screen yet (upstream's
        /// `_updateDescendantElements`), so a screen added after its elements still owns them.
        void initializeComponentData() override;
        void cloneFrom(const Component* source) override;

        static const std::vector<ScreenComponent*>& instances() { return _instances; }

        /// A world-space screen takes this as given; a screen-space one ignores it and
        /// follows the canvas.
        const Vector2& resolution() const { return _resolution; }
        void setResolution(const Vector2& value);

        /// The resolution the UI was authored for. With ScreenScaleMode::None it reads back
        /// as the resolution itself, as upstream's getter does.
        const Vector2& referenceResolution() const;
        void setReferenceResolution(const Vector2& value);

        /// Blend is available only to a screen-space screen; a world-space one is None.
        ScreenScaleMode scaleMode() const { return _scaleMode; }
        void setScaleMode(ScreenScaleMode value);

        /// 0 scales by width only, 1 by height only; blended in log space (upstream).
        float scaleBlend() const { return _scaleBlend; }
        void setScaleBlend(float value);

        bool screenSpace() const { return _screenSpace; }
        void setScreenSpace(bool value);

        /// Draw-order priority of this screen's elements, 0-127.
        int priority() const { return _priority; }
        void setPriority(int value);

        float scale() const { return _scale; }
        const Matrix4& screenMatrix() const { return _screenMatrix; }

        /// Called by ScreenComponentSystem when the canvas changes size.
        void onCanvasResize(int width, int height);

        // Elements laid out on this screen (upstream `_bindElement` / `_unbindElement`).
        void bindElement(ElementComponent* element);
        void unbindElement(ElementComponent* element);
        const std::vector<ElementComponent*>& elements() const { return _elements; }

        /// Queue a draw-order sync (upstream `syncDrawOrder`); the screen system resolves it
        /// on its next update, so a burst of hierarchy edits costs one pass.
        void syncDrawOrder() { _drawOrderDirty = true; }
        bool drawOrderDirty() const { return _drawOrderDirty; }
        /// Upstream `_processDrawOrderSync`: every element under the screen entity gets
        /// the next order depth-first from 1, so children draw over their parent and later
        /// siblings over earlier ones.
        void processDrawOrderSync();

    private:
        void updateScale();
        void calcProjectionMatrix();
        void dirtifyEntityLocal();
        void notifyElementsResized();
        Vector2 canvasResolution() const;

        inline static std::vector<ScreenComponent*> _instances;

        Vector2 _resolution = Vector2(640.0f, 320.0f);
        Vector2 _referenceResolution = Vector2(640.0f, 320.0f);
        ScreenScaleMode _scaleMode = ScreenScaleMode::None;
        float _scale = 1.0f;
        float _scaleBlend = 0.5f;
        int _priority = 0;
        bool _screenSpace = false;
        Matrix4 _screenMatrix = Matrix4::identity();
        std::vector<ElementComponent*> _elements;
        bool _drawOrderDirty = true;
    };
}
