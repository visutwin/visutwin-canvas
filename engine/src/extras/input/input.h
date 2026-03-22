// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 02.01.2026
//
#pragma once

namespace visutwin::canvas
{
    /**
     * The base class for all input consumers, which are used to process input frames
     */
    class InputConsumer
    {

    };

    /*
     *  The base class for all input controllers
     */
    class InputController : public InputConsumer
    {
    public:
        virtual void attach(Pose* pose, bool smooth = true) {}
    };
}