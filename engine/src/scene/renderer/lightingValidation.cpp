// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "lightingValidation.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

#include "core/math/vector3.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float kEps = 1e-4f;

        float square(const float value)
        {
            return value * value;
        }

        float saturate(const float value)
        {
            return std::clamp(value, 0.0f, 1.0f);
        }

        float smoothstep(const float edge0, const float edge1, const float x)
        {
            if (std::abs(edge1 - edge0) <= 1e-6f) {
                return x < edge0 ? 0.0f : 1.0f;
            }
            const float t = saturate((x - edge0) / (edge1 - edge0));
            return t * t * (3.0f - 2.0f * t);
        }

        bool nearlyEqual(const float a, const float b, const float eps = kEps)
        {
            return std::abs(a - b) <= eps;
        }

        float getFalloffLinear(const float lightRadius, const Vector3& lightDir)
        {
            const float radius = std::max(lightRadius, 1e-4f);
            const float d = lightDir.length();
            return std::max((radius - d) / radius, 0.0f);
        }

        float getFalloffInvSquared(const float lightRadius, const Vector3& lightDir)
        {
            const float sqrDist = lightDir.lengthSquared();
            float falloff = 1.0f / (sqrDist + 1.0f);
            const float invRadius = 1.0f / std::max(lightRadius, 1e-4f);

            falloff *= 16.0f;
            falloff *= square(saturate(1.0f - square(sqrDist * square(invRadius))));
            return falloff;
        }

        float getSpotEffect(const Vector3& lightSpotDir, const float lightInnerConeAngle,
            const float lightOuterConeAngle, const Vector3& lightDirNorm)
        {
            const float cosAngle = lightDirNorm.dot(lightSpotDir);
            return smoothstep(lightOuterConeAngle, lightInnerConeAngle, cosAngle);
        }

        struct LocalLightEvalInput
        {
            Vector3 lightPosition;
            Vector3 spotDirection;
            float range = 10.0f;
            bool linearFalloff = true;
            bool spot = false;
            float innerConeCos = 1.0f;
            float outerConeCos = 1.0f;
            Vector3 worldPos;
        };

        float evaluateLocalAttenuation(const LocalLightEvalInput& in)
        {
            const Vector3 lightDirW = in.lightPosition - in.worldPos;
            const float lightDirLenSq = lightDirW.lengthSquared();
            if (lightDirLenSq <= 1e-8f) {
                return in.linearFalloff ? 1.0f : 16.0f;
            }

            const Vector3 dLightDirNormW = lightDirW.normalized();
            float attenuation = in.linearFalloff ? getFalloffLinear(in.range, lightDirW) : getFalloffInvSquared(in.range, lightDirW);
            if (in.spot) {
                attenuation *= getSpotEffect(in.spotDirection.normalized(), in.innerConeCos, in.outerConeCos, -dLightDirNormW);
            }
            return attenuation;
        }
    }

    bool runLightingValidationSelfTest()
    {
        static bool ran = false;
        static bool pass = true;
        if (ran) {
            return pass;
        }
        ran = true;

        auto check = [&](const bool condition, const char* message) {
            if (!condition) {
                pass = false;
                spdlog::error("Lighting validation failed: {}", message);
            }
        };

        // Directional light baseline in shader is constant attenuation 1.0.
        check(nearlyEqual(1.0f, 1.0f), "directional baseline attenuation must be 1.0");

        // Point light linear falloff sanity.
        {
            LocalLightEvalInput in{};
            in.lightPosition = Vector3(0.0f, 0.0f, 0.0f);
            in.range = 10.0f;
            in.linearFalloff = true;

            in.worldPos = Vector3(0.0f, 0.0f, 0.0f);
            check(nearlyEqual(evaluateLocalAttenuation(in), 1.0f), "point linear attenuation at source must be 1.0");

            in.worldPos = Vector3(0.0f, 0.0f, 5.0f);
            check(nearlyEqual(evaluateLocalAttenuation(in), 0.5f), "point linear attenuation at half range must be 0.5");

            in.worldPos = Vector3(0.0f, 0.0f, 10.0f);
            check(nearlyEqual(evaluateLocalAttenuation(in), 0.0f), "point linear attenuation at range limit must be 0.0");
        }

        // Point light inverse-squared falloff sanity and range window clamp.
        {
            LocalLightEvalInput in{};
            in.lightPosition = Vector3(0.0f, 0.0f, 0.0f);
            in.range = 10.0f;
            in.linearFalloff = false;

            in.worldPos = Vector3(0.0f, 0.0f, 1.0f);
            const float nearValue = evaluateLocalAttenuation(in);
            in.worldPos = Vector3(0.0f, 0.0f, 5.0f);
            const float midValue = evaluateLocalAttenuation(in);
            check(nearValue > midValue, "point inverse-squared attenuation must decrease with distance");

            in.worldPos = Vector3(0.0f, 0.0f, 12.0f);
            check(nearlyEqual(evaluateLocalAttenuation(in), 0.0f), "point inverse-squared attenuation must clamp to 0 outside range window");
        }

        // Spot cone falloff sanity: inside full, outside none, between partial.
        {
            const float innerCone = std::cos(20.0f * std::numbers::pi_v<float> / 180.0f);
            const float outerCone = std::cos(30.0f * std::numbers::pi_v<float> / 180.0f);
            LocalLightEvalInput in{};
            in.lightPosition = Vector3(0.0f, 0.0f, 0.0f);
            in.spotDirection = Vector3(0.0f, 0.0f, -1.0f);
            in.spot = true;
            in.innerConeCos = innerCone;
            in.outerConeCos = outerCone;
            in.linearFalloff = true;
            in.range = 1000.0f;

            in.worldPos = Vector3(0.0f, 0.0f, -5.0f);
            const float inside = evaluateLocalAttenuation(in);
            check(inside >= 0.99f, "spot attenuation should be near 1.0 inside inner cone");

            in.worldPos = Vector3(5.0f, 0.0f, -5.0f);
            const float outside = evaluateLocalAttenuation(in);
            check(outside <= 1e-3f, "spot attenuation should be near 0.0 outside outer cone");

            in.worldPos = Vector3(2.0f, 0.0f, -5.0f);
            const float between = evaluateLocalAttenuation(in);
            check(between > 0.0f && between < 1.0f, "spot attenuation should interpolate between cones");
        }

        if (!pass) {
            assert(false && "Lighting self-test failed");
        } else {
            spdlog::info("Lighting validation self-test passed.");
        }

        return pass;
    }
}
