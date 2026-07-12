// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <string>
#include <vector>

#include <core/math/matrix4.h>
#include <core/shape/boundingBox.h>

namespace visutwin::canvas
{
    /**
     * A skin contains data about the bones in a hierarchy that drive a skinned mesh animation.
     * Specifically, the skin stores the bone name and inverse bind matrix for each bone.
     * Inverse bind matrices are instrumental in the mathematics of vertex skinning.
     *
     * Constant between clones — shared by all SkinInstance objects created from it.
     */
    class Skin
    {
    public:
        Skin(std::vector<Matrix4> inverseBindPose, std::vector<std::string> boneNames)
            : _inverseBindPose(std::move(inverseBindPose)),
              _boneNames(std::move(boneNames))
        {
        }

        const std::vector<Matrix4>& inverseBindPose() const { return _inverseBindPose; }
        const std::vector<std::string>& boneNames() const { return _boneNames; }
        int boneCount() const { return static_cast<int>(_inverseBindPose.size()); }

        /**
         * Per-bone AABBs of the bind-space vertices each bone influences (weight
         * above threshold), computed by the parser. Transforming bone i's AABB by
         * `bone[i].worldTransform * inverseBindPose[i]` and unioning yields a valid
         * world-space bound for the skinned mesh in any pose — this is what enables
         * frustum culling of skinned mesh instances.
         */
        void setBoneAabbs(std::vector<BoundingBox> aabbs, std::vector<uint8_t> used)
        {
            _boneAabbs = std::move(aabbs);
            _boneAabbUsed = std::move(used);
        }
        const std::vector<BoundingBox>& boneAabbs() const { return _boneAabbs; }
        const std::vector<uint8_t>& boneAabbUsed() const { return _boneAabbUsed; }
        bool hasBoneAabbs() const { return !_boneAabbs.empty(); }

    private:
        std::vector<Matrix4> _inverseBindPose;
        std::vector<std::string> _boneNames;
        std::vector<BoundingBox> _boneAabbs;
        std::vector<uint8_t> _boneAabbUsed;
    };
}
