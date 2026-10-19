// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2026
//
// Spherical-harmonics coefficients of a gaussian splat PLY: how many bands a file's f_rest_*
// count means, and where each coefficient lands in the buffer the shader reads.
//
// The f_rest_* columns are channel-major ([R x n, G x n, B x n] for n coefficients per
// channel) and the shader reads them coefficient-major interleaved, 45 floats a splat for
// bands 1-3 (zero-padded) and 72 for band 4. A count read at the wrong stride mixes the
// channels and tints every splat; a count the loader does not recognise drops the SH and
// leaves the colour flat. Neither is plain in a render, so the values are written here
// with an answer that names their channel and coefficient.

#include <cstdio>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "scene/gsplat/gsplatData.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // The f_rest value of splat s, channel c, coefficient k: unique, and readable back.
    float restValue(const int splat, const int channel, const int coefficient)
    {
        return static_cast<float>(splat * 1000 + channel * 100 + coefficient + 1);
    }

    std::string writePly(const int splats, const int coeffsPerChannel, const char* name)
    {
        const auto path = (std::filesystem::temp_directory_path() / name).string();
        std::ofstream file(path, std::ios::binary);
        file << "ply\n"
             << "format binary_little_endian 1.0\n"
             << "element vertex " << splats << "\n"
             << "property float x\nproperty float y\nproperty float z\n"
             << "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n";
        for (int i = 0; i < coeffsPerChannel * 3; ++i) {
            file << "property float f_rest_" << i << "\n";
        }
        file << "property float opacity\n"
             << "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
             << "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
             << "end_header\n";
        for (int s = 0; s < splats; ++s) {
            std::vector<float> row = {static_cast<float>(s), 0.0f, 0.0f, 0.5f, 0.5f, 0.5f};
            for (int c = 0; c < 3; ++c) {
                for (int k = 0; k < coeffsPerChannel; ++k) {
                    row.push_back(restValue(s, c, k));
                }
            }
            for (const float v : {3.0f, -2.0f, -2.0f, -2.0f, 1.0f, 0.0f, 0.0f, 0.0f}) {
                row.push_back(v);
            }
            file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size() * sizeof(float)));
        }
        return path;
    }

    void checkFile(const int coeffsPerChannel, const int expectedBands)
    {
        constexpr int kSplats = 3;
        char name[64];
        std::snprintf(name, sizeof(name), "visutwin_gsplat_sh_%d.ply", coeffsPerChannel);
        const auto path = writePly(kSplats, coeffsPerChannel, name);
        const auto data = GSplatData::loadPly(path);
        std::filesystem::remove(path);

        char what[160];
        std::snprintf(what, sizeof(what), "%d coefficients per channel load as %d band(s)", coeffsPerChannel,
            expectedBands);
        check(data && data->shBands() == expectedBands, what);
        if (!data || data->shBands() != expectedBands) {
            return;
        }
        if (expectedBands == 0) {
            check(data->shCoeffs().empty(), "an unrecognised count keeps no SH");
            return;
        }

        const int stride = GSplatData::shFloatsPerSplat(expectedBands);
        std::snprintf(what, sizeof(what), "  %d floats a splat", stride);
        check(data->shCoeffs().size() == static_cast<size_t>(kSplats * stride), what);

        bool interleaved = true;
        bool padded = true;
        const auto& sh = data->shCoeffs();
        for (int s = 0; s < kSplats; ++s) {
            for (int k = 0; k < stride / 3; ++k) {
                for (int c = 0; c < 3; ++c) {
                    const float value = sh[static_cast<size_t>(s * stride + k * 3 + c)];
                    if (k < coeffsPerChannel) {
                        interleaved = interleaved && value == restValue(s, c, k);
                    } else {
                        padded = padded && value == 0.0f;
                    }
                }
            }
        }
        check(interleaved, "  every coefficient lands coefficient-major interleaved, in its own channel");
        check(padded, "  the coefficients above the present band are zero");
    }
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "SH bands from the f_rest_* count\n";
    checkFile(3, 1);
    checkFile(8, 2);
    checkFile(15, 3);
    checkFile(24, 4);
    checkFile(10, 0);

    std::cout << "row strides\n";
    check(GSplatData::shFloatsPerSplat(1) == 45 && GSplatData::shFloatsPerSplat(3) == 45,
        "bands 1-3 keep 45 floats a splat");
    check(GSplatData::shFloatsPerSplat(4) == 72, "band 4 takes 72");

    return finish("gaussian splat SH");
}
