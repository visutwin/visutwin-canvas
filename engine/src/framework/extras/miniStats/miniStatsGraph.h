// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;

    /**
     * The performance HUD's history: one RGBA8 row per graph, one column per frame, kept
     * on the CPU and uploaded once a frame. The write column (the cursor) is shared by
     * every row and advances once a frame while graphs are shown.
     *
     * A texel's r is the sample over its row's scale and a marks a sample: its value is
     * the height of the budget line (170 for timings, whose scale is 1.5 budgets; 255 for
     * counts, whose scale has no budget). A cleared texel draws nothing.
     *
     * The upload goes to the next of maxFramesInFlight() textures in turn: on Metal a
     * texture upload is a copy into memory the GPU reads directly, and the frames still in
     * flight read the textures uploaded before.
     */
    class MiniStatsHistory
    {
    public:
        explicit MiniStatsHistory(GraphicsDevice* device);
        ~MiniStatsHistory();

        MiniStatsHistory(const MiniStatsHistory&) = delete;
        MiniStatsHistory& operator=(const MiniStatsHistory&) = delete;

        /// Grows to at least `width` columns and `rows` rows, each a power of two, keeping
        /// every row's samples.
        void ensureSize(int width, int rows);
        [[nodiscard]] int width() const { return _width; }
        [[nodiscard]] int height() const { return _height; }

        /// Row `row`'s texels, width() x 4 bytes.
        [[nodiscard]] uint8_t* row(int row);
        [[nodiscard]] const std::vector<uint8_t>& pixels() const { return _pixels; }

        [[nodiscard]] int cursor() const { return _cursor; }
        void advance() { _cursor = (_cursor + 1) % _width; }

        /// Copies the rows into the next texture of the ring and returns it.
        Texture* upload();
        /// The texture uploaded last (null before the first upload).
        [[nodiscard]] Texture* current() const;

    private:
        void createRing();

        GraphicsDevice* _device;
        int _width = 1;
        int _height = 1;
        std::vector<uint8_t> _pixels;
        std::vector<std::shared_ptr<Texture>> _ring;
        int _ringIndex = -1;
        int _cursor = 0;
    };

    /**
     * One row of the performance HUD: a counter averaged over the text refresh window,
     * with its peak, and its per-frame history. A heading row (headerOnly) has no value
     * of its own; a resource row (countOnly) shows a count that is set, not sampled, and
     * keeps no average or peak.
     */
    class MiniStatsGraph
    {
    public:
        /// The value this frame: a sum of whatever the counter adds up (CPU: update plus
        /// render).
        using Sampler = std::function<float()>;

        MiniStatsGraph(std::string name, float watermark, float textRefreshMs, Sampler sampler,
                       int decimals = 0, std::string units = {});

        /// Feeds one frame of `ms` milliseconds. With `history`, the sample is also written
        /// into this row's column at the history's cursor. Returns a bitmask: 1 when the
        /// average text changed, 2 when the peak text did.
        int update(float ms, MiniStatsHistory* history);

        /// Writes `value` into this row's column at the cursor, clearing the row first if it
        /// is new, and growing a count row's scale (rescaling what it holds) when needed.
        void updateHistory(float value, MiniStatsHistory& history);

        /// `value` with this row's decimals.
        [[nodiscard]] std::string format(float value) const;

        std::string name;
        std::string label;
        std::string units;
        int decimals = 0;
        float watermark = 100.0f;
        float textRefreshMs = 500.0f;
        Sampler sampler;

        bool enabled = false;          // keeps history (a size with graphs)
        std::string timingText = "\xE2\x80\x94";
        std::string maxText = "\xE2\x80\x94";

        int row = -1;                  // history row, -1 for none
        bool needsClear = true;
        int group = 0;
        MiniStatsGraph* parent = nullptr;
        bool headerOnly = false;
        bool countOnly = false;
        int count = 0;
        float historyRange = 0.0f;

        // Where the last rebuild put this row's heading, for clicks (points from the bottom).
        float headerTop = 0.0f;
        float headerBottom = 0.0f;
        int quad = -1;
        int lastNonZeroFrame = 0;
        std::string statName;

    private:
        float _avgTotal = 0.0f;
        float _avgTimer = 0.0f;
        int _avgCount = 0;
        float _maxValue = 0.0f;
    };
}
