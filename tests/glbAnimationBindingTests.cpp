// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// glTF animation binding: node IDENTITY has to survive from the parser to the
// instantiated entity and into the evaluator.
//
// Two holes this pins, both found by review on 2026-09-19:
//   - an UNNAMED animated node. The parser skipped its channels ("can't bind
//     unnamed nodes") and the container instantiated it with an empty name, so
//     it never moved. Upstream names such a node `node_<index>` in both places.
//   - two nodes with the SAME name in different branches (a left and a right
//     "Wheel"). Bound by bare name, both channels resolved to whichever
//     findByName met first, so one entity took both animations and the other
//     none. Upstream binds a PATH of names from the root down.
//
// The model is built in memory with tinygltf: no meshes, so the stub device
// creates nothing, and the only outputs are node payloads and one animation.

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "framework/anim/binder/defaultAnimBinder.h"
#include "framework/anim/evaluator/animClip.h"
#include "framework/anim/evaluator/animEvaluator.h"
#include "framework/anim/evaluator/animTrack.h"
#include "framework/entity.h"
#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "platform/graphics/graphicsDevice.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << what << '\n';
        }
    }

    bool near(const Vector3& a, const float x, const float y, const float z)
    {
        return std::fabs(a.getX() - x) < 1e-4f && std::fabs(a.getY() - y) < 1e-4f &&
            std::fabs(a.getZ() - z) < 1e-4f;
    }

    /// Creates nothing; the model has no meshes or images, so nothing asks it to.
    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&, int,
            const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    // Node indices of the test hierarchy:
    //   0 "Root"
    //   ├─ 1 ""  (unnamed)
    //   │   └─ 3 "Wheel"
    //   └─ 2 "Arm"
    //       └─ 4 "Wheel"
    // One animation, three translation channels with distinct constant values:
    //   node 1 -> (1, 0, 0), node 3 -> (0, 2, 0), node 4 -> (0, 0, 3).
    constexpr int kUnnamed = 1;
    constexpr int kArm = 2;
    constexpr int kWheelUnderUnnamed = 3;
    constexpr int kWheelUnderArm = 4;

    void appendFloats(std::vector<unsigned char>& bytes, const std::vector<float>& values)
    {
        const size_t at = bytes.size();
        bytes.resize(at + values.size() * sizeof(float));
        std::memcpy(bytes.data() + at, values.data(), values.size() * sizeof(float));
    }

    int addAccessor(tinygltf::Model& model, const int byteOffset, const int count, const int type)
    {
        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = static_cast<size_t>(byteOffset);
        view.byteLength = static_cast<size_t>(count) * sizeof(float) *
            static_cast<size_t>(type == TINYGLTF_TYPE_VEC3 ? 3 : 1);
        model.bufferViews.push_back(view);

        tinygltf::Accessor accessor;
        accessor.bufferView = static_cast<int>(model.bufferViews.size()) - 1;
        accessor.byteOffset = 0;
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        accessor.count = static_cast<size_t>(count);
        accessor.type = type;
        model.accessors.push_back(accessor);
        return static_cast<int>(model.accessors.size()) - 1;
    }

    tinygltf::Model buildModel()
    {
        tinygltf::Model model;
        model.asset.version = "2.0";

        model.nodes.resize(5);
        model.nodes[0].name = "Root";
        model.nodes[0].children = {kUnnamed, kArm};
        model.nodes[kUnnamed].children = {kWheelUnderUnnamed};
        model.nodes[kArm].name = "Arm";
        model.nodes[kArm].children = {kWheelUnderArm};
        model.nodes[kWheelUnderUnnamed].name = "Wheel";
        model.nodes[kWheelUnderArm].name = "Wheel";

        tinygltf::Scene scene;
        scene.nodes = {0};
        model.scenes.push_back(scene);
        model.defaultScene = 0;

        // One buffer: keyframe times, then three 2-key vec3 outputs.
        tinygltf::Buffer buffer;
        appendFloats(buffer.data, {0.0f, 1.0f});
        appendFloats(buffer.data, {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
        appendFloats(buffer.data, {0.0f, 2.0f, 0.0f, 0.0f, 2.0f, 0.0f});
        appendFloats(buffer.data, {0.0f, 0.0f, 3.0f, 0.0f, 0.0f, 3.0f});
        model.buffers.push_back(buffer);

        const int times = addAccessor(model, 0, 2, TINYGLTF_TYPE_SCALAR);
        const int out1 = addAccessor(model, 8, 2, TINYGLTF_TYPE_VEC3);
        const int out3 = addAccessor(model, 8 + 24, 2, TINYGLTF_TYPE_VEC3);
        const int out4 = addAccessor(model, 8 + 48, 2, TINYGLTF_TYPE_VEC3);

        tinygltf::Animation animation;
        animation.name = "drive";
        const int targets[3] = {kUnnamed, kWheelUnderUnnamed, kWheelUnderArm};
        const int outputs[3] = {out1, out3, out4};
        for (int i = 0; i < 3; ++i) {
            tinygltf::AnimationSampler sampler;
            sampler.input = times;
            sampler.output = outputs[i];
            sampler.interpolation = "LINEAR";
            animation.samplers.push_back(sampler);

            tinygltf::AnimationChannel channel;
            channel.sampler = i;
            channel.target_node = targets[i];
            channel.target_path = "translation";
            animation.channels.push_back(channel);
        }
        model.animations.push_back(animation);
        return model;
    }

    GraphNode* childNamed(GraphNode* parent, const std::string& name)
    {
        if (!parent) {
            return nullptr;
        }
        for (const auto& child : parent->children()) {
            if (child->name() == name) {
                return child.get();
            }
        }
        return nullptr;
    }

    struct Instance
    {
        Entity* root = nullptr;
        GraphNode* unnamed = nullptr;
        GraphNode* arm = nullptr;
        GraphNode* wheelUnderUnnamed = nullptr;
        GraphNode* wheelUnderArm = nullptr;
    };

    Instance instantiate(GlbContainerResource& container)
    {
        Instance instance;
        instance.root = container.instantiateRenderEntity();
        check(instance.root != nullptr, "instantiate returns a root");
        if (!instance.root) {
            return instance;
        }
        check(instance.root->name() == "Root", "single-root scene instantiates as its root node");
        instance.unnamed = childNamed(instance.root, "node_1");
        instance.arm = childNamed(instance.root, "Arm");
        instance.wheelUnderUnnamed = childNamed(instance.unnamed, "Wheel");
        instance.wheelUnderArm = childNamed(instance.arm, "Wheel");
        check(instance.unnamed != nullptr, "unnamed node instantiates as node_<index>");
        check(instance.arm != nullptr && instance.wheelUnderUnnamed != nullptr && instance.wheelUnderArm != nullptr,
            "the named nodes instantiate under their parents");
        return instance;
    }

    void evaluateAndCheck(Entity* bound, const Instance& instance, const std::shared_ptr<AnimTrack>& track,
        const std::string& label)
    {
        AnimEvaluator evaluator(std::make_unique<DefaultAnimBinder>(bound));
        evaluator.addClip(std::make_shared<AnimClip>(track, 0.0f, 1.0f, true, true));
        evaluator.update(0.25f);

        check(instance.unnamed && near(instance.unnamed->localPosition(), 1.0f, 0.0f, 0.0f),
            label + ": the unnamed node is animated");
        check(instance.wheelUnderUnnamed && near(instance.wheelUnderUnnamed->localPosition(), 0.0f, 2.0f, 0.0f),
            label + ": the Wheel under the unnamed node takes ITS channel");
        check(instance.wheelUnderArm && near(instance.wheelUnderArm->localPosition(), 0.0f, 0.0f, 3.0f),
            label + ": the Wheel under Arm takes ITS channel");
        check(instance.arm && near(instance.arm->localPosition(), 0.0f, 0.0f, 0.0f),
            label + ": a node with no channel is left alone");
    }
}

int main()
{
    auto device = std::make_shared<StubDevice>();

    // ── Parser: every channel survives, as a path ─────────────────────────
    tinygltf::Model model = buildModel();
    auto container = GlbParser::createFromModel(model, device, "glbAnimationBindingTests");
    check(container != nullptr, "createFromModel on a meshless model");
    if (!container) {
        return 1;
    }
    check(container->animTracks().size() == 1, "one animation track");
    const auto trackIt = container->animTracks().find("drive");
    check(trackIt != container->animTracks().end(), "track keeps the animation's name");
    if (trackIt == container->animTracks().end()) {
        return 1;
    }
    const std::shared_ptr<AnimTrack> track = trackIt->second;
    check(track->curves().size() == 3, "all three channels parsed, the unnamed target included");
    std::set<std::string> targets;
    for (const auto& curve : track->curves()) {
        targets.insert(curve.nodeName);
    }
    check(targets == std::set<std::string>{"Root/node_1", "Root/node_1/Wheel", "Root/Arm/Wheel"},
        "targets are name paths from the glTF root, unnamed nodes as node_<index>");

    // ── Bound to the model's own root (what instantiate returns) ─────────
    {
        const Instance instance = instantiate(*container);
        if (instance.root) {
            evaluateAndCheck(instance.root, instance, track, "bound to root");

            // The binder on its own: paths walk, bare names still search.
            DefaultAnimBinder binder(instance.root);
            check(binder.resolve("Root/Arm/Wheel") == instance.wheelUnderArm, "path resolves the Arm wheel");
            check(binder.resolve("Root/node_1/Wheel") == instance.wheelUnderUnnamed,
                "path resolves the wheel under the unnamed node");
            check(binder.resolve("Arm") == instance.arm, "a bare name still resolves by findByName");
            check(binder.resolve("Root/Arm/Nothing") == nullptr, "a path to a missing leaf resolves to nothing");
            check(binder.resolve("Elsewhere/Arm") == instance.arm,
                "a path whose root is unknown falls back to the leaf name");
            delete instance.root;
        }
    }

    // ── Bound to an app entity holding the model (the root is a child) ───
    {
        tinygltf::Model again = buildModel();
        auto container2 = GlbParser::createFromModel(again, device, "glbAnimationBindingTests");
        check(container2 != nullptr, "second container");
        if (container2) {
            const Instance instance = instantiate(*container2);
            if (instance.root) {
                auto* holder = new Entity();
                holder->setName("Holder");
                holder->addChild(instance.root);
                evaluateAndCheck(holder, instance, track, "bound to a holder above the root");
                delete holder;
            }
        }
    }

    if (failures == 0) {
        std::cout << "glb-animation-binding: all checks passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
