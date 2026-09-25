// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Where a camera frame's scene pass stops (RenderPassCameraFrame::findActionIndex).
//
// The scene pass renders up to the grab layer — the Skybox — then grabs the colour, and
// the transparent layers draw after the grab so a refractive surface samples the
// opaque world. Render actions exist only for ENABLED layers, and a scene lit by its
// environment atlas alone disables the Skybox layer: the search used to match nothing,
// the caller fell back to EVERY action, the grab ran after the transparent layers, and
// a refractive surface sampled itself from the previous frame (one of the three stacked
// causes of the dark refraction found on 2026-09-15). A disabled stop layer is placed
// by its POSITION in the composition instead, as upstream's addCameraLayers does.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "framework/entity.h"
#include "scene/composition/layerComposition.h"
#include "scene/composition/renderAction.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassCameraFrame.h"
#include "scene/layer.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    int indexOf(const std::vector<RenderAction*>& actions, const int layerId, const bool transparent)
    {
        for (size_t i = 0; i < actions.size(); ++i) {
            if (actions[i] && actions[i]->layer && actions[i]->layer->id() == layerId &&
                actions[i]->transparent == transparent) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto root = std::make_unique<Entity>();
    root->setEnabledInHierarchy(true);   // a root with no parent: make it active
    auto* camera = static_cast<CameraComponent*>(
        root->addComponentInstance(std::make_unique<CameraComponent>(nullptr, root.get()), 9301));
    camera->initializeComponentData();
    camera->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX, LAYERID_UI});

    // The engine's default order: opaque world, depth, skybox, then transparent world
    // and UI.
    LayerComposition composition;
    auto world = std::make_shared<Layer>("World", LAYERID_WORLD);
    auto depth = std::make_shared<Layer>("Depth", LAYERID_DEPTH);
    auto skybox = std::make_shared<Layer>("Skybox", LAYERID_SKYBOX);
    auto ui = std::make_shared<Layer>("UI", LAYERID_UI);
    composition.pushOpaque(world);
    composition.pushOpaque(depth);
    composition.pushOpaque(skybox);
    composition.pushTransparent(world);
    composition.pushTransparent(ui);

    std::cout << "the grab stop with the Skybox layer enabled\n";
    {
        const auto actions = composition.renderActions();
        check(actions.size() == 5, "one action per enabled sublayer (" + std::to_string(actions.size()) + ")");
        const int stop = RenderPassCameraFrame::findActionIndex(actions, &composition, LAYERID_SKYBOX, false, 0);
        check(stop >= 0 && stop == indexOf(actions, LAYERID_SKYBOX, false), "the scene pass stops at the Skybox's own action");
        check(stop < indexOf(actions, LAYERID_WORLD, true), "before the transparent world");
    }

    std::cout << "\nthe grab stop with the Skybox layer DISABLED\n";
    {
        skybox->setEnabled(false);
        const auto actions = composition.renderActions();
        check(indexOf(actions, LAYERID_SKYBOX, false) == -1, "a disabled layer has no render action");
        const int stop = RenderPassCameraFrame::findActionIndex(actions, &composition, LAYERID_SKYBOX, false, 0);
        const int transparentWorld = indexOf(actions, LAYERID_WORLD, true);
        check(stop >= 0 && transparentWorld >= 0 && stop < transparentWorld,
            "the stop is still placed by the Skybox's POSITION: before the transparent world, so the grab "
            "runs before anything refractive draws (" + std::to_string(stop) + " < " +
            std::to_string(transparentWorld) + ")");
        check(stop == indexOf(actions, LAYERID_DEPTH, false) || stop == indexOf(actions, LAYERID_WORLD, false),
            "and it is the last action at or before that position");
        skybox->setEnabled(true);
    }

    std::cout << "\nedges\n";
    {
        const auto actions = composition.renderActions();
        check(RenderPassCameraFrame::findActionIndex(actions, &composition, 12345, false, 0) ==
              RenderPassCameraFrame::kStopLayerNotInComposition, "a layer not in the composition says so");
        const int uiIndex = indexOf(actions, LAYERID_UI, true);
        check(RenderPassCameraFrame::findActionIndex(actions, &composition, LAYERID_SKYBOX, false, uiIndex) ==
              uiIndex - 1, "searching past the stop renders nothing (fromIndex - 1)");
    }

    std::cout << (failures == 0 ? "\nAll camera frame stop tests passed\n" : "\nCamera frame stop tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
