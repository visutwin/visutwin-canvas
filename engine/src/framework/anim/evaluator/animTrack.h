// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math/quaternion.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    struct AnimTransform
    {
        Vector3 position = Vector3(0.0f);
        Quaternion rotation = Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
        Vector3 scale = Vector3(1.0f);
        bool hasPosition = false;
        bool hasRotation = false;
        bool hasScale = false;
        // Morph target weights (glTF "weights" channels) — one float per target.
        std::vector<float> weights;
        bool hasWeights = false;
    };

    // AnimData wraps a flat float array + component count.
    //
    struct AnimData
    {
        int components = 1;          // values per element (1=scalar, 3=vec3, 4=quat)
        std::vector<float> data;     // flat float array

        size_t count() const { return components > 0 ? data.size() / static_cast<size_t>(components) : 0; }
    };

    //
    enum class AnimInterpolation : uint8_t
    {
        STEP = 0,       // ANIM_INTERPOLATION_STEP
        LINEAR = 1,     // ANIM_INTERPOLATION_LINEAR
        CUBIC = 2       // ANIM_INTERPOLATION_CUBIC
    };

    // AnimCurve links input times → output values for one property of one node.
    struct AnimCurve
    {
        // Target node, resolved by the AnimBinder. The GLB parser writes a PATH of
        // node names from the glTF root down, joined with '/' ("Root/Arm/Hand"), so
        // two nodes sharing a name in different branches stay distinct; a bare name
        // still resolves by findByName. Also the key the evaluator blends on.
        std::string nodeName;
        std::string propertyPath;      // "localPosition", "localRotation", "localScale", "weights"
        size_t inputIndex = 0;         // index into AnimTrack::_inputs
        size_t outputIndex = 0;        // index into AnimTrack::_outputs
        AnimInterpolation interpolation = AnimInterpolation::LINEAR;
    };

    class AnimTrack
    {
    public:
        AnimTrack() = default;
        AnimTrack(std::string name, float duration);

        const std::string& name() const { return _name; }
        void setName(const std::string& value) { _name = value; }

        float duration() const { return _duration; }
        void setDuration(float value) { _duration = value; }

        // Curve-based API
        void addCurve(const AnimCurve& curve) { _curves.push_back(curve); }
        void addInput(const AnimData& input) { _inputs.push_back(input); }
        void addOutput(const AnimData& output) { _outputs.push_back(output); }

        const std::vector<AnimCurve>& curves() const { return _curves; }
        const std::vector<AnimData>& inputs() const { return _inputs; }
        const std::vector<AnimData>& outputs() const { return _outputs; }

        // eval() signature unchanged — AnimClip/AnimEvaluator don't change.
        void eval(float time, std::unordered_map<std::string, AnimTransform>& transforms) const;

    private:
        static float hermite(float t, float p0, float m0, float p1, float m1);

        std::string _name;
        float _duration = 0.0f;
        std::vector<AnimCurve> _curves;
        std::vector<AnimData> _inputs;    // keyframe time arrays (shared across curves)
        std::vector<AnimData> _outputs;   // value arrays
    };
}
