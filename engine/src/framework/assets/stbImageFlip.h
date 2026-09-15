// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The one way this engine sets stb_image's vertical-flip flag.
//
// stb_image keeps TWO flip flags: a process-global one
// (stbi_set_flip_vertically_on_load) and a thread-local one
// (stbi_set_flip_vertically_on_load_thread). Once the thread-local flag has been
// set on a thread it overrides the global one there FOR THE REST OF THE THREAD'S
// LIFE, and stb offers no way to unset it. So a loader that clears only the
// global flag is silently ignored after any other loader has set the thread-local
// one: the GLB parser decodes its images flipped, and a bitmap font or an
// environment atlas decoded after it on the same thread came out upside down.
//
// Every decode therefore goes through this scope, which only ever touches the
// thread-local flag. It restores the value the enclosing scope set (stb's default,
// no flip, at the outermost level), so a flip asked for by one loader cannot leak
// into the next decode whether or not that decode names its own orientation.
#pragma once

// Declared rather than included: stb_image.h guards its declarations but NOT its
// implementation, so re-including it in the translation unit that defines
// STB_IMAGE_IMPLEMENTATION (asset.cpp) would compile the implementation twice.
// This matches stb's own declaration, which it wraps in extern "C".
extern "C" void stbi_set_flip_vertically_on_load_thread(int flag_true_if_should_flip);

namespace visutwin::canvas
{
    class StbVerticalFlipScope
    {
    public:
        explicit StbVerticalFlipScope(const bool flip)
            : _previous(_current)
        {
            _current = flip;
            stbi_set_flip_vertically_on_load_thread(flip ? 1 : 0);
        }

        ~StbVerticalFlipScope()
        {
            _current = _previous;
            stbi_set_flip_vertically_on_load_thread(_previous ? 1 : 0);
        }

        StbVerticalFlipScope(const StbVerticalFlipScope&) = delete;
        StbVerticalFlipScope& operator=(const StbVerticalFlipScope&) = delete;
        StbVerticalFlipScope(StbVerticalFlipScope&&) = delete;
        StbVerticalFlipScope& operator=(StbVerticalFlipScope&&) = delete;

        /// The flip the innermost live scope on this thread asked for (false outside any scope).
        static bool current() { return _current; }

    private:
        bool _previous;
        static inline thread_local bool _current = false;
    };
}
