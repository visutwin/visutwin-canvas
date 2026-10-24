// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Lines drawn as instanced unit cylinders (programs/gizmo-line.slang), filled each frame between
// begin() and end(). Each line has a visible colour and an occluded colour, drawn first without
// depth test where the scene hides it. Consecutive lines form a run, one instanced draw per pass
// in one draw order; split() starts a new run, so a shape placed between two runs draws between
// them in a manually sorted layer.
//
// A run's instance buffer is written once a frame into one of `maxFramesInFlight()` buffers, so
// no frame still drawing it is overwritten (the frames-in-flight rule).
//
// DEVIATION: upstream pulls each line toward the camera in its vertex stage (0.4% of its view
// distance, so a line on a surface wins its depth test). A mesh draw's vertex stage here gets
// only the view-projection, so the pull is baked into each line's transform for the camera
// `setView` names; it changes depth only, and the occluded pass draws the same pixels.
//
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class Engine;
    class Entity;
    class GraphicsDevice;
    class Material;
    class Mesh;
    class MeshInstance;
    class VertexBuffer;

    class MeshLineBatch
    {
    public:
        /// Lines under `parent` (which must not be rotated or scaled), on layer `layerId`.
        MeshLineBatch(Engine* engine, int layerId, Entity* parent, uint32_t drawOrder);
        ~MeshLineBatch();

        MeshLineBatch(const MeshLineBatch&) = delete;
        MeshLineBatch& operator=(const MeshLineBatch&) = delete;

        /// The draw order of the next run.
        uint32_t drawOrder = 0;

        void setLayer(int layerId);
        int layer() const { return _layerId; }

        /// The camera the lines are pulled toward this frame.
        void setView(const Vector3& eye, const Vector3& forward, bool orthographic);

        void begin();
        /// Starts a new run with the next line.
        void split();
        /// A line between two points relative to the parent, `thickness` wide.
        void add(const Vector3& from, const Vector3& to, float thickness, const Color& color,
            const Color& occludedColor);
        /// Uploads the runs of this frame and hides the rest.
        void end();
        /// Safe to call again.
        void destroy();

        /// The number of lines and runs of the last frame; for tests.
        size_t lineCount() const;
        size_t runCount() const { return _runCount; }

    private:
        struct Run
        {
            Entity* entity = nullptr;
            std::vector<MeshInstance*> meshInstances;
            std::vector<std::shared_ptr<VertexBuffer>> buffers;   // one per frame in flight
            std::vector<int> capacities;
            size_t slot = 0;
            bool written = false;
            int lastKey = 0;
            std::vector<float> data;
            int count = 0;
            Vector3 boundsMin = Vector3(0.0f);
            Vector3 boundsMax = Vector3(0.0f);
        };

        Run& nextRun();
        void upload(Run& run);

        Engine* _engine = nullptr;
        std::shared_ptr<GraphicsDevice> _device;
        Entity* _parent = nullptr;
        int _layerId = 0;
        std::shared_ptr<Material> _material;
        std::shared_ptr<Material> _occludedMaterial;
        std::shared_ptr<Mesh> _mesh;
        std::vector<std::unique_ptr<Run>> _runs;
        size_t _runCount = 0;
        Run* _run = nullptr;
        Matrix4 _pull;
        bool _destroyed = false;
    };
}
