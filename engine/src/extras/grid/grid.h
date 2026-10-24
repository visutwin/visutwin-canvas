// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// An anti-aliased reference grid whose levels of lines follow the zoom (programs/grid.slang).
// It follows the camera and fades out with distance, or is sized by its node's scale when not
// infinite. It lies at its node's position and rotation; spacing sizes the cells.
//
//     auto grid = std::make_unique<Grid>(device);
//     grid->attach(entity);   // the entity's render component draws it
//
// The grid is two mesh instances: the lines, and a pass that writes their depth (only while
// depthWrite is on). Both are blended, drawn first of the transparent draws (draw bucket 255),
// never culled and cast no shadow.
//
// DEVIATIONS:
// - attach() adds the mesh instances to an entity's render component (creating one), which
//   owns them, where upstream hands its own mesh instances to a render component.
// - the depth dither is interleaved gradient noise, not a 32x32 blue noise texture.
// - the line widths are in POINTS (the window's units) where upstream's are CSS pixels; the
//   examples render at one pixel per point, as upstream's do.
//
#pragma once

#include <array>
#include <memory>

#include "core/math/color.h"
#include "core/math/vector2.h"
#include "framework/destroyWatch.h"

namespace visutwin::canvas
{
    class Entity;
    class GraphicsDevice;
    class GridMaterial;
    class MeshInstance;
    class RenderComponent;

    /// The plane of a Grid in its node's local space.
    enum class GridPlane
    {
        XZ,
        XY,
        YZ,
    };

    struct GridTheme
    {
        Color axisX = Color(1.0f, 0.3f, 0.3f, 1.0f);
        Color axisY = Color(0.3f, 1.0f, 0.3f, 1.0f);
        Color axisZ = Color(0.3f, 0.3f, 1.0f, 1.0f);
        Color major = Color(0.8f, 0.8f, 0.8f, 0.6f);
        Color minor = Color(0.6f, 0.6f, 0.6f, 0.4f);
    };

    class Grid
    {
    public:
        explicit Grid(const std::shared_ptr<GraphicsDevice>& device);
        /// Takes the mesh instances out of the render component they were added to.
        ~Grid();

        Grid(const Grid&) = delete;
        Grid& operator=(const Grid&) = delete;

        /// Adds the mesh instances to `entity`'s render component, creating one (no shadows)
        /// when it has none, and takes them out of the one they were in. Null detaches.
        void attach(Entity* entity);

        /// The theme; each colour's alpha is its lines' opacity. Colours are gamma encoded.
        const GridTheme& theme() const { return _theme; }
        void setTheme(const GridTheme& theme);

        /// The plane in the node's local space. XZ by default.
        GridPlane plane() const { return _plane; }
        void setPlane(GridPlane plane);

        /// Whether the lines through the origin take the axis colours. True by default.
        bool showAxes() const { return _showAxes; }
        void setShowAxes(bool value);

        /// Lines black over light parts of the scene and white over dark ones, instead of
        /// their theme colours. Needs the scene colour map
        /// (CameraComponent::requestSceneColorMap). False by default.
        bool contrast() const { return _contrast; }
        void setContrast(bool value);

        /// The cell size the levels are scaled from, in world units. 10 by default.
        float spacing() const { return _spacing; }
        void setSpacing(float value);

        /// How many cells of a level fit across one of the level above, or 1 for a single
        /// fixed level. 10 by default.
        float subdivisions() const { return _subdivisions; }
        void setSubdivisions(float value);

        /// Line widths in points: 1.5, 1 and 2 by default.
        float majorLineThickness() const { return _majorLineThickness; }
        void setMajorLineThickness(float value);
        float minorLineThickness() const { return _minorLineThickness; }
        void setMinorLineThickness(float value);
        float axisLineThickness() const { return _axisLineThickness; }
        void setAxisLineThickness(float value);

        /// The distances from the camera where the grid starts and finishes fading out; the
        /// end is kept within 90% of the far clip. (400, 1000) by default.
        const Vector2& fadeRange() const { return _fadeRange; }
        void setFadeRange(const Vector2& value);

        /// Whether the grid has no edge. When false it is as large as its node's scale along
        /// the plane's axes. True by default.
        bool infinite() const { return _infinite; }
        void setInfinite(bool value);

        /// The fraction of the camera distance the grid's depth moves toward the camera by, so a
        /// coplanar surface does not hide it. 0.001 by default.
        float depthOffset() const { return _depthOffset; }
        void setDepthOffset(float value);

        /// Whether the grid is depth tested against the scene. True by default.
        bool depthTest() const { return _depthTest; }
        void setDepthTest(bool value);

        /// Whether the lines write depth, hiding transparent meshes and gaussian splats behind
        /// them. False by default.
        bool depthWrite() const { return _depthWrite; }
        void setDepthWrite(bool value);

        /// Whether the written depth is dithered by line coverage, so faint lines partly hide
        /// what is behind them. Suits gaussian splats. False by default.
        bool depthDither() const { return _depthDither; }
        void setDepthDither(bool value);

        bool visible() const { return _visible; }
        void setVisible(bool value);

    private:
        void updatePlane();
        void updateShaders();
        void updateVisible();
        void updateBlocks();

        std::shared_ptr<GraphicsDevice> _device;
        // The lines, then the depth pass.
        std::array<std::shared_ptr<GridMaterial>, 2> _materials;
        std::array<MeshInstance*, 2> _meshInstances{};
        RenderComponent* _render = nullptr;
        DestroyWatch _entityWatch;

        GridTheme _theme;
        GridPlane _plane = GridPlane::XZ;
        bool _showAxes = true;
        bool _contrast = false;
        float _spacing = 10.0f;
        float _subdivisions = 10.0f;
        float _majorLineThickness = 1.5f;
        float _minorLineThickness = 1.0f;
        float _axisLineThickness = 2.0f;
        Vector2 _fadeRange = Vector2(400.0f, 1000.0f);
        bool _infinite = true;
        float _depthOffset = 0.001f;
        bool _depthTest = true;
        bool _depthWrite = false;
        bool _depthDither = false;
        bool _visible = true;
    };
}
