// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The splat sorter orders by dot(localCentre, sortDirection). That has to agree
// with the true world-space depth dot(model * localCentre, cameraForward) up to a
// positive scale and a constant offset, or a transformed splat sorts wrongly
// against the rest of the scene.
//
// Transforming the view direction by the inverse model matrix — the older form —
// only satisfies that under a UNIFORM scale, which is what this checks
// (upstream #9268).
//
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "core/math/matrix4.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "scene/gsplat/gsplatInstance.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        if (!condition) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }

    Matrix4 makeTransform(const Vector3& scale, const Vector3& translation, const float yawDegrees)
    {
        return Matrix4::trs(translation,
            Quaternion::fromEulerAngles(0.0f, yawDegrees, 0.0f), scale);
    }

    // Largest deviation between the sorter's key and the true depth, both mapped
    // onto a common scale, over random local points. Zero means the ordering is
    // exact for every camera angle.
    float depthError(const Matrix4& model, const Vector3& cameraForward)
    {
        const Vector3 direction = GSplatInstance::sortDirection(model, cameraForward);

        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> dist(-3.0f, 3.0f);

        std::vector<float> keys, truths;
        for (int i = 0; i < 256; ++i) {
            const Vector3 local(dist(rng), dist(rng), dist(rng));
            const Vector3 world = model.transformPoint(local);
            keys.push_back(local.dot(direction));
            truths.push_back(world.dot(cameraForward));
        }

        // Fit the positive scale and offset the two representations may differ by,
        // then report the residual: any affine agreement is a correct ordering.
        const auto range = [](const std::vector<float>& v) {
            float lo = v[0], hi = v[0];
            for (const float x : v) { lo = std::min(lo, x); hi = std::max(hi, x); }
            return std::pair{lo, hi};
        };
        const auto [keyLo, keyHi] = range(keys);
        const auto [trueLo, trueHi] = range(truths);
        const float keySpan = keyHi - keyLo, trueSpan = trueHi - trueLo;
        if (keySpan < 1e-8f || trueSpan < 1e-8f) {
            return 0.0f;
        }

        float worst = 0.0f;
        for (size_t i = 0; i < keys.size(); ++i) {
            const float normalisedKey = (keys[i] - keyLo) / keySpan;
            const float normalisedTruth = (truths[i] - trueLo) / trueSpan;
            worst = std::max(worst, std::abs(normalisedKey - normalisedTruth));
        }
        return worst;
    }
}

int main()
{
    const Vector3 forward = Vector3(0.3f, -0.5f, 1.0f).normalized();

    struct Case { const char* name; Vector3 scale; };
    const Case cases[] = {
        {"identity",              {1.0f, 1.0f, 1.0f}},
        {"uniform scale",         {2.5f, 2.5f, 2.5f}},
        {"mirrored",              {1.0f, -1.0f, -1.0f}},
        {"flattened to a plane",  {4.0f, 0.2f, 4.0f}},
        {"strongly anisotropic",  {0.1f, 8.0f, 1.0f}},
    };

    for (const auto& [name, scale] : cases) {
        const Matrix4 model = makeTransform(scale, Vector3(7.0f, -2.0f, 3.0f), 35.0f);
        const float error = depthError(model, forward);
        std::printf("%-24s error %.3e\n", name, static_cast<double>(error));
        check(error < 1e-5f, name);
    }

    // The ordering must also hold as the camera swings around: the older form's
    // error is a function of the view direction, so a single angle can hide it.
    const Matrix4 flattened = makeTransform({4.0f, 0.2f, 4.0f}, Vector3(0.0f, 0.0f, 0.0f), 0.0f);
    for (int i = 0; i < 16; ++i) {
        const float angle = static_cast<float>(i) * 6.2831853f / 16.0f;
        const Vector3 direction(std::cos(angle), 0.4f, std::sin(angle));
        check(depthError(flattened, direction.normalized()) < 1e-5f, "swept camera angle");
    }

    if (failures == 0) {
        std::printf("gsplat sort direction: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
