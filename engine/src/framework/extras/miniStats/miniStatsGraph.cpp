// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
#include "miniStatsGraph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        int nextPowerOfTwo(const int value)
        {
            int result = 1;
            while (result < value) {
                result <<= 1;
            }
            return result;
        }
    }

    MiniStatsHistory::MiniStatsHistory(GraphicsDevice* device)
        : _device(device), _pixels(4, 0)
    {
    }

    MiniStatsHistory::~MiniStatsHistory() = default;

    void MiniStatsHistory::ensureSize(const int width, const int rows)
    {
        const int newWidth = std::max(_width, nextPowerOfTwo(std::max(width, 1)));
        const int newHeight = std::max(_height, nextPowerOfTwo(std::max(rows, 1)));
        if (newWidth == _width && newHeight == _height) {
            return;
        }
        std::vector<uint8_t> pixels(static_cast<size_t>(newWidth) * newHeight * 4, 0);
        for (int r = 0; r < _height; ++r) {
            std::memcpy(pixels.data() + static_cast<size_t>(r) * newWidth * 4,
                        _pixels.data() + static_cast<size_t>(r) * _width * 4, static_cast<size_t>(_width) * 4);
        }
        _pixels = std::move(pixels);
        _width = newWidth;
        _height = newHeight;
        _cursor %= _width;
        _ring.clear();
        _ringIndex = -1;
    }

    uint8_t* MiniStatsHistory::row(const int row)
    {
        return _pixels.data() + static_cast<size_t>(row) * _width * 4;
    }

    void MiniStatsHistory::createRing()
    {
        const int count = std::max(_device ? _device->maxFramesInFlight() : 1, 1);
        for (int i = 0; i < count; ++i) {
            TextureOptions options;
            options.width = static_cast<uint32_t>(_width);
            options.height = static_cast<uint32_t>(_height);
            options.format = PixelFormat::PIXELFORMAT_RGBA8;
            options.mipmaps = false;
            options.minFilter = FilterMode::FILTER_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;
            options.name = "mini-stats-history";
            auto texture = std::make_shared<Texture>(_device, options);
            // REPEAT across, so the cursor scrolls the row; clamped down, so a row never
            // samples its neighbour.
            texture->setAddressU(AddressMode::ADDRESS_REPEAT);
            texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            _ring.push_back(std::move(texture));
        }
    }

    Texture* MiniStatsHistory::upload()
    {
        if (_ring.empty()) {
            createRing();
        }
        _ringIndex = (_ringIndex + 1) % static_cast<int>(_ring.size());
        Texture* texture = _ring[static_cast<size_t>(_ringIndex)].get();
        texture->setLevelData(0, _pixels.data(), _pixels.size());
        texture->upload();
        return texture;
    }

    Texture* MiniStatsHistory::current() const
    {
        return _ringIndex >= 0 ? _ring[static_cast<size_t>(_ringIndex)].get() : nullptr;
    }

    MiniStatsGraph::MiniStatsGraph(std::string name_, const float watermark_, const float textRefreshMs_,
                                   Sampler sampler_, const int decimals_, std::string units_)
        : name(std::move(name_)), units(std::move(units_)), decimals(decimals_), watermark(watermark_),
          textRefreshMs(textRefreshMs_), sampler(std::move(sampler_))
    {
        label = name == "DrawCalls" ? "Draw calls" : name;
    }

    std::string MiniStatsGraph::format(const float value) const
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", decimals, static_cast<double>(value));
        return text;
    }

    int MiniStatsGraph::update(const float ms, MiniStatsHistory* history)
    {
        if (headerOnly) {
            return 0;
        }
        if (countOnly) {
            if (history && enabled && row >= 0) {
                updateHistory(static_cast<float>(count), *history);
            }
            return 0;
        }
        float total = sampler ? sampler() : 0.0f;
        if (!std::isfinite(total)) {
            total = 0.0f;
        }
        _avgTotal += total;
        _avgTimer += ms;
        _avgCount++;
        _maxValue = std::max(_maxValue, total);
        int changed = 0;
        if (_avgTimer >= textRefreshMs) {
            const std::string average = format(_avgTotal / static_cast<float>(_avgCount));
            const std::string peak = format(_maxValue);
            changed = (average != timingText ? 1 : 0) | (peak != maxText ? 2 : 0);
            timingText = average;
            maxText = peak;
            _avgTotal = 0.0f;
            _avgTimer = 0.0f;
            _avgCount = 0;
            _maxValue = 0.0f;
        }
        if (history && enabled && row >= 0) {
            updateHistory(total, *history);
        }
        return changed;
    }

    void MiniStatsGraph::updateHistory(const float value, MiniStatsHistory& history)
    {
        uint8_t* data = history.row(row);
        const int width = history.width();
        float range;
        if (countOnly) {
            range = std::max({16.0f, historyRange,
                value > historyRange ? std::exp2(std::ceil(std::log2(value))) : 0.0f});
        } else {
            range = 1.5f * (watermark > 0.0f ? watermark : 100.0f);
        }
        if (needsClear) {
            std::memset(data, 0, static_cast<size_t>(width) * 4);
            needsClear = false;
        } else if (countOnly && range > historyRange) {
            // A count has no fixed budget: its scale grows as needed, and what the row already
            // holds is rescaled, so a change of scale does not look like resources destroyed.
            const float scale = historyRange / range;
            for (int i = 0; i < width; ++i) {
                data[i * 4] = static_cast<uint8_t>(std::lround(static_cast<float>(data[i * 4]) * scale));
            }
        }
        historyRange = range;
        uint8_t* texel = data + static_cast<size_t>(history.cursor()) * 4;
        texel[0] = static_cast<uint8_t>(std::clamp(std::lround(value / range * 255.0f), 0L, 255L));
        texel[3] = countOnly ? 255 : 170;
    }
}
