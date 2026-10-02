// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <cstdint>
#include <map>
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

    /// Which property a curve drives, decided once when the curve is added from its
    /// propertyPath, so evaluation compares an enum rather than strings.
    enum class AnimProperty : uint8_t
    {
        Unknown = 0,
        Position,   // "localPosition"
        Rotation,   // "localRotation"
        Scale,      // "localScale"
        Weights     // "weights"
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

        // Filled by AnimTrack::addCurve; whatever the caller wrote here is replaced.
        size_t targetIndex = 0;                         // index into AnimTrack::targets()
        AnimProperty property = AnimProperty::Unknown;  // from propertyPath
    };

    /**
     * One animation event (upstream AnimEvents entry): fired on the AnimComponent under
     * `name` when playback passes `time` (seconds). Upstream events carry any extra
     * properties; here they are strings in `properties`.
     */
    struct AnimEvent
    {
        std::string name;
        float time = 0.0f;
        std::map<std::string, std::string> properties;
    };

    class AnimTrack;

    /// What an anim event handler receives: `entity.anim.on(name, [](const AnimEventFired&) {})`.
    struct AnimEventFired
    {
        const AnimTrack* track = nullptr;
        const AnimEvent* event = nullptr;
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

        /// Upstream `AnimTrack.events`: kept sorted by time, as AnimEvents sorts them.
        const std::vector<AnimEvent>& events() const { return _events; }
        void setEvents(std::vector<AnimEvent> events);

        // Curve-based API. addCurve gives the curve its target index (one per distinct
        // nodeName, in first-seen order) and its property.
        void addCurve(const AnimCurve& curve);
        void addInput(const AnimData& input) { _inputs.push_back(input); }
        void addOutput(const AnimData& output) { _outputs.push_back(output); }

        const std::vector<AnimCurve>& curves() const { return _curves; }
        const std::vector<AnimData>& inputs() const { return _inputs; }
        const std::vector<AnimData>& outputs() const { return _outputs; }

        /// The distinct node paths the curves drive, in first-seen order.
        const std::vector<std::string>& targets() const { return _targets; }

        /// Process-unique identity of this track's target list: an evaluator caches how
        /// the track's targets map onto its own nodes under it. A copy gets a new one.
        uint64_t serial() const { return _serial.value; }

        /**
         * Evaluates every curve at `time` into `out[curve.targetIndex]`, one entry per
         * target. A target some curve evaluated is marked in `touched` (as the map the
         * old signature filled gained an entry); the caller clears the has* flags of the
         * entries and the marks before the call. Nothing is allocated here and no string
         * is hashed or compared: this runs per clip per frame.
         */
        void eval(float time, AnimTransform* out, uint8_t* touched) const;

    private:
        static float hermite(float t, float p0, float m0, float p1, float m1);

        struct Serial
        {
            uint64_t value;
            Serial();
            Serial(const Serial&);
            Serial& operator=(const Serial&) { return *this; }   // an assigned-to track keeps its own
        };

        std::string _name;
        float _duration = 0.0f;
        std::vector<AnimCurve> _curves;
        std::vector<AnimData> _inputs;    // keyframe time arrays (shared across curves)
        std::vector<AnimData> _outputs;   // value arrays
        std::vector<std::string> _targets;
        std::vector<AnimEvent> _events;
        std::unordered_map<std::string, size_t> _targetLookup;   // addCurve only
        Serial _serial;
    };
}
