// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Entity::clone copies an entity, its components and its Entity descendants, then
// remaps every reference the source subtree held into itself onto the copy (upstream
// `Entity#clone` + `resolveDuplicatedEntityReferenceProperties`). Until 2026-09-24
// only the render and light components had a cloneFrom — every other component came
// back default-constructed — references were not remapped at all, components were
// cloned in hash-map order, and a cloned primitive borrowed its mesh from the source,
// so destroying the source freed the clone's mesh (the sanitizer build aborts there).

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

#include "framework/anim/evaluator/animTrack.h"
#include "framework/anim/state-graph/animStateGraph.h"
#include "framework/appOptions.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentLayer.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/joint/jointComponent.h"
#include "framework/components/joint/jointComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/script/scriptRegistry.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/skin.h"
#include "scene/skinInstance.h"

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

    // Buffers that keep their bytes on the CPU: enough for a primitive mesh to exist.
    class CpuVertexBuffer final : public VertexBuffer
    {
    public:
        using VertexBuffer::VertexBuffer;
        void unlock() override {}
    };

    class CpuIndexBuffer final : public IndexBuffer
    {
    public:
        using IndexBuffer::IndexBuffer;
        bool setData(const std::vector<uint8_t>&) override { return true; }
    };

    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            const int numVertices, const VertexBufferOptions& options) override
        {
            return std::make_shared<CpuVertexBuffer>(this, format, numVertices, options);
        }
        std::shared_ptr<IndexBuffer> createIndexBuffer(const IndexFormat format, const int numIndices,
            const std::vector<uint8_t>& data) override
        {
            auto buffer = std::make_shared<CpuIndexBuffer>(this, format, numIndices);
            buffer->setData(data);
            return buffer;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    // A script with configuration it chooses to share with its clones, and one that
    // holds a reference into its own subtree.
    class Tuned final : public Script
    {
    public:
        SCRIPT_NAME("tuned")
        int value = 0;
        void cloneFrom(const Script& source) override { value = static_cast<const Tuned&>(source).value; }
    };

    class Holder final : public Script
    {
    public:
        SCRIPT_NAME("holder")
        Entity* target = nullptr;
        void cloneFrom(const Script& source) override { target = static_cast<const Holder&>(source).target; }
        void resolveClonedReferences(const Script& source, const CloneNodeMap& map) override
        {
            target = Component::remapCloned(static_cast<const Holder&>(source).target, map);
        }
    };

    Entity* addEntity(Engine& engine, GraphNode* parent, const std::string& name)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        entity->setName(name);
        parent->addChild(std::move(owned));
        return entity;
    }

    Entity* childNamed(const Entity* parent, const std::string& name)
    {
        for (const auto& child : parent->children()) {
            if (child->name() == name) {
                return dynamic_cast<Entity*>(child.get());
            }
        }
        return nullptr;
    }

    std::shared_ptr<AnimTrack> constantTranslation(const Vector3& v)
    {
        auto track = std::make_shared<AnimTrack>("const", 1.0f);
        AnimData times;
        times.components = 1;
        times.data = {0.0f, 1.0f};
        AnimData values;
        values.components = 3;
        values.data = {v.getX(), v.getY(), v.getZ(), v.getX(), v.getY(), v.getZ()};
        track->addInput(times);
        track->addOutput(values);
        AnimCurve curve;
        curve.nodeName = "Bone";
        curve.propertyPath = "localPosition";
        curve.inputIndex = 0;
        curve.outputIndex = 0;
        track->addCurve(curve);
        return track;
    }

    std::vector<std::string> componentTypes(const Entity& entity)
    {
        std::vector<std::string> names;
        for (const auto* component : entity.orderedComponents()) {
            names.emplace_back(typeid(*component).name());
        }
        return names;
    }
}

int main()
{
    std::cout << std::unitbuf;   // a crash must not swallow the checks already printed
    auto engine = std::make_shared<Engine>(nullptr);
    AppOptions options;
    options.graphicsDevice = std::make_shared<StubDevice>();
    options.registerComponentSystem<RenderComponentSystem>();
    options.registerComponentSystem<LightComponentSystem>();
    options.registerComponentSystem<CameraComponentSystem>();
    options.registerComponentSystem<CollisionComponentSystem>();
    options.registerComponentSystem<RigidBodyComponentSystem>();
    options.registerComponentSystem<JointComponentSystem>();
    options.registerComponentSystem<ButtonComponentSystem>();
    options.registerComponentSystem<ScreenComponentSystem>();
    options.registerComponentSystem<ElementComponentSystem>();
    options.registerComponentSystem<AnimationComponentSystem>();
    options.registerComponentSystem<AnimComponentSystem>();
    options.registerComponentSystem<ScriptComponentSystem>();
    engine->init(options);
    engine->scripts()->registerType<Tuned>();
    engine->scripts()->registerType<Holder>();

    // Source subtree:
    //   Rig        camera, light, collision, rigid body, script(tuned, holder), anim
    //     Bone     render (box, skinned on Bone + Outside)
    //     Image
    //     Hinge    joint (A = Bone, inside; B = Outside)
    //     Ui       screen, element, button (image = Image), animation (model = Bone)
    //   Outside    (not cloned)
    Entity* outside = addEntity(*engine, engine->root(), "Outside");
    Entity* rig = addEntity(*engine, engine->root(), "Rig");
    Entity* bone = addEntity(*engine, rig, "Bone");
    Entity* image = addEntity(*engine, rig, "Image");
    Entity* hinge = addEntity(*engine, rig, "Hinge");
    Entity* ui = addEntity(*engine, rig, "Ui");
    rig->tags().add("enemy");
    rig->setLocalPosition(1.0f, 2.0f, 3.0f);
    bone->setLocalPosition(1.0f, 2.0f, 3.0f);

    auto* camera = static_cast<CameraComponent*>(rig->addComponent<CameraComponent>());
    camera->camera()->setFov(30.0f);
    camera->camera()->setClearColor(Color(0.1f, 0.2f, 0.3f, 1.0f));
    camera->setPriority(5);
    camera->setLayers({1});
    DofSettings dof;
    dof.enabled = true;
    dof.focusDistance = 7.0f;
    camera->setDof(dof);
    auto* light = static_cast<LightComponent*>(rig->addComponent<LightComponent>());
    light->setIntensity(3.0f);
    light->setPenumbraSize(7.0f);
    auto* collision = static_cast<CollisionComponent*>(rig->addComponent<CollisionComponent>());
    collision->setType("sphere");
    collision->setRadius(2.0f);
    collision->setEnabled(false);
    auto* body = static_cast<RigidBodyComponent*>(rig->addComponent<RigidBodyComponent>());
    body->setType(RigidBodyType::Dynamic);
    body->setMass(5.0f);
    auto* scripts = static_cast<ScriptComponent*>(rig->addComponent<ScriptComponent>());
    static_cast<Tuned*>(scripts->create("tuned", {.enabled = false}))->value = 42;
    static_cast<Holder*>(scripts->create("holder"))->target = image;

    AnimStateGraph graph;
    auto& layer = graph.addLayer("Base", 1.0f);
    layer.states = {AnimStateDesc{"Pose"}};
    layer.transitions = {AnimTransitionDesc{.from = "START", .to = "Pose"}};
    graph.addFloatParameter("speed", 0.0f);
    auto* anim = static_cast<AnimComponent*>(rig->addComponent<AnimComponent>());
    anim->loadStateGraph(graph);
    anim->assignAnimation("Pose", constantTranslation(Vector3(10.0f, 0.0f, 0.0f)));
    anim->setFloat("speed", 2.5f);
    anim->baseLayer()->setWeight(0.5f);

    auto material = std::make_shared<StandardMaterial>();
    auto* render = static_cast<RenderComponent*>(bone->addComponent<RenderComponent>());
    render->setMaterial(material.get());
    render->setType("box");
    render->setCastShadows(false);
    auto skin = std::make_shared<Skin>(std::vector<Matrix4>{Matrix4::identity(), Matrix4::identity()},
        std::vector<std::string>{"Bone", "Outside"});
    auto skinInstance = std::make_shared<SkinInstance>(skin);
    skinInstance->setBones({bone, outside});
    skinInstance->setRootBone(rig);
    render->meshInstances()[0]->setSkinInstance(skinInstance);

    auto* joint = static_cast<JointComponent*>(hinge->addComponent<JointComponent>());
    joint->setType(PhysicsJointType::Hinge);
    joint->setLimits(-30.0f, 45.0f);
    joint->setEntityA(bone);
    joint->setEntityB(outside);

    auto* screen = static_cast<ScreenComponent*>(ui->addComponent<ScreenComponent>());
    screen->setReferenceResolution(Vector2(640.0f, 480.0f));
    auto* element = static_cast<ElementComponent*>(ui->addComponent<ElementComponent>());
    element->setText("hello");
    element->setWidth(321.0f);
    auto* button = static_cast<ButtonComponent*>(ui->addComponent<ButtonComponent>());
    button->setImageEntity(image);
    auto* animation = static_cast<AnimationComponent*>(ui->addComponent<AnimationComponent>());
    animation->setSpeed(1.5f);
    animation->setLoop(false);
    animation->setModel(bone);
    animation->addAnimation("idle", constantTranslation(Vector3(0.0f, 0.0f, 0.0f)));

    Entity* clone = rig->clone();
    engine->root()->addChild(clone);

    Entity* cBone = childNamed(clone, "Bone");
    Entity* cImage = childNamed(clone, "Image");
    Entity* cHinge = childNamed(clone, "Hinge");
    Entity* cUi = childNamed(clone, "Ui");
    if (!cBone || !cImage || !cHinge || !cUi) {
        std::cout << "  FAIL the clone has the source's children\n";
        return 1;
    }

    std::cout << "the node\n";
    check(clone->name() == "Rig" && clone->tags().has("enemy"), "name and tags are copied");
    check(componentTypes(*clone) == componentTypes(*rig), "components are cloned in creation order");

    std::cout << "\nevery component's settings\n";
    const auto* cCamera = clone->findComponent<CameraComponent>();
    check(cCamera && cCamera->camera()->fov() == 30.0f && cCamera->priority() == 5 &&
          cCamera->camera()->clearColor().g == 0.2f && cCamera->layers() == std::vector<int>{1},
        "camera: fov, clear colour, priority, layers");
    check(cCamera && cCamera->dof().enabled && cCamera->dof().focusDistance == 7.0f, "camera: depth of field");
    check(cCamera && cCamera->camera() != camera->camera() && cCamera->camera()->node() == clone,
        "camera: its own Camera on the clone's node");
    const auto* cLight = clone->findComponent<LightComponent>();
    check(cLight && cLight->intensity() == 3.0f && cLight->penumbraSize() == 7.0f, "light: intensity, penumbra size");
    const auto* cCollision = clone->findComponent<CollisionComponent>();
    check(cCollision && cCollision->type() == "sphere" && cCollision->radius() == 2.0f, "collision: type and radius");
    check(cCollision && !cCollision->enabled(), "a disabled component stays disabled");
    const auto* cBody = clone->findComponent<RigidBodyComponent>();
    check(cBody && cBody->type() == RigidBodyType::Dynamic && cBody->mass() == 5.0f, "rigid body: type and mass");
    const auto* cJoint = cHinge->findComponent<JointComponent>();
    check(cJoint && cJoint->type() == PhysicsJointType::Hinge && cJoint->hasLimits(), "joint: type and limits");
    const auto* cScreen = cUi->findComponent<ScreenComponent>();
    check(cScreen && cScreen->referenceResolution().x == 640.0f, "screen: reference resolution");
    const auto* cElement = cUi->findComponent<ElementComponent>();
    check(cElement && cElement->text() == "hello" && cElement->width() == 321.0f, "element: text and width");
    const auto* cAnimation = cUi->findComponent<AnimationComponent>();
    check(cAnimation && cAnimation->speed() == 1.5f && !cAnimation->loop() &&
          cAnimation->animations().contains("idle"), "animation: speed, loop, clips");

    auto* cScripts = clone->findComponent<ScriptComponent>();
    auto* cTuned = cScripts ? dynamic_cast<Tuned*>(cScripts->get("tuned")) : nullptr;
    check(cTuned && cTuned->value == 42, "script: created by name, and Script::cloneFrom copied its setting");
    check(cTuned && !cTuned->enabled(), "script: a disabled script stays disabled");

    std::cout << "\nrender\n";
    const auto* cRender = cBone->findComponent<RenderComponent>();
    check(cRender && cRender->meshInstances().size() == 1, "one mesh instance, as the source");
    MeshInstance* cMi = cRender && !cRender->meshInstances().empty() ? cRender->meshInstances()[0] : nullptr;
    MeshInstance* mi = render->meshInstances()[0];
    check(cMi && cMi->mesh() == mi->mesh() && cMi->material() == material.get() && !cMi->castShadow(),
        "shares the mesh and material, keeps the shadow flag");
    check(cMi && cMi->node() == cBone, "and is drawn at the clone's node");
    check(cMi && cMi->skinInstance() && cMi->skinInstance() != mi->skinInstance(), "its own skin instance");

    std::cout << "\nreferences into the cloned subtree go to the copy, others stay\n";
    const auto& cBones = cMi && cMi->skinInstance() ? cMi->skinInstance()->bones() : std::vector<GraphNode*>{};
    check(cBones.size() == 2 && cBones[0] == cBone && cBones[1] == outside, "skin bones");
    check(cMi && cMi->skinInstance() && cMi->skinInstance()->rootBone() == clone, "skin root bone");
    check(mi->skinInstance()->bones()[0] == bone, "the SOURCE skin keeps its own bones");
    check(cJoint && cJoint->entityA() == cBone && cJoint->entityB() == outside, "joint ends");
    check(joint->entityA() == bone, "the SOURCE joint keeps its own end");
    const auto* cButton = cUi->findComponent<ButtonComponent>();
    check(cButton && cButton->imageEntity() == cImage, "button image entity");
    auto* cHolder = cScripts ? dynamic_cast<Holder*>(cScripts->get("holder")) : nullptr;
    check(cHolder && cHolder->target == cImage, "a script's reference, through Script::resolveClonedReferences");

    std::cout << "\nanim\n";
    auto* cAnim = clone->findComponent<AnimComponent>();
    check(cAnim && cAnim->getFloat("speed") == 2.5f, "parameter values");
    check(cAnim && cAnim->baseLayer() && cAnim->baseLayer()->weight() == 0.5f, "layer weight");
    if (cAnim && cAnim->baseLayer()) {
        cAnim->baseLayer()->setWeight(1.0f);
        cAnim->update(0.1f);
    }
    const Vector3 moved = cBone->localPosition();
    check(std::fabs(moved.getX() - 10.0f) < 1e-4f,
        "the assigned animation drives the CLONE's bone (x = " + std::to_string(moved.getX()) + ")");
    check(bone->localPosition().getX() == 1.0f, "and not the source's");

    std::cout << "\nlifetime\n";
    Mesh* sharedMesh = mi->mesh();
    auto removed = engine->root()->removeChild(rig);
    removed.reset();   // the source and its primitive mesh owner are gone
    check(cMi && cMi->mesh() == sharedMesh && cMi->mesh()->aabb().halfExtents().getX() > 0.0f,
        "the clone's primitive mesh outlives the source (a use-after-free under the sanitizer before)");

    std::cout << (failures == 0 ? "\nAll entity clone tests passed\n" : "\nEntity clone tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
