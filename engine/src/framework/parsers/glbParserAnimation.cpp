// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// glTF parser: skins, skin attributes, morph targets and animations.
#include "glbParserInternal.h"

#include "glbParser.h"
#include "framework/parsers/packedVertex.h"

#include "framework/components/light/lightComponent.h"

#include <algorithm>
#include <atomic>
#include <numbers>
#include <mutex>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/shape/boundingBox.h"
#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "core/math/vector3.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/textureMips.h"
#include "platform/graphics/textureUtils.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    using namespace gltf;

    namespace gltf
    {
        SkinAttributes readSkinAttributes(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const size_t vertexCount)
        {
            SkinAttributes out;
            if (!primitive.attributes.contains("JOINTS_0") ||
                !primitive.attributes.contains("WEIGHTS_0")) {
                return out;
            }
            const auto* jointsAccessor = getAccessor(model, primitive.attributes.at("JOINTS_0"));
            const auto* weightsAccessor = getAccessor(model, primitive.attributes.at("WEIGHTS_0"));
            if (!jointsAccessor || !weightsAccessor ||
                jointsAccessor->type != TINYGLTF_TYPE_VEC4 ||
                weightsAccessor->type != TINYGLTF_TYPE_VEC4 ||
                static_cast<size_t>(jointsAccessor->count) < vertexCount ||
                static_cast<size_t>(weightsAccessor->count) < vertexCount) {
                spdlog::warn("GLB skin attributes present but malformed — skinning skipped");
                return out;
            }
            // Joints are integers and weights unit fractions whatever the accessor's
            // normalized flag says; the spec allows only these component types.
            const int jointsType = jointsAccessor->componentType;
            const int weightsType = weightsAccessor->componentType;
            if ((jointsType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                 jointsType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) ||
                (weightsType != TINYGLTF_COMPONENT_TYPE_FLOAT &&
                 weightsType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                 weightsType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)) {
                return out;
            }
            const AccessorReader joints(model, *jointsAccessor, TINYGLTF_TYPE_VEC4, false);
            const AccessorReader weights(model, *weightsAccessor, TINYGLTF_TYPE_VEC4, true);
            if (!joints.valid() || !weights.valid()) {
                return out;
            }

            out.joints.resize(vertexCount * 4);
            out.weights.resize(vertexCount * 4);
            for (size_t i = 0; i < vertexCount; ++i) {
                joints.read(i, out.joints.data() + i * 4, 4);
                float w[4] = {0, 0, 0, 0};
                weights.read(i, w, 4);
                // Renormalize (quantized weights rarely sum to exactly 1); a degenerate
                // all-zero vertex binds fully to its first joint instead of collapsing
                // to the origin.
                const float sum = w[0] + w[1] + w[2] + w[3];
                if (sum > 1e-6f) {
                    const float inv = 1.0f / sum;
                    for (float& c : w) c *= inv;
                } else {
                    w[0] = 1.0f;
                }
                for (int c = 0; c < 4; ++c) {
                    out.weights[i * 4 + static_cast<size_t>(c)] = w[c];
                }
            }
            out.valid = true;
            return out;
        }

        /// Interleave base vertices with skin influences into the 88-byte layout.
        std::vector<uint8_t> packSkinnedVertices(const std::vector<PackedVertex>& vertices,
            const SkinAttributes& skin)
        {
            std::vector<uint8_t> bytes(vertices.size() * SKINNED_VERTEX_STRIDE);
            for (size_t i = 0; i < vertices.size(); ++i) {
                auto* dst = bytes.data() + i * SKINNED_VERTEX_STRIDE;
                std::memcpy(dst, &vertices[i], sizeof(PackedVertex));
                std::memcpy(dst + sizeof(PackedVertex), skin.weights.data() + i * 4, 4 * sizeof(float));
                std::memcpy(dst + sizeof(PackedVertex) + 4 * sizeof(float), skin.joints.data() + i * 4, 4 * sizeof(float));
            }
            return bytes;
        }

        /// Read glTF primitive morph targets (POSITION/NORMAL deltas).
        std::vector<MorphTarget> readMorphTargets(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const size_t vertexCount)
        {
            std::vector<MorphTarget> targets;
            for (size_t t = 0; t < primitive.targets.size(); ++t) {
                const auto& target = primitive.targets[t];
                MorphTarget morphTarget;
                morphTarget.name = "target_" + std::to_string(t);

                const auto posIt = target.find("POSITION");
                if (posIt != target.end()) {
                    if (const auto* accessor = getAccessor(model, posIt->second)) {
                        readFloatArray(model, *accessor, morphTarget.deltaPositions);
                    }
                }
                if (morphTarget.deltaPositions.size() < vertexCount * 3) {
                    spdlog::warn("GLB morph target {} has incomplete POSITION deltas — skipped", t);
                    continue;
                }
                const auto nrmIt = target.find("NORMAL");
                if (nrmIt != target.end()) {
                    if (const auto* accessor = getAccessor(model, nrmIt->second)) {
                        readFloatArray(model, *accessor, morphTarget.deltaNormals);
                    }
                }
                targets.push_back(std::move(morphTarget));
            }
            return targets;
        }

        /// Parse glTF skins into container payloads (inverse bind matrices + joint indices).
        void parseSkins(const tinygltf::Model& model, GlbContainerResource* container)
        {
            const std::vector<std::string> nodeNames = glbNodeNames(model, glbNodeParents(model));
            for (const auto& skin : model.skins) {
                GlbSkinPayload payload;
                payload.jointNodeIndices = skin.joints;

                std::vector<Matrix4> inverseBindPose;
                inverseBindPose.reserve(skin.joints.size());
                std::vector<float> ibmData;
                if (skin.inverseBindMatrices >= 0) {
                    if (const auto* accessor = getAccessor(model, skin.inverseBindMatrices)) {
                        readFloatArray(model, *accessor, ibmData);
                    }
                }
                for (size_t j = 0; j < skin.joints.size(); ++j) {
                    if (ibmData.size() >= (j + 1) * 16) {
                        // glTF matrices are column-major — same as Matrix4.
                        const float* m = ibmData.data() + j * 16;
                        inverseBindPose.emplace_back(
                            Vector4(m[0], m[1], m[2], m[3]),
                            Vector4(m[4], m[5], m[6], m[7]),
                            Vector4(m[8], m[9], m[10], m[11]),
                            Vector4(m[12], m[13], m[14], m[15]));
                    } else {
                        inverseBindPose.push_back(Matrix4::identity());
                    }
                }

                std::vector<std::string> boneNames;
                boneNames.reserve(skin.joints.size());
                for (const int jointNodeIndex : skin.joints) {
                    boneNames.push_back(
                        (jointNodeIndex >= 0 && jointNodeIndex < static_cast<int>(model.nodes.size()))
                            ? nodeNames[static_cast<size_t>(jointNodeIndex)] : std::string());
                }

                payload.skin = std::make_shared<Skin>(std::move(inverseBindPose), std::move(boneNames));

                // Per-bone AABBs over the bind-space vertices each joint influences.
                // Enables frustum culling of skinned meshes: at runtime the world
                // bound is the union of bone AABBs transformed by
                // bone.worldTransform * inverseBind (see MeshInstance::aabb()).
                {
                    const size_t jointCount = skin.joints.size();
                    std::vector<Vector3> mins(jointCount, Vector3(std::numeric_limits<float>::max()));
                    std::vector<Vector3> maxs(jointCount, Vector3(std::numeric_limits<float>::lowest()));
                    std::vector<uint8_t> used(jointCount, 0);
                    const int skinIndex = static_cast<int>(container->skinPayloadCount());

                    for (const auto& node : model.nodes) {
                        if (node.skin != skinIndex || node.mesh < 0 ||
                            node.mesh >= static_cast<int>(model.meshes.size())) {
                            continue;
                        }
                        for (const auto& primitive : model.meshes[static_cast<size_t>(node.mesh)].primitives) {
                            const auto posIt = primitive.attributes.find("POSITION");
                            if (posIt == primitive.attributes.end()) {
                                continue;
                            }
                            const auto* posAccessor = getAccessor(model, posIt->second);
                            if (!posAccessor) {
                                continue;
                            }
                            std::vector<float> positions;
                            if (!readFloatArray(model, *posAccessor, positions)) {
                                continue;
                            }
                            const size_t vertexCount = positions.size() / 3;
                            const auto attributes = readSkinAttributes(model, primitive, vertexCount);
                            if (attributes.joints.size() < vertexCount * 4 ||
                                attributes.weights.size() < vertexCount * 4) {
                                continue;
                            }
                            // A morphed skin: each vertex can also move by its morph deltas, so
                            // the bone boxes take the vertex's reach under every target at once
                            // — the negative deltas summed toward the min, the positive toward
                            // the max, per axis. Without it
                            // a skinned mesh whose targets push it outward could be culled on screen.
                            const auto morphTargets = readMorphTargets(model, primitive, vertexCount);
                            for (size_t v = 0; v < vertexCount; ++v) {
                                const Vector3 rest = Vector3::load(&positions[v * 3]);
                                Vector3 reachMin = rest;
                                Vector3 reachMax = rest;
                                for (const auto& target : morphTargets) {
                                    const Vector3 delta = Vector3::load(&target.deltaPositions[v * 3]);
                                    reachMin += Vector3::min(delta, Vector3(0.0f, 0.0f, 0.0f));
                                    reachMax += Vector3::max(delta, Vector3(0.0f, 0.0f, 0.0f));
                                }
                                for (int k = 0; k < 4; ++k) {
                                    const float weight = attributes.weights[v * 4 + static_cast<size_t>(k)];
                                    if (weight <= 1e-4f) {
                                        continue;
                                    }
                                    const auto joint = static_cast<size_t>(attributes.joints[v * 4 + static_cast<size_t>(k)]);
                                    if (joint >= jointCount) {
                                        continue;
                                    }
                                    used[joint] = 1;
                                    mins[joint] = Vector3::min(mins[joint], reachMin);
                                    maxs[joint] = Vector3::max(maxs[joint], reachMax);
                                }
                            }
                        }
                    }

                    std::vector<BoundingBox> boneAabbs(jointCount);
                    bool anyUsed = false;
                    for (size_t j = 0; j < jointCount; ++j) {
                        if (!used[j]) {
                            continue;
                        }
                        anyUsed = true;
                        boneAabbs[j].setCenter((mins[j] + maxs[j]) * 0.5f);
                        boneAabbs[j].setHalfExtents((maxs[j] - mins[j]) * 0.5f);
                    }
                    if (anyUsed) {
                        payload.skin->setBoneAabbs(std::move(boneAabbs), std::move(used));
                    }
                }

                container->addSkinPayload(payload);
            }
            if (!model.skins.empty()) {
                spdlog::info("  Parsed {} skin(s)", model.skins.size());
            }
        }

        // Parse glTF animations into AnimTrack objects stored on the container.
        //
        // Overload: output animation tracks to a map (thread-safe — no container needed).
        void parseAnimations(const tinygltf::Model& model,
            AnimTrackList& outTracks)
        {
            if (model.animations.empty()) {
                return;
            }

            const std::vector<int> nodeParents = glbNodeParents(model);
            const std::vector<std::string> nodeNames = glbNodeNames(model, nodeParents);

            for (size_t animIdx = 0; animIdx < model.animations.size(); ++animIdx) {
                const auto& anim = model.animations[animIdx];

                std::string trackName = anim.name.empty()
                    ? ("animation_" + std::to_string(animIdx))
                    : anim.name;

                float duration = 0.0f;
                auto track = std::make_shared<AnimTrack>();

                for (const auto& channel : anim.channels) {
                    if (channel.target_node < 0 ||
                        channel.target_node >= static_cast<int>(model.nodes.size())) {
                        continue;
                    }
                    if (channel.sampler < 0 ||
                        channel.sampler >= static_cast<int>(anim.samplers.size())) {
                        continue;
                    }

                    const auto& sampler = anim.samplers[channel.sampler];
                    const auto* inputAccessor = getAccessor(model, sampler.input);
                    const auto* outputAccessor = getAccessor(model, sampler.output);
                    if (!inputAccessor || !outputAccessor) {
                        continue;
                    }

                    // Map glTF target path to the animation property path.
                    std::string propertyPath;
                    int outputComponents = 0;
                    if (channel.target_path == "translation") {
                        propertyPath = "localPosition";
                        outputComponents = 3;
                    } else if (channel.target_path == "rotation") {
                        propertyPath = "localRotation";
                        outputComponents = 4;
                    } else if (channel.target_path == "scale") {
                        propertyPath = "localScale";
                        outputComponents = 3;
                    } else if (channel.target_path == "weights") {
                        // Morph target weights: one output component per target of the
                        // node's mesh (glTF: output count = keyCount * targetCount).
                        propertyPath = "weights";
                        const auto& targetNode = model.nodes[static_cast<size_t>(channel.target_node)];
                        if (targetNode.mesh < 0 ||
                            targetNode.mesh >= static_cast<int>(model.meshes.size())) {
                            continue;
                        }
                        const auto& mesh = model.meshes[static_cast<size_t>(targetNode.mesh)];
                        if (mesh.primitives.empty() || mesh.primitives[0].targets.empty()) {
                            continue;
                        }
                        outputComponents = static_cast<int>(mesh.primitives[0].targets.size());
                    } else {
                        continue;
                    }

                    // Map interpolation mode.
                    AnimInterpolation interpMode = AnimInterpolation::LINEAR;
                    if (sampler.interpolation == "STEP") {
                        interpMode = AnimInterpolation::STEP;
                    } else if (sampler.interpolation == "CUBICSPLINE") {
                        interpMode = AnimInterpolation::CUBIC;
                    }

                    // Read input (keyframe times).
                    AnimData inputData;
                    inputData.components = 1;
                    if (!readFloatArray(model, *inputAccessor, inputData.data)) {
                        continue;
                    }

                    // Track max time for duration.
                    if (!inputData.data.empty()) {
                        duration = std::max(duration, inputData.data.back());
                    }

                    // Read output (values).
                    AnimData outputData;
                    if (interpMode == AnimInterpolation::CUBIC) {
                        // CUBICSPLINE stores 3 values per keyframe: [inTangent, value, outTangent].
                        // The accessor has count == keyframe_count, but each element has
                        // 3 * outputComponents floats.
                        outputData.components = outputComponents;
                        if (!readFloatArray(model, *outputAccessor, outputData.data)) {
                            continue;
                        }
                    } else {
                        outputData.components = outputComponents;
                        if (!readFloatArray(model, *outputAccessor, outputData.data)) {
                            continue;
                        }
                    }

                    // Quaternion winding normalization for rotation channels.
                    // Ensures shortest-path slerp: if dot(q[i], q[i+1]) < 0, negate q[i+1].
                    if (propertyPath == "localRotation" && interpMode != AnimInterpolation::CUBIC) {
                        const size_t quatCount = outputData.count();
                        for (size_t i = 1; i < quatCount; ++i) {
                            const size_t prev = (i - 1) * 4;
                            const size_t curr = i * 4;
                            const float dot = outputData.data[prev] * outputData.data[curr] +
                                              outputData.data[prev + 1] * outputData.data[curr + 1] +
                                              outputData.data[prev + 2] * outputData.data[curr + 2] +
                                              outputData.data[prev + 3] * outputData.data[curr + 3];
                            if (dot < 0.0f) {
                                outputData.data[curr]     = -outputData.data[curr];
                                outputData.data[curr + 1] = -outputData.data[curr + 1];
                                outputData.data[curr + 2] = -outputData.data[curr + 2];
                                outputData.data[curr + 3] = -outputData.data[curr + 3];
                            }
                        }
                    }

                    // Target node as a name path (see glbNodePath); an unnamed node
                    // takes the same fallback name its entity is instantiated under.
                    const std::string nodeName = glbNodePath(nodeNames, nodeParents, channel.target_node);

                    // Create curve referencing the input/output by index.
                    const size_t inputIndex = track->inputs().size();
                    const size_t outputIndex = track->outputs().size();

                    track->addInput(std::move(inputData));
                    track->addOutput(std::move(outputData));

                    AnimCurve curve;
                    curve.nodeName = nodeName;
                    curve.propertyPath = propertyPath;
                    curve.inputIndex = inputIndex;
                    curve.outputIndex = outputIndex;
                    curve.interpolation = interpMode;
                    track->addCurve(curve);
                }

                // Two animations may share a name (nothing in glTF forbids it), and the
                // tracks are keyed by name, so the later one would overwrite the earlier
                // in silence. Keep both, the later under a suffixed name, and say so.
                if (outTracks.contains(trackName)) {
                    std::string unique;
                    for (int n = 1; unique.empty() || outTracks.contains(unique); ++n) {
                        unique = trackName + "_" + std::to_string(n);
                    }
                    spdlog::warn("GLB: two animations are named '{}'; the later one is kept as '{}'",
                        trackName, unique);
                    trackName = unique;
                }
                track->setName(trackName);
                track->setDuration(duration);

                if (!track->curves().empty()) {
                    outTracks.add(trackName, track);
                    spdlog::info("  Parsed animation '{}': {:.2f}s, {} curves",
                        trackName, duration, track->curves().size());
                }
            }
        }
    }
}
