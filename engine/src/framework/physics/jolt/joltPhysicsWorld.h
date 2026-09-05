// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Jolt-backed PhysicsWorld. Built only when VISUTWIN_PHYSICS_JOLT is on; the
// header is safe to include either way, but the factory returns null without it.
//
// An application opts in by handing the result to AppOptions::physicsWorld:
//
//     options.physicsWorld = createJoltPhysicsWorld();
//
// Nothing in the engine constructs one, the same rule component systems follow.
#pragma once

#include <memory>

#include "framework/physics/physicsWorld.h"

namespace visutwin::canvas
{
    /// A Jolt-backed world, or null when the engine was built without
    /// VISUTWIN_PHYSICS_JOLT. Logs an error in that case rather than failing
    /// silently, because the caller's scene will simply not move.
    std::shared_ptr<PhysicsWorld> createJoltPhysicsWorld();
}
