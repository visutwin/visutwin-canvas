// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <string>
#include <vector>

#include <core/math/matrix4.h>

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

    private:
        std::vector<Matrix4> _inverseBindPose;
        std::vector<std::string> _boneNames;
    };
}
