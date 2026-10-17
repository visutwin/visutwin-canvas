// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.10.2026
//
#include "dynamicSkyNoise.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <numbers>
#include <numeric>
#include <unordered_map>

#include "core/math/blueNoise.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        // JavaScript's integer helpers, which the hashes are written in.
        int32_t toInt32(const int64_t value) { return static_cast<int32_t>(static_cast<uint32_t>(value)); }
        int32_t imul(const int32_t a, const int32_t b)
        {
            return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
        }
        int32_t shiftRight(const int32_t value, const int bits) // >>>
        {
            return static_cast<int32_t>(static_cast<uint32_t>(value) >> bits);
        }
        // Math.round of a non-negative value: halves up, exactly (floor(value + 0.5) rounds a
        // value just below a half up when the addition itself rounds).
        uint8_t roundByte(const double value)
        {
            double whole = std::floor(value);
            if (value - whole >= 0.5) {
                whole += 1.0;
            }
            return static_cast<uint8_t>(whole);
        }

        double hash3(const int x, const int y, const int z, const int seed)
        {
            int32_t h = toInt32(static_cast<int64_t>(imul(x, 374761393)) + imul(y, 668265263) +
                imul(z, static_cast<int32_t>(0x9e3779b1u)) + imul(seed, 1442695041));
            h = imul(h ^ shiftRight(h, 13), 1274126177);
            h ^= shiftRight(h, 16);
            return static_cast<double>(h & 0xffff) / 65535.0;
        }

        // Value noise with a period of px x py lattice cells across the texture, so it tiles.
        double valueNoise(const double u, const double v, const int px, const int py, const int seed)
        {
            const double x = u * px;
            const double y = v * py;
            const double ix = std::floor(x);
            const double iy = std::floor(y);
            const double fx = x - ix;
            const double fy = y - iy;
            const double sx = fx * fx * fx * (fx * (fx * 6 - 15) + 10);
            const double sy = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
            const int x0 = static_cast<int>(ix) % px;
            const int y0 = static_cast<int>(iy) % py;
            const int x1 = (static_cast<int>(ix) + 1) % px;
            const int y1 = (static_cast<int>(iy) + 1) % py;
            const double a = dynamic_sky_noise::hash2(x0, y0, seed);
            const double b = dynamic_sky_noise::hash2(x1, y0, seed);
            const double c = dynamic_sky_noise::hash2(x0, y1, seed);
            const double d = dynamic_sky_noise::hash2(x1, y1, seed);
            return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
        }

        double fractalNoise(const double u, const double v, const int px, const int py, const int octaves,
            const int seed)
        {
            double sum = 0.0;
            double amp = 0.5;
            double norm = 0.0;
            for (int i = 0; i < octaves; ++i) {
                sum += amp * valueNoise(u, v, px << i, py << i, seed + i);
                norm += amp;
                amp *= 0.5;
            }
            return sum / norm;
        }

        // One minus the distance to the nearest of randomly placed points, one per cell.
        double cellularNoise(const double u, const double v, const int period, const int seed)
        {
            const double x = u * period;
            const double y = v * period;
            const int ix = static_cast<int>(std::floor(x));
            const int iy = static_cast<int>(std::floor(y));
            double best = 4.0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int cx = ix + dx;
                    const int cy = iy + dy;
                    const int hx = ((cx % period) + period) % period;
                    const int hy = ((cy % period) + period) % period;
                    const double ox = cx + dynamic_sky_noise::hash2(hx, hy, seed) - x;
                    const double oy = cy + dynamic_sky_noise::hash2(hx, hy, seed + 7) - y;
                    best = std::min(best, ox * ox + oy * oy);
                }
            }
            return 1.0 - std::min(std::sqrt(best), 1.0);
        }

        double stretchNoise(const double value) { return std::min(std::max((value - 0.5) * 2.2 + 0.5, 0.0), 1.0); }

        using Noise3 = std::function<double(double, double, double)>;

        Noise3 cellularNoise3D(const int period, const int seed)
        {
            const int count = period * period * period;
            auto points = std::make_shared<std::vector<float>>(static_cast<size_t>(count) * 3);
            for (int c = 0; c < count; ++c) {
                const int x = c % period;
                const int y = (c / period) % period;
                const int z = c / (period * period);
                (*points)[static_cast<size_t>(c) * 3] = static_cast<float>(hash3(x, y, z, seed));
                (*points)[static_cast<size_t>(c) * 3 + 1] = static_cast<float>(hash3(x, y, z, seed + 1));
                (*points)[static_cast<size_t>(c) * 3 + 2] = static_cast<float>(hash3(x, y, z, seed + 2));
            }
            return [points, period](const double u, const double v, const double w) {
                const double x = u * period;
                const double y = v * period;
                const double z = w * period;
                const int ix = static_cast<int>(std::floor(x));
                const int iy = static_cast<int>(std::floor(y));
                const int iz = static_cast<int>(std::floor(z));
                const double fx = x - ix;
                const double fy = y - iy;
                const double fz = z - iz;
                double best = 3.0;
                for (int dz = -1; dz <= 1; ++dz) {
                    const int cz = (iz + dz + period) % period;
                    for (int dy = -1; dy <= 1; ++dy) {
                        const int row = (cz * period + (iy + dy + period) % period) * period;
                        for (int dx = -1; dx <= 1; ++dx) {
                            const size_t o = static_cast<size_t>(row + (ix + dx + period) % period) * 3;
                            // In double, as upstream's: an int plus a float is a float here.
                            const double ox = dx + static_cast<double>((*points)[o]) - fx;
                            const double oy = dy + static_cast<double>((*points)[o + 1]) - fy;
                            const double oz = dz + static_cast<double>((*points)[o + 2]) - fz;
                            best = std::min(best, ox * ox + oy * oy + oz * oz);
                        }
                    }
                }
                return 1.0 - std::min(std::sqrt(best), 1.0);
            };
        }

        Noise3 gradientNoise3D(const int period, const int seed)
        {
            const int count = period * period * period;
            auto gradients = std::make_shared<std::vector<float>>(static_cast<size_t>(count) * 3);
            for (int c = 0; c < count; ++c) {
                const int x = c % period;
                const int y = (c / period) % period;
                const int z = c / (period * period);
                const double gz = hash3(x, y, z, seed) * 2 - 1;
                const double a = hash3(x, y, z, seed + 1) * std::numbers::pi * 2;
                const double r = std::sqrt(1 - gz * gz);
                (*gradients)[static_cast<size_t>(c) * 3] = static_cast<float>(r * std::cos(a));
                (*gradients)[static_cast<size_t>(c) * 3 + 1] = static_cast<float>(r * std::sin(a));
                (*gradients)[static_cast<size_t>(c) * 3 + 2] = static_cast<float>(gz);
            }
            return [gradients, period](const double u, const double v, const double w) {
                const auto corner = [&](const int ix, const int iy, const int iz, const double x, const double y,
                                        const double z) {
                    const size_t o = static_cast<size_t>(((iz % period) * period + (iy % period)) * period +
                        (ix % period)) * 3;
                    return (*gradients)[o] * x + (*gradients)[o + 1] * y + (*gradients)[o + 2] * z;
                };
                const double x = u * period;
                const double y = v * period;
                const double z = w * period;
                const int ix = static_cast<int>(std::floor(x));
                const int iy = static_cast<int>(std::floor(y));
                const int iz = static_cast<int>(std::floor(z));
                const double fx = x - ix;
                const double fy = y - iy;
                const double fz = z - iz;
                const double sx = fx * fx * fx * (fx * (fx * 6 - 15) + 10);
                const double sy = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
                const double sz = fz * fz * fz * (fz * (fz * 6 - 15) + 10);
                const double n00 = corner(ix, iy, iz, fx, fy, fz);
                const double n10 = corner(ix + 1, iy, iz, fx - 1, fy, fz);
                const double n01 = corner(ix, iy + 1, iz, fx, fy - 1, fz);
                const double n11 = corner(ix + 1, iy + 1, iz, fx - 1, fy - 1, fz);
                const double m00 = corner(ix, iy, iz + 1, fx, fy, fz - 1);
                const double m10 = corner(ix + 1, iy, iz + 1, fx - 1, fy, fz - 1);
                const double m01 = corner(ix, iy + 1, iz + 1, fx, fy - 1, fz - 1);
                const double m11 = corner(ix + 1, iy + 1, iz + 1, fx - 1, fy - 1, fz - 1);
                const double n0 = n00 + (n10 - n00) * sx;
                const double n1 = n01 + (n11 - n01) * sx;
                const double m0 = m00 + (m10 - m00) * sx;
                const double m1 = m01 + (m11 - m01) * sx;
                const double n = n0 + (n1 - n0) * sy;
                const double m = m0 + (m1 - m0) * sy;
                return n + (m - n) * sz;
            };
        }

        // An R8 noise volume, equalized by the cumulative histogram of its values.
        std::vector<uint8_t> noiseVolume(const int size, const Noise3& fill)
        {
            const size_t count = static_cast<size_t>(size) * size * size;
            std::vector<float> values(count);
            // The values are stored as float32, the range taken from them unrounded (upstream's).
            double minValue = std::numeric_limits<double>::infinity();
            double maxValue = -std::numeric_limits<double>::infinity();
            size_t i = 0;
            for (int z = 0; z < size; ++z) {
                for (int y = 0; y < size; ++y) {
                    for (int x = 0; x < size; ++x) {
                        const double value = fill(static_cast<double>(x) / size, static_cast<double>(y) / size,
                            static_cast<double>(z) / size);
                        values[i++] = static_cast<float>(value);
                        minValue = std::min(minValue, value);
                        maxValue = std::max(maxValue, value);
                    }
                }
            }
            constexpr int kBins = 4096;
            std::vector<float> histogram(kBins + 1, 0.0f);
            const double binScale = (kBins - 1) / std::max(maxValue - minValue, 1e-6);
            for (size_t j = 0; j < count; ++j) {
                histogram[static_cast<size_t>(std::max(std::floor((values[j] - minValue) * binScale) + 1.0, 0.0))] += 1.0f;
            }
            for (int b = 1; b <= kBins; ++b) {
                histogram[static_cast<size_t>(b)] += histogram[static_cast<size_t>(b - 1)];
            }
            std::vector<uint8_t> data(count);
            for (size_t j = 0; j < count; ++j) {
                // A value rounded to float32 can land below the range's (double) minimum: bin -1,
                // which upstream reads as undefined, a NaN its Uint8Array stores as 0.
                const double bin = std::floor((values[j] - minValue) * binScale);
                data[j] = bin < 0.0 ? 0
                    : roundByte(static_cast<double>(histogram[static_cast<size_t>(bin)]) / static_cast<double>(count) * 255.0);
            }
            return data;
        }

        std::shared_ptr<Texture> createTexture(GraphicsDevice* device, const char* name, const int size,
            const int layers, const PixelFormat format, const bool mipmaps, const FilterMode minFilter,
            const FilterMode magFilter, const std::vector<uint8_t>& data)
        {
            TextureOptions options;
            options.name = name;
            options.width = static_cast<uint32_t>(size);
            options.height = static_cast<uint32_t>(size);
            options.format = format;
            options.arrayLength = layers > 0 ? static_cast<uint32_t>(layers) : 0u;
            options.mipmaps = mipmaps;
            options.numLevels = mipmaps ? 0 : 1;
            options.minFilter = minFilter;
            options.magFilter = magFilter;
            auto texture = std::make_shared<Texture>(device, options);
            texture->setAddressU(AddressMode::ADDRESS_REPEAT);
            texture->setAddressV(AddressMode::ADDRESS_REPEAT);
            if (layers > 0) {
                const size_t layerBytes = data.size() / static_cast<size_t>(layers);
                for (int layer = 0; layer < layers; ++layer) {
                    texture->setLevelData(0, data.data() + layerBytes * static_cast<size_t>(layer), layerBytes,
                        static_cast<uint32_t>(layer));
                }
            } else {
                texture->setLevelData(0, data.data(), data.size());
            }
            texture->upload();
            return texture;
        }
    }

    namespace dynamic_sky_noise
    {
        double hash2(const int x, const int y, const int seed)
        {
            // Plain products (doubles in upstream, exact at these sizes), wrapped to 32 bits.
            int32_t h = toInt32(static_cast<int64_t>(x) * 374761393 + static_cast<int64_t>(y) * 668265263 +
                static_cast<int64_t>(seed) * 1442695041);
            h = imul(h ^ shiftRight(h, 13), 1274126177);
            h ^= shiftRight(h, 16);
            return static_cast<double>(h & 0xffff) / 65535.0;
        }

        std::vector<uint8_t> cloudMapData(const int size)
        {
            const size_t count = static_cast<size_t>(size) * size;
            std::array<std::vector<float>, 4> channels;
            for (auto& channel : channels) {
                channel.resize(count);
            }
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const double u = static_cast<double>(x) / size;
                    const double v = static_cast<double>(y) / size;
                    const size_t i = static_cast<size_t>(y) * size + x;
                    channels[0][i] = static_cast<float>(stretchNoise(fractalNoise(u, v, 4, 4, 5, 1)));
                    channels[1][i] = static_cast<float>(cellularNoise(u, v, 6, 11) * 0.65 + cellularNoise(u, v, 12, 23) * 0.35);
                    channels[2][i] = static_cast<float>(fractalNoise(u, v, 16, 16, 4, 31));
                    channels[3][i] = static_cast<float>(fractalNoise(u, v, 4, 4, 3, 41));
                }
            }
            // b and a equalized: each value replaced by its rank, so a threshold covers exactly
            // its fraction of the texture (ties keep their order, as upstream's stable sort).
            constexpr std::array<bool, 4> equalize = {false, false, true, true};
            std::vector<uint8_t> data(count * 4);
            std::vector<uint32_t> order(count);
            for (size_t c = 0; c < 4; ++c) {
                const auto& values = channels[c];
                if (equalize[c]) {
                    std::iota(order.begin(), order.end(), 0u);
                    std::stable_sort(order.begin(), order.end(),
                        [&values](const uint32_t a, const uint32_t b) { return values[a] < values[b]; });
                    for (size_t r = 0; r < count; ++r) {
                        data[static_cast<size_t>(order[r]) * 4 + c] =
                            roundByte(static_cast<double>(r) / static_cast<double>(count - 1) * 255.0);
                    }
                } else {
                    for (size_t i = 0; i < count; ++i) {
                        data[i * 4 + c] = roundByte(static_cast<double>(values[i]) * 255.0);
                    }
                }
            }
            return data;
        }

        std::vector<uint8_t> shapeMapData(const int size)
        {
            std::array<Noise3, 4> gradients;
            for (int i = 0; i < 4; ++i) {
                gradients[static_cast<size_t>(i)] = gradientNoise3D(4 << i, 51 + i);
            }
            const auto c4 = cellularNoise3D(4, 61);
            const auto c8 = cellularNoise3D(8, 62);
            const auto c16 = cellularNoise3D(16, 63);
            return noiseVolume(size, [&](const double u, const double v, const double w) {
                double perlin = 0.0;
                double amplitude = 1.0;
                for (const auto& gradient : gradients) {
                    perlin += gradient(u, v, w) * amplitude;
                    amplitude *= 0.5;
                }
                perlin = std::min(std::max(perlin * 0.5 + 0.5, 0.0), 1.0);
                // Perlin-Worley noise, dilated again by the fractal cellular noise.
                const double worley = c4(u, v, w) * 0.625 + c8(u, v, w) * 0.25 + c16(u, v, w) * 0.125;
                const double perlinWorley = (perlin - (worley - 1)) / (2 - worley);
                return (perlinWorley + 1 - worley) / (2 - worley);
            });
        }

        std::vector<uint8_t> detailMapData(const int size)
        {
            const auto c4 = cellularNoise3D(4, 71);
            const auto c8 = cellularNoise3D(8, 72);
            const auto c16 = cellularNoise3D(16, 73);
            return noiseVolume(size, [&](const double u, const double v, const double w) {
                return c4(u, v, w) * 0.625 + c8(u, v, w) * 0.25 + c16(u, v, w) * 0.125;
            });
        }
    }

    DynamicSkyNoise::~DynamicSkyNoise() = default;

    std::shared_ptr<DynamicSkyNoise> DynamicSkyNoise::forDevice(GraphicsDevice* device)
    {
        static std::mutex mutex;
        static std::unordered_map<GraphicsDevice*, std::weak_ptr<DynamicSkyNoise>> cache;
        std::lock_guard lock(mutex);
        auto& entry = cache[device];
        auto noise = entry.lock();
        if (!noise) {
            noise = std::make_shared<DynamicSkyNoise>(device);
            entry = noise;
        }
        return noise;
    }

    Texture* DynamicSkyNoise::cloudMap()
    {
        if (!_cloudMap && _device) {
            _cloudMap = createTexture(_device, "dynamic-sky-cloud-noise", 256, 0, PixelFormat::PIXELFORMAT_RGBA8, true,
                FilterMode::FILTER_LINEAR_MIPMAP_LINEAR, FilterMode::FILTER_LINEAR,
                dynamic_sky_noise::cloudMapData(256));
        }
        return _cloudMap.get();
    }

    Texture* DynamicSkyNoise::shapeMap()
    {
        if (!_shapeMap && _device) {
            _shapeMap = createTexture(_device, "dynamic-sky-cloud-shapes", 64, 64, PixelFormat::PIXELFORMAT_R8, false,
                FilterMode::FILTER_LINEAR, FilterMode::FILTER_LINEAR, dynamic_sky_noise::shapeMapData(64));
        }
        return _shapeMap.get();
    }

    Texture* DynamicSkyNoise::detailMap()
    {
        if (!_detailMap && _device) {
            _detailMap = createTexture(_device, "dynamic-sky-cloud-detail", 32, 32, PixelFormat::PIXELFORMAT_R8, false,
                FilterMode::FILTER_LINEAR, FilterMode::FILTER_LINEAR, dynamic_sky_noise::detailMapData(32));
        }
        return _detailMap.get();
    }

    Texture* DynamicSkyNoise::blueNoise()
    {
        if (!_blueNoise && _device) {
            const std::vector<uint8_t> data(kBlueNoiseRgba32.begin(), kBlueNoiseRgba32.end());
            _blueNoise = createTexture(_device, "BlueNoise32", 32, 0, PixelFormat::PIXELFORMAT_RGBA8, false,
                FilterMode::FILTER_NEAREST, FilterMode::FILTER_NEAREST, data);
        }
        return _blueNoise.get();
    }
}
