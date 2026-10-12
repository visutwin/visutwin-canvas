// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
#include "cameraFrameEffect.h"

#include <atomic>
#include <utility>

namespace visutwin::canvas
{
    namespace
    {
        std::atomic<uint64_t> nextEffectSerial{1};
    }

    CameraFrameEffect::CameraFrameEffect(std::string id, std::vector<FrameResource> requiredResources)
        : _id(std::move(id)), _requiredResources(std::move(requiredResources)), _serial(nextEffectSerial.fetch_add(1))
    {
    }
}
