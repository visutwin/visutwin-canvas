// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#pragma once

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    /**
     * Frame statistics structure
     */
    struct FrameStats {
        float fps = 0.0f;
        float ms = 0.0f;
        float dt = 0.0f;

        double updateStart = 0.0;
        double updateTime = 0.0;
        double fixedUpdateTime = 0.0;
        double renderStart = 0.0;
        double renderTime = 0.0;
        double physicsStart = 0.0;
        double physicsTime = 0.0;
        double cullTime = 0.0;
        double sortTime = 0.0;
        double skinTime = 0.0;
        double morphTime = 0.0;
        double instancingTime = 0.0; // deprecated

        int triangles = 0;
        int gsplats = 0;
        int otherPrimitives = 0;
        int shaders = 0;
        int materials = 0;
        int cameras = 0;
        int shadowMapUpdates = 0;
        double shadowMapTime = 0.0;
        double depthMapTime = 0.0; // deprecated
        double forwardTime = 0.0;

        double lightClustersTime = 0.0;
        int lightClusters = 0;

        double timeToCountFrames = 0.0;
        int fpsAccum = 0;
    };

    /**
     * Draw call statistics structure
     */
    struct DrawCallStats {
        int forward = 0;
        int depth = 0; // deprecated
        int shadow = 0;
        int immediate = 0; // deprecated
        int misc = 0; // everything that is not forward/depth/shadow (post effect quads etc)
        int total = 0; // total = forward + depth + shadow + misc

        // Some of the forward/depth/shadow/misc draw calls:
        int skinned = 0;
        int instanced = 0; // deprecated

        int removedByInstancing = 0; // deprecated
    };

    /**
     * Particle system statistics structure
     */
    struct ParticleStats {
        int updatesPerFrame = 0;
        int _updatesPerFrame = 0;
        double frameTime = 0.0;
        double _frameTime = 0.0;
    };

    struct MiscStats {
        double renderTargetCreationTime = 0.0;
    };

    /**
     * Records performance-related statistics related to the application
     */
    class ApplicationStats
    {
    public:
        ApplicationStats(std::shared_ptr<GraphicsDevice> _graphicsDevice) {}

        DrawCallStats& drawCalls() { return _drawCalls; }

        FrameStats& frame() { return _frame; }

        ParticleStats& particles() { return _particles; }

        MiscStats& misc() { return _misc; }

        void setFrameStats(double now, float dt, float ms);

    private:
        DrawCallStats _drawCalls;

        ParticleStats _particles;

        FrameStats _frame;

        MiscStats _misc;
    };
}
