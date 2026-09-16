// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//

#include "lightTextureAtlas.h"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "scene/light.h"
#include "scene/renderer/shadowMap.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/renderTarget.h"

namespace visutwin::canvas
{
    namespace
    {
        // Where each cube face's tile sits in a slot's 3x3 grid (upstream
        // `cubeSlotsOffsets`): faces +X, -X, +Y, -Y, +Z, -Z in the order
        // LightCamera::pointLightRotations renders them. Column = axis, row = sign.
        constexpr int kCubeTileOffsets[6][2] = {
            {0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {2, 1}
        };
    }

    std::vector<int> LightTextureAtlas::automaticSplit(const int lightCount)
    {
        const int gridSize = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(std::max(lightCount, 1)))));
        return {gridSize};
    }

    std::vector<Vector4> LightTextureAtlas::subdivide(const std::vector<int>& split)
    {
        std::vector<Vector4> rects;
        const int splitCount = split.empty() ? 1 : std::max(split[0], 1);
        if (splitCount <= 1) {
            rects.emplace_back(0.0f, 0.0f, 1.0f, 1.0f);
            return rects;
        }
        const float invSize = 1.0f / static_cast<float>(splitCount);
        for (int i = 0; i < splitCount; ++i) {
            for (int j = 0; j < splitCount; ++j) {
                const Vector4 rect(static_cast<float>(i) * invSize, static_cast<float>(j) * invSize, invSize, invSize);
                const size_t nextIndex = 1 + static_cast<size_t>(i * splitCount + j);
                const int nextSplit = nextIndex < split.size() ? split[nextIndex] : 1;
                if (nextSplit > 1) {
                    const float invNext = invSize / static_cast<float>(nextSplit);
                    for (int x = 0; x < nextSplit; ++x) {
                        for (int y = 0; y < nextSplit; ++y) {
                            rects.emplace_back(rect.getX() + static_cast<float>(x) * invNext,
                                rect.getY() + static_cast<float>(y) * invNext, invNext, invNext);
                        }
                    }
                } else {
                    rects.push_back(rect);
                }
            }
        }
        // Largest first: the lights are ranked by screen size and take slots in order.
        std::stable_sort(rects.begin(), rects.end(),
            [](const Vector4& a, const Vector4& b) { return a.getZ() > b.getZ(); });
        return rects;
    }

    Vector4 LightTextureAtlas::omniFaceRect(const Vector4& slot, const int face)
    {
        const int clamped = std::clamp(face, 0, 5);
        const float tile = slot.getZ() / 3.0f;
        return Vector4(slot.getX() + tile * static_cast<float>(kCubeTileOffsets[clamped][0]),
            slot.getY() + tile * static_cast<float>(kCubeTileOffsets[clamped][1]), tile, tile);
    }

    Vector4 LightTextureAtlas::spotViewport(const Vector4& slot, const int resolution, const int edgePixels)
    {
        const float inset = static_cast<float>(edgePixels) / static_cast<float>(std::max(resolution, 1));
        return Vector4(slot.getX() + inset, slot.getY() + inset,
            std::max(slot.getZ() - 2.0f * inset, 0.0f), std::max(slot.getW() - 2.0f * inset, 0.0f));
    }

    Vector2 LightTextureAtlas::cubemapFaceCoordinates(const Vector3& dir, int& faceIndex)
    {
        // Upstream's getCubemapFaceCoordinates with its V term NEGATED: the six face
        // cameras are upstream's rotations, but upstream renders them into bottom-up
        // GL storage and this engine's targets are top-down, so v runs the other way.
        // tests/lightTextureAtlasTests.cpp holds this against the cameras' real
        // projection; it is what found the sign.
        const float ax = std::fabs(dir.getX());
        const float ay = std::fabs(dir.getY());
        const float az = std::fabs(dir.getZ());
        float ma;
        Vector2 uv;
        if (az >= ax && az >= ay) {
            faceIndex = dir.getZ() < 0.0f ? 5 : 4;
            ma = 0.5f / az;
            uv = Vector2(dir.getZ() < 0.0f ? -dir.getX() : dir.getX(), dir.getY());
        } else if (ay >= ax) {
            faceIndex = dir.getY() < 0.0f ? 3 : 2;
            ma = 0.5f / ay;
            uv = Vector2(dir.getX(), dir.getY() < 0.0f ? dir.getZ() : -dir.getZ());
        } else {
            faceIndex = dir.getX() < 0.0f ? 1 : 0;
            ma = 0.5f / ax;
            uv = Vector2(dir.getX() < 0.0f ? dir.getZ() : -dir.getZ(), dir.getY());
        }
        return Vector2(uv.x * ma + 0.5f, uv.y * ma + 0.5f);
    }

    void LightTextureAtlas::configure(const int resolution, const std::vector<int>& atlasSplit)
    {
        _pendingResolution = std::max(1, resolution);
        _pendingSplit = atlasSplit;
    }

    void LightTextureAtlas::ensureCreated()
    {
        if (_texture) {
            return;
        }
        _resolution = _pendingResolution;

        TextureOptions options;
        options.name = "ClusterShadowAtlas";
        options.profilerHint = TexHint::TEXHINT_SHADOWMAP;
        options.width = static_cast<uint32_t>(_resolution);
        options.height = static_cast<uint32_t>(_resolution);
        options.format = PixelFormat::PIXELFORMAT_DEPTH;
        options.mipmaps = false;
        // PCF compares per tap; the shader filters, not the sampler.
        options.minFilter = FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        _texture = std::make_shared<Texture>(_device.get(), options);
        _texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions rt;
        rt.graphicsDevice = _device.get();
        rt.name = "ClusterShadowAtlasRT";
        rt.depthBuffer = _texture.get();
        rt.depth = true;
        _renderTarget = _device->createRenderTarget(rt);

        _shadowMap = ShadowMap::createAtlas(_texture, _renderTarget);
        ++_version;
    }

    void LightTextureAtlas::assignSlot(Light* light, const int slotIndex, const bool reassigned)
    {
        Slot& slot = _slots[static_cast<size_t>(slotIndex)];
        slot.light = light;
        slot.used = true;
        light->setAtlasViewportAllocated(true);
        light->setAtlasViewport(slot.rect);
        if (reassigned) {
            light->setAtlasSlotUpdated(true);
            light->setAtlasVersion(_version);
            light->setAtlasSlotIndex(slotIndex);
            // A one-shot shadow that has already rendered lives in the OLD slot;
            // the new one is blank until it renders once more.
            if (light->shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_NONE) {
                light->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_THISFRAME);
            }
        }
    }

    void LightTextureAtlas::writeViewports(Light* light) const
    {
        const Vector4& slot = light->atlasViewport();
        const bool isOmni = light->type() == LightType::LIGHTTYPE_OMNI;
        const int faceCount = light->numShadowFaces();
        for (int face = 0; face < faceCount; ++face) {
            LightRenderData* rd = light->getRenderData(nullptr, face);
            if (!rd) {
                continue;
            }
            const Vector4 rect = isOmni
                ? omniFaceRect(slot, face)
                : spotViewport(slot, _resolution, kSpotEdgePixels);
            rd->shadowViewport = rect;
            rd->shadowScissor = rect;
        }
    }

    void LightTextureAtlas::update(const std::vector<Light*>& lights)
    {
        ensureCreated();

        // Every light starts the frame unallocated; only an assignment below says
        // otherwise. (A light that was in the atlas last frame and is not in the list
        // now — culled, disabled, shadows off — must not keep claiming a slot.)
        for (auto* light : lights) {
            if (light) {
                light->setAtlasViewportAllocated(false);
                light->setAtlasSlotUpdated(false);
            }
        }

        std::vector<Light*> ranked;
        ranked.reserve(lights.size());
        for (auto* light : lights) {
            if (light) {
                ranked.push_back(light);
            }
        }
        if (ranked.empty()) {
            return;
        }

        // Re-split when the count (or the authored split) changes. Slots are laid out
        // largest first, and a light whose slot is reused keeps its contents.
        const std::vector<int> split = _pendingSplit.empty()
            ? automaticSplit(static_cast<int>(ranked.size())) : _pendingSplit;
        if (split != _split) {
            _split = split;
            ++_version;
            const auto rects = subdivide(_split);
            _slots.clear();
            _slots.reserve(rects.size());
            for (const auto& rect : rects) {
                _slots.push_back(Slot{rect, rect.getZ(), nullptr, false});
            }
        }
        for (auto& slot : _slots) {
            slot.used = false;
        }

        // The lights covering most of the picture take the biggest slots. Stable, so
        // an exact tie keeps the caller's order and the layout does not churn.
        std::stable_sort(ranked.begin(), ranked.end(),
            [](const Light* a, const Light* b) { return a->maxScreenSize() > b->maxScreenSize(); });

        const size_t assignCount = std::min(ranked.size(), _slots.size());
        if (assignCount < ranked.size()) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("LightTextureAtlas: {} lights need a slot but the split holds {} — "
                    "the rest cast no shadow", ranked.size(), _slots.size());
            }
        }

        // First pass: a light whose previous slot is still its own and the same size
        // as the one it would get now keeps it — no re-render for a static light.
        for (size_t i = 0; i < assignCount; ++i) {
            Light* light = ranked[i];
            light->setShadowMap(_shadowMap);
            const int previous = light->atlasSlotIndex();
            if (light->atlasVersion() == _version && previous >= 0 &&
                previous < static_cast<int>(_slots.size())) {
                const Slot& slot = _slots[static_cast<size_t>(previous)];
                if (slot.light == light && !slot.used && slot.size == _slots[i].size) {
                    assignSlot(light, previous, false);
                }
            }
        }

        // Second pass: everyone else takes the next free slot in size order.
        size_t nextSlot = 0;
        for (size_t i = 0; i < assignCount; ++i) {
            Light* light = ranked[i];
            if (light->atlasViewportAllocated()) {
                continue;
            }
            while (nextSlot < _slots.size() && _slots[nextSlot].used) {
                ++nextSlot;
            }
            if (nextSlot >= _slots.size()) {
                break;
            }
            assignSlot(light, static_cast<int>(nextSlot), true);
        }

        for (size_t i = 0; i < assignCount; ++i) {
            if (ranked[i]->atlasViewportAllocated()) {
                writeViewports(ranked[i]);
            }
        }
    }
}
