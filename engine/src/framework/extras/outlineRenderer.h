// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.07.2026.
//
#pragma once

#include <memory>
#include <vector>

#include "core/math/color.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Engine;
    class Entity;
    class Layer;
    class MeshInstance;
    class RenderPassShaderQuad;
    class RenderTarget;
    class StandardMaterial;
    class Texture;

    /**
     * @brief Renders colored outlines around selected entities (port of upstream extras
     * `OutlineRenderer`).
     * @ingroup group_framework
     *
     * A dedicated layer + offscreen camera render flat-color silhouettes of the added
     * entities into an offscreen target; two separable extend passes dilate the
     * silhouettes and mark their edges in alpha; the result is alpha-blended over the
     * back buffer. DEVIATION: upstream re-renders the original mesh instances through a
     * custom shader pass — this port adds unlit *clone* mesh instances (sharing mesh and
     * node) to the outline layer instead, and blends the outline texture after the frame
     * (upstream injects the blend before a chosen layer of the main camera).
     *
     * The extend + blend passes are registered as renderer append passes, so they run
     * inside Engine::render() after all scene render actions (the back buffer is not
     * writable after render() returns — frameEnd presents the drawable).
     *
     * Usage per frame:
     *   outline->frameUpdate(sceneCameraEntity);   // before engine->render()
     *   engine->render();
     */
    class OutlineRenderer
    {
    public:
        /// layerId must not collide with existing layers (defaults 1-5).
        explicit OutlineRenderer(Engine* engine, int layerId = 100);
        ~OutlineRenderer();

        /// Add an entity (and, when recursive, its descendants) with the given outline color.
        void addEntity(Entity* entity, const Color& color, bool recursive = true);

        /// Remove a previously added entity.
        void removeEntity(Entity* entity);

        void removeAllEntities();

        /// Sync the outline camera with the scene camera and resize targets. Call before render().
        void frameUpdate(Entity* sceneCameraEntity);

    private:
        void setPassesRegistered(bool value);
        void collectClones(Entity* entity, const Color& color, bool recursive,
            std::vector<std::unique_ptr<MeshInstance>>& clones,
            std::vector<std::shared_ptr<StandardMaterial>>& materials);
        void resizeTargets(uint32_t width, uint32_t height);

        Engine* _engine = nullptr;

        std::shared_ptr<Layer> _layer;
        Entity* _cameraEntity = nullptr;
        CameraComponent* _cameraComponent = nullptr;

        std::shared_ptr<Texture> _colorTexture;
        std::shared_ptr<Texture> _tempTexture;
        std::shared_ptr<RenderTarget> _renderTarget;
        std::shared_ptr<RenderTarget> _tempRenderTarget;

        std::shared_ptr<RenderPassShaderQuad> _extendHorizontalPass;
        std::shared_ptr<RenderPassShaderQuad> _extendVerticalPass;
        std::shared_ptr<RenderPassShaderQuad> _blendPass;

        struct OutlinedEntity
        {
            Entity* entity = nullptr;
            std::vector<std::unique_ptr<MeshInstance>> clones;
            std::vector<std::shared_ptr<StandardMaterial>> materials;
        };
        std::vector<OutlinedEntity> _outlined;
        bool _passesRegistered = false;
    };
}
