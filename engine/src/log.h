// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.07.2025.
//

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace visutwin::canvas::log
{
    inline void init()
    {
        const auto logger = spdlog::stdout_color_mt("ecs");
        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        spdlog::set_level(spdlog::level::info); // default: info
    }

    inline void set_level_debug()
    {
        spdlog::set_level(spdlog::level::debug);
    }

    inline void set_level_trace()
    {
        spdlog::set_level(spdlog::level::trace);
    }
}
