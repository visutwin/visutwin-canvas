// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan uniform binder — stub.
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanUniformBinder.h"

namespace visutwin::canvas
{
    void VulkanUniformBinder::resetPassState()
    {
        _materialBoundThisPass = false;
        _lastBoundMaterial = nullptr;
    }

    bool VulkanUniformBinder::isMaterialChanged(const Material* mat) const
    {
        return !_materialBoundThisPass || mat != _lastBoundMaterial;
    }
}

#endif // VISUTWIN_HAS_VULKAN
