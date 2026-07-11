// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 22.12.2025.
//
#include "skinInstance.h"

#include "graphNode.h"
#include "skin.h"

namespace visutwin::canvas
{
    uint64_t SkinInstance::s_frameIndex = 0;

    SkinInstance::SkinInstance(std::shared_ptr<Skin> skin)
        : _skin(std::move(skin))
    {
        if (_skin) {
            _palette.resize(_skin->inverseBindPose().size(), Matrix4::identity());
        }
    }

    void SkinInstance::setBones(std::vector<GraphNode*> bones)
    {
        _bones = std::move(bones);
    }

    void SkinInstance::updateMatrixPalette(GraphNode* rootNode)
    {
        if (_skinUpdateIndex == s_frameIndex) {
            return;
        }
        _skinUpdateIndex = s_frameIndex;

        if (!_skin) {
            return;
        }

        const Matrix4 invRoot = rootNode
            ? rootNode->worldTransform().inverse()
            : Matrix4::identity();

        const auto& inverseBindPose = _skin->inverseBindPose();
        const size_t count = std::min(_bones.size(), inverseBindPose.size());
        for (size_t i = 0; i < count; ++i) {
            // world space -> rootNode space -> bind space (upstream _updateMatrices)
            const Matrix4 boneWorld = _bones[i]
                ? _bones[i]->worldTransform()
                : Matrix4::identity();
            _palette[i] = invRoot * boneWorld * inverseBindPose[i];
        }
    }
}
