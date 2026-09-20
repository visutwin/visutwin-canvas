// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The metal-cpp *_PRIVATE_IMPLEMENTATION translation unit for a SHARED engine.
//
// metal-cpp is header-only apart from one TU per binary that materializes its
// selector and class globals. Every application linking the STATIC engine
// supplies that TU itself (see examples/exampleApp.cpp), which is why the static
// build never needed one here — but a dylib is linked on its own and must
// resolve those symbols internally, so it compiles this file instead.
//
// Only in a shared build: in a static build this would collide with the
// definition the application already provides. An application that keeps its own
// TU and links the dylib is fine; the two copies live in separate images and
// each only caches sel_registerName results.
#ifdef VISUTWIN_CANVAS_SHARED_BUILD

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <QuartzCore/QuartzCore.hpp>

#endif
