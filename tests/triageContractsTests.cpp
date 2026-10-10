// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.09.2026
//
// Small contracts, each invisible in a render:
//
//  - N6   StandardMaterial::setDiffuseMap(nullptr) (and the normal, metalness and
//         emissive siblings) clears a map a glTF material bound on the BASE slot. A
//         setter that stores only its own pointer clears nothing.
//  - N11.2 ScriptComponent::setExecutionOrder moves the component in its system's update
//         order, rather than storing the value and changing nothing.
//  - N10.4 ResourceLoader::shutdown hands every undelivered completion back as an error,
//         and a load after shutdown fails at once; otherwise the caller waits for good
//         (an Asset stays `_loading`).
//  - N11.1 a button's image entity reads null once that entity is destroyed, rather
//         than a dangling raw pointer.
//  - N10.2 two glTF animations with the same name are both kept; the later one must not
//         overwrite the earlier in silence.
//  - A gsplat component given a new resource, or none, removes the splat instance it
//    attached; otherwise the old cloud goes on drawing beside the new one.
//  - A volume's mip chain halves its depth too, so a deep volume has more levels than
//    its width and height alone allow.
//  - GraphicsDevice::resizeCanvas keeps the current resolution at a zero size (a
//    minimised window), rather than asking the backend for an empty drawable.
//  - A StandardMaterial's gloss map tiling reaches the material block (the gloss map
//    was sampled untransformed, so a tiled ground repeated its other maps alone).

#include <tiny_gltf.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/handlers/resourceLoader.h"
#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "framework/script/scriptRegistry.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "platform/graphics/texture.h"
#include "scene/gsplat/gsplatData.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
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
    const auto device = std::make_shared<StubGraphicsDevice>();

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
        auto engine = makeTestEngine<ScriptComponentSystem, ButtonComponentSystem>(device);
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
        // Two "Walk"s, then names whose hash order is not their file order.
        const std::vector<std::string> names = {"Walk", "Walk", "Zeta", "Alpha", "Mid", "Beta"};
        for (const auto& animationName : names) {
            tinygltf::AnimationSampler sampler;
            sampler.input = 0;
            sampler.output = 1;
            sampler.interpolation = "LINEAR";
            tinygltf::AnimationChannel channel;
            channel.sampler = 0;
            channel.target_node = 0;
            channel.target_path = "translation";
            tinygltf::Animation animation;
            animation.name = animationName;
            animation.samplers = {sampler};
            animation.channels = {channel};
            model.animations.push_back(animation);
        }
        auto container = GlbParser::createFromModel(model, device, "triageContractsTests");
        static const AnimTrackList none;
        const auto& tracks = container ? container->animTracks() : none;
        check(tracks.size() == 6 && tracks.contains("Walk") && tracks.contains("Walk_1"),
            "both are kept, the later one as 'Walk_1'");
        std::string order;
        for (const auto& [name, track] : tracks) {
            order += (order.empty() ? "" : ",") + name;
        }
        check(order == "Walk,Walk_1,Zeta,Alpha,Mid,Beta",
            "the tracks keep the file's order (upstream container.animations): " + order);
    }

    std::cout << "VRAM: storage buffers are counted as sb, not vb\n";
    {
        const auto format = std::make_shared<VertexFormat>(64, true, false);
        {
            auto vertices = std::make_shared<CpuVertexBuffer>(device.get(), format, 10, VertexBufferOptions{});
            auto storage = std::make_shared<CpuVertexBuffer>(device.get(), format, 100, VertexBufferOptions{});
            check(device->vram().vb == 640 + 6400 && device->vram().sb == 0, "both start as vertex buffers");
            storage->markStorageUse();
            storage->markStorageUse();
            check(device->vram().vb == 640 && device->vram().sb == 6400,
                "marking one as storage moves its bytes from vb to sb, once");
            Compute compute(device.get(), nullptr, "vram");
            compute.setParameter("buffer", std::static_pointer_cast<VertexBuffer>(vertices));
            check(device->vram().vb == 0 && device->vram().sb == 7040,
                "a Compute buffer parameter is storage");
        }
        check(device->vram().vb == 0 && device->vram().sb == 0, "freeing them returns both buckets to zero");
    }

    std::cout << "gsplat resource replacement removes the previous splat instance\n";
    {
        const auto splatDevice = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true});
        const auto makeResource = [&splatDevice](const float x) {
            GSplatData::ActivatedSplats splats;
            splats.count = 1;
            splats.positions = {x, 0.0f, 0.0f};
            splats.rotations = {0.0f, 0.0f, 0.0f, 1.0f};
            splats.scales = {0.1f, 0.1f, 0.1f};
            splats.opacities = {1.0f};
            splats.sh0 = {0.5f, 0.5f, 0.5f};
            return std::make_shared<GSplatResource>(GSplatData::fromActivated(splats, "test"), splatDevice);
        };
        Entity entity;
        auto* gsplat = static_cast<GSplatComponent*>(entity.addComponentInstance(
            std::make_unique<GSplatComponent>(nullptr, &entity), componentTypeID<GSplatComponent>()));
        const auto instanceCount = [&entity] {
            const auto* render = entity.findComponent<RenderComponent>();
            return render ? render->meshInstances().size() : size_t{0};
        };
        gsplat->setResource(makeResource(0.0f));
        check(instanceCount() == 1, "the first resource attaches one splat instance");
        gsplat->setResource(makeResource(1.0f));
        check(instanceCount() == 1, "a second resource replaces it rather than adding another");
        gsplat->setResource(nullptr);
        check(instanceCount() == 0, "clearing the resource removes it");
    }

    std::cout << "a volume's mip chain counts its depth\n";
    {
        TextureOptions options;
        options.width = 4;
        options.height = 4;
        options.depth = 32;
        options.volume = true;
        options.mipmaps = true;
        Texture volume(device.get(), options);
        check(volume.getNumLevels() == 6, "a 4x4x32 volume has 6 levels (32 down to 1), not 3");
        options.depth = 1;
        options.volume = false;
        Texture flat(device.get(), options);
        check(flat.getNumLevels() == 3, "a 4x4 2D texture still has 3");
    }

    std::cout << "resizeCanvas keeps the resolution at a zero size\n";
    {
        const auto canvasDevice = std::make_shared<StubGraphicsDevice>(
            StubGraphicsDevice::Options{.size = {640, 480}, .resizable = true});
        canvasDevice->resizeCanvas(0, 0);
        check(canvasDevice->size() == std::pair{640, 480}, "a 0x0 canvas keeps 640x480");
        canvasDevice->resizeCanvas(320, 0);
        check(canvasDevice->size() == std::pair{640, 480}, "a zero height alone keeps it too");
        canvasDevice->resizeCanvas(320, 240);
        check(canvasDevice->size() == std::pair{320, 240}, "a real size still applies");
    }

    std::cout << "gloss map tiling reaches the material block\n";
    {
        StandardMaterial material;
        check(material.packedUniforms().glossTransform0[0] == 1.0f &&
              material.packedUniforms().glossTransform1[1] == 1.0f, "an untiled gloss map packs the identity");
        material.setGlossMapTiling(Vector2(17.0f, 7.0f));
        const auto& packed = material.packedUniforms();
        check(packed.glossTransform0[0] == 17.0f && packed.glossTransform1[1] == 7.0f,
            "setGlossMapTiling(17, 7) packs the scale into the gloss transform");
        check(packed.metalRoughTransform0[0] == 1.0f, "and leaves the metalness map's transform alone");
    }

    return finish("triage contracts");
}
