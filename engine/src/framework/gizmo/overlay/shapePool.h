// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// A pool of gizmo shapes, placed in order each frame and hidden when unused. Each shape draws
// in colours its record owns and the placing code updates in place.
//
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "core/math/color.h"
#include "framework/components/render/renderComponent.h"
#include "framework/entity.h"
#include "framework/gizmo/pool.h"
#include "framework/gizmo/shape/shape.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    struct ShapePoolItem
    {
        Entity* entity = nullptr;
        std::unique_ptr<Shape> shape;
        /// The colours the shape draws in, updated in place.
        Color base;
        Color occluded;
        /// The shape's mesh instances, in draw order, as of the shape's `meshVersion`.
        std::vector<MeshInstance*> meshInstances;
        uint32_t meshVersion = 0;
        /// The draw order the shape was placed at this frame.
        double drawOrder = 0.0;

        /// Collects the mesh instances again (a shape rebuilds them when its geometry changes).
        void collectMeshInstances()
        {
            meshInstances.clear();
            for (auto* render : entity->findComponents<RenderComponent>()) {
                for (MeshInstance* instance : render->meshInstances()) {
                    meshInstances.push_back(instance);
                }
            }
            meshVersion = shape->meshVersion();
        }
    };

    class ShapePool : public Pool<ShapePoolItem>
    {
    public:
        /// Creates a shape drawing in the colours pointed at.
        using Create = std::function<std::unique_ptr<Shape>(const Color* base, const Color* occluded)>;

        ShapePool(const Create& create, Entity* parent, const uint32_t drawOrder)
            : Pool<ShapePoolItem>([create, parent]() {
                  auto item = std::make_unique<ShapePoolItem>();
                  item->shape = create(&item->base, &item->occluded);
                  item->entity = item->shape->entity();
                  if (auto owned = item->shape->takeEntity()) {
                      parent->addChild(std::move(owned));
                  }
                  item->collectMeshInstances();
                  return item;
              }),
              drawOrder(drawOrder)
        {
        }

        /// The draw order the shapes are placed from in a manually sorted layer.
        uint32_t drawOrder = 0;

        /// The next shape, enabled.
        ShapePoolItem& nextShape()
        {
            ShapePoolItem& item = next();
            item.entity->setEnabled(true);
            return item;
        }

        /// Gives the shapes placed this frame their draw orders (their mesh instances collected
        /// again where a shape rebuilt them while it was placed), and hides the rest.
        void finish()
        {
            for (size_t i = 0; i < count; ++i) {
                ShapePoolItem& item = *items()[i];
                if (item.meshVersion != item.shape->meshVersion()) {
                    item.collectMeshInstances();
                }
                for (MeshInstance* instance : item.meshInstances) {
                    instance->setDrawOrder(item.drawOrder);
                }
            }
            hide();
        }

        /// Hides the shapes not placed this frame.
        void hide()
        {
            for (size_t i = count; i < items().size(); ++i) {
                items()[i]->entity->setEnabled(false);
            }
        }

        /// Frees the shapes, calling `release` on each first.
        void destroy(const std::function<void(Shape*)>& release)
        {
            for (auto& item : items()) {
                if (release) {
                    release(item->shape.get());
                }
                if (item->entity && item->entity->parent()) {
                    auto owned = item->entity->remove();
                }
            }
            items().clear();
            count = 0;
        }
    };
}
