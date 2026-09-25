// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Small contracts fixed by the 2026-09-25 triage of the 2026-09-23 audit findings, each
// invisible in a render:
//
//  - N6   StandardMaterial::setDiffuseMap(nullptr) (and the normal, metalness and
//         emissive siblings) clears a map a glTF material bound on the BASE slot. The
//         setter used to store its own pointer only, so it cleared nothing.
//  - N11.2 ScriptComponent::setExecutionOrder moves the component in its system's update
//         order. It used to store the value and change nothing.
//  - N10.4 ResourceLoader::shutdown hands every undelivered completion back as an error,
//         and a load after shutdown fails at once; both used to leave the caller waiting
//         for good (an Asset stayed `_loading`).
//  - N11.1 a button's image entity reads null once that entity is destroyed; the raw
//         pointer used to dangle.
//  - N10.2 two glTF animations with the same name are both kept; the later one used to
//         overwrite the earlier in silence.

#include <tiny_gltf.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/appOptions.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/handlers/resourceLoader.h"
#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "framework/script/scriptRegistry.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

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

    bool hasSlot(const StandardMaterial& material, const int slot)
    {
        std::vector<TextureSlot> slots;
        material.getTextureSlots(slots);
        for (const auto& [index, texture] : slots) {
            if (index == slot && texture) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> ran;

    class First final : public Script
    {
    public:
        SCRIPT_NAME("first")
        void update(float) override { ran.push_back("first"); }
    };

    class Second final : public Script
    {
    public:
        SCRIPT_NAME("second")
        void update(float) override { ran.push_back("second"); }
    };

    class ByteHandler final : public ResourceHandler
    {
    public:
        std::unique_ptr<LoadedData> load(const std::string&) override { return std::make_unique<LoadedData>(); }
    };

    Entity* addEntity(Engine& engine)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        engine.root()->addChild(std::move(owned));
        return entity;
    }
}

int main()
{
    std::cout << std::unitbuf;
    const auto device = std::make_shared<StubDevice>();

    std::cout << "N6: clearing a map a glTF material bound on the base slot\n";
    {
        Texture diffuse(device.get());
        Texture normal(device.get());
        StandardMaterial material;
        // What the glTF parser does: the BASE Material's slots.
        material.setBaseColorTexture(&diffuse);
        material.setHasBaseColorTexture(true);
        material.setNormalTexture(&normal);
        material.setHasNormalTexture(true);
        check(material.diffuseMap() == &diffuse && hasSlot(material, 0),
            "the diffuse map reads back the base texture, and slot 0 is bound");
        material.setDiffuseMap(nullptr);
        material.setNormalMap(nullptr);
        check(!hasSlot(material, 0) && !hasSlot(material, 1), "setDiffuseMap / setNormalMap(nullptr) unbind the slots");
        check((material.packedUniforms().flags & 0x5u) == 0u, "and clear the flag bits the shader reads");
    }

    std::cout << "\nN11.2: execution order\n";
    {
        auto engine = std::make_shared<Engine>(nullptr);
        AppOptions options;
        options.graphicsDevice = device;
        options.registerComponentSystem<ScriptComponentSystem>();
        options.registerComponentSystem<ButtonComponentSystem>();
        engine->init(options);
        engine->scripts()->registerType<First>();
        engine->scripts()->registerType<Second>();
        auto* system = dynamic_cast<ScriptComponentSystem*>(engine->systems()->getById("script"));

        auto* a = static_cast<ScriptComponent*>(addEntity(*engine)->addComponent<ScriptComponent>());
        auto* b = static_cast<ScriptComponent*>(addEntity(*engine)->addComponent<ScriptComponent>());
        a->create("first");
        b->create("second");
        ran.clear();
        system->update(0.016f);
        check(ran == std::vector<std::string>{"first", "second"}, "components run in creation order");
        a->setExecutionOrder(100);
        ran.clear();
        system->update(0.016f);
        check(ran == std::vector<std::string>{"second", "first"}, "a later execution order moves a component back");
        auto* c = static_cast<ScriptComponent*>(addEntity(*engine)->addComponent<ScriptComponent>());
        c->create("second");
        ran.clear();
        system->update(0.016f);
        check(ran.size() == 3 && ran.back() == "first",
            "a component added afterwards goes before the one moved past the counter");

        std::cout << "\nN11.1: a destroyed button image\n";
        Entity* button = addEntity(*engine);
        Entity* image = addEntity(*engine);
        auto* component = static_cast<ButtonComponent*>(button->addComponent<ButtonComponent>());
        component->setImageEntity(image);
        check(component->imageEntity() == image, "the image entity is set");
        image->destroy();
        check(component->imageEntity() == nullptr, "and reads null once it is destroyed");
    }

    std::cout << "\nN10.4: loader shutdown\n";
    {
        ResourceLoader loader(nullptr);   // the engine is only kept for handlers that ask
        loader.addHandler("bytes", std::make_unique<ByteHandler>());
        int successes = 0;
        std::vector<std::string> errors;
        loader.load("a", "bytes", [&](std::unique_ptr<LoadedData>) { ++successes; },
            [&](const std::string& e) { errors.push_back(e); });
        loader.shutdown();   // the worker finishes the request; nobody processes it
        check(successes == 0 && errors.size() == 1, "an undelivered completion comes back as an error at shutdown");
        check(!loader.hasPending(), "and nothing is left pending");
        loader.load("b", "bytes", [&](std::unique_ptr<LoadedData>) { ++successes; },
            [&](const std::string& e) { errors.push_back(e); });
        check(successes == 0 && errors.size() == 2, "a load after shutdown fails at once");
    }

    std::cout << "\nN10.2: two glTF animations with one name\n";
    {
        tinygltf::Model model;
        model.asset.version = "2.0";
        tinygltf::Buffer buffer;
        const float times[] = {0.0f, 1.0f};
        const float values[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        buffer.data.resize(sizeof(times) + sizeof(values));
        std::memcpy(buffer.data.data(), times, sizeof(times));
        std::memcpy(buffer.data.data() + sizeof(times), values, sizeof(values));
        model.buffers.push_back(buffer);
        tinygltf::BufferView timeView;
        timeView.buffer = 0;
        timeView.byteLength = sizeof(times);
        tinygltf::BufferView valueView;
        valueView.buffer = 0;
        valueView.byteOffset = sizeof(times);
        valueView.byteLength = sizeof(values);
        model.bufferViews = {timeView, valueView};
        tinygltf::Accessor timeAccessor;
        timeAccessor.bufferView = 0;
        timeAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        timeAccessor.count = 2;
        timeAccessor.type = TINYGLTF_TYPE_SCALAR;
        timeAccessor.minValues = {0.0};
        timeAccessor.maxValues = {1.0};
        tinygltf::Accessor valueAccessor;
        valueAccessor.bufferView = 1;
        valueAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        valueAccessor.count = 2;
        valueAccessor.type = TINYGLTF_TYPE_VEC3;
        model.accessors = {timeAccessor, valueAccessor};
        tinygltf::Node node;
        node.name = "Mover";
        model.nodes = {node};
        tinygltf::Scene scene;
        scene.nodes = {0};
        model.scenes = {scene};
        model.defaultScene = 0;
        for (int i = 0; i < 2; ++i) {
            tinygltf::AnimationSampler sampler;
            sampler.input = 0;
            sampler.output = 1;
            sampler.interpolation = "LINEAR";
            tinygltf::AnimationChannel channel;
            channel.sampler = 0;
            channel.target_node = 0;
            channel.target_path = "translation";
            tinygltf::Animation animation;
            animation.name = "Walk";
            animation.samplers = {sampler};
            animation.channels = {channel};
            model.animations.push_back(animation);
        }
        auto container = GlbParser::createFromModel(model, device, "triageContractsTests");
        const auto& tracks = container ? container->animTracks() : std::unordered_map<std::string, std::shared_ptr<AnimTrack>>{};
        check(tracks.size() == 2 && tracks.contains("Walk") && tracks.contains("Walk_1"),
            "both are kept, the later one as 'Walk_1'");
    }

    std::cout << (failures == 0 ? "\nAll triage contract tests passed\n" : "\nTriage contract tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
