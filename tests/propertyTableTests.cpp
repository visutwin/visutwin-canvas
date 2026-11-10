// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// The property table and entity ids. The sweep reads and writes back every property of every
// built-in type on a real entity, so a property whose getter and setter disagree, an enum
// without names, a value of the wrong type or a text form that does not read back fails here
// whichever type it was added to. The ids must be unique and never reused, and an id kept after
// its entity is destroyed must find nothing, in any engine.

#include <cmath>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <variant>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/properties/propertyTable.h"
#include "scene/materials/standardMaterial.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    bool closeTo(const float a, const float b) { return std::abs(a - b) <= 1e-4f * std::max(1.0f, std::abs(a)); }

    // Equal, floats to rounding (a rotation goes through a quaternion and back).
    bool valuesEqual(const PropertyValue& a, const PropertyValue& b)
    {
        if (a.index() != b.index()) {
            return false;
        }
        if (const auto* x = std::get_if<float>(&a)) {
            return closeTo(*x, std::get<float>(b));
        }
        if (const auto* x = std::get_if<Vector2>(&a)) {
            const auto& y = std::get<Vector2>(b);
            return closeTo(x->x, y.x) && closeTo(x->y, y.y);
        }
        if (const auto* x = std::get_if<Vector3>(&a)) {
            const auto& y = std::get<Vector3>(b);
            return closeTo(x->getX(), y.getX()) && closeTo(x->getY(), y.getY()) && closeTo(x->getZ(), y.getZ());
        }
        if (const auto* x = std::get_if<Vector4>(&a)) {
            const auto& y = std::get<Vector4>(b);
            return closeTo(x->getX(), y.getX()) && closeTo(x->getY(), y.getY()) && closeTo(x->getZ(), y.getZ()) &&
                closeTo(x->getW(), y.getW());
        }
        if (const auto* x = std::get_if<Quaternion>(&a)) {
            const auto& y = std::get<Quaternion>(b);
            return closeTo(x->getX(), y.getX()) && closeTo(x->getY(), y.getY()) && closeTo(x->getZ(), y.getZ()) &&
                closeTo(x->getW(), y.getW());
        }
        if (const auto* x = std::get_if<Color>(&a)) {
            const auto& y = std::get<Color>(b);
            return closeTo(x->r, y.r) && closeTo(x->g, y.g) && closeTo(x->b, y.b) && closeTo(x->a, y.a);
        }
        if (const auto* x = std::get_if<bool>(&a)) {
            return *x == std::get<bool>(b);
        }
        if (const auto* x = std::get_if<int>(&a)) {
            return *x == std::get<int>(b);
        }
        if (const auto* x = std::get_if<std::string>(&a)) {
            return *x == std::get<std::string>(b);
        }
        if (const auto* x = std::get_if<EntityId>(&a)) {
            return *x == std::get<EntityId>(b);
        }
        return std::holds_alternative<std::monostate>(a);
    }

    // Every property of one object: typed, named once, enums named, set(get()) a no-op, and the
    // text form reading back.
    void sweep(const TypeDesc& type, void* target)
    {
        std::set<std::string> names;
        for (const PropertyDesc& property : type.properties) {
            const std::string name = type.name + "." + property.name;
            if (!names.insert(property.name).second) {
                fail(name + " is registered twice");
            }
            const PropertyValue value = property.get(target);
            if (!valueHolds(property.type, value)) {
                fail(name + " reads a value that is not a " + valueTypeName(property.type));
                continue;
            }
            if (property.type == ValueType::Enum) {
                if (property.options.empty()) {
                    fail(name + " is an enum with no options");
                } else if (!property.option(std::get<int>(value))) {
                    fail(name + " reads " + std::to_string(std::get<int>(value)) + ", none of its options");
                }
            }
            if (!property.readOnly()) {
                const PropertyStatus status = PropertyRegistry::set(property, target, value);
                if (status != PropertyStatus::Ok) {
                    fail(name + " refused its own value: " + propertyStatusName(status));
                } else if (!valuesEqual(property.get(target), value)) {
                    fail(name + " changed when given its own value (" + formatPropertyValue(property, value) +
                        " -> " + formatPropertyValue(property, property.get(target)) + ")");
                }
            } else if (PropertyRegistry::set(property, target, value) != PropertyStatus::ReadOnly) {
                fail(name + " is read-only but a set was not refused as such");
            }
            PropertyValue parsed;
            const std::string text = formatPropertyValue(property, value);
            if (!parsePropertyValue(property, text, parsed) || !valuesEqual(parsed, value)) {
                fail(name + " writes \"" + text + "\", which does not read back");
            }
        }
        check(true, type.name + ": " + std::to_string(type.properties.size()) + " properties swept");
    }
}

int main()
{
    std::cout << std::unitbuf;
    auto engine = makeTestEngine<RenderComponentSystem, LightComponentSystem, CameraComponentSystem,
        ScreenComponentSystem, ElementComponentSystem, ScriptComponentSystem>(std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true}));
    PropertyRegistry& registry = PropertyRegistry::instance();

    // --- Entity ids ---------------------------------------------------------------------
    Entity* car = addTo(engine->root(), newEntity(engine.get(), "Car"));
    Entity* wheel = addTo(car, newEntity(engine.get(), "Wheel"));
    Entity* wheel2 = addTo(car, newEntity(engine.get(), "Wheel"));
    check(car->id() && wheel->id() && car->id() != wheel->id() && wheel->id() != wheel2->id(),
        "every entity has its own nonzero id");
    check(wheel->id().value > car->id().value, "ids increase in creation order");
    check(engine->entityById(wheel->id()) == wheel, "an entity is found by its id");
    check(!engine->entityById(EntityId{}), "id 0 names nothing");

    const EntityId wheel2Id = wheel2->id();
    wheel2->destroy();
    check(!engine->entityById(wheel2Id), "a destroyed entity is not found while its node still exists");
    { auto freed = car->removeChild(wheel2); }
    Entity* later = addTo(car, newEntity(engine.get(), "Later"));
    check(later->id().value > wheel2Id.value && !engine->entityById(wheel2Id),
        "a freed entity's id is not reused and still finds nothing");

    std::unique_ptr<Entity> copy(car->clone());
    check(copy->id() != car->id(), "a clone has an id of its own");

    auto other = makeTestEngine<RenderComponentSystem>(std::make_shared<StubGraphicsDevice>());
    Entity* foreign = addTo(other->root(), newEntity(other.get(), "Car"));
    check(!engine->entityById(foreign->id()) && other->entityById(foreign->id()) == foreign,
        "an engine finds only the entities of its own hierarchy");
    check(Entity::findById(foreign->id()) == foreign, "Entity::findById keeps to no engine");

    // --- Paths --------------------------------------------------------------------------
    check(wheel->path() == "Car/Wheel", "path names the entity from below the root: " + wheel->path());
    check(engine->entityByPath("Car/Wheel") == wheel, "a path finds its entity");
    check(engine->entityByPath("") == engine->root(), "the empty path is the root");
    check(!engine->entityByPath("Car/Nothing") && !engine->entityByPath("Nothing/Wheel"),
        "a path naming no entity finds nothing");
    Entity* wheelTwin = addTo(car, newEntity(engine.get(), "Wheel"));
    check(engine->entityByPath("Car/Wheel") == wheel && wheelTwin->path() == "Car/Wheel",
        "of siblings sharing a name the path finds the first");

    // --- The sweep, over an entity carrying every built-in type --------------------------
    Entity* rig = addTo(engine->root(), newEntity(engine.get(), "Rig"));
    rig->setLocalPosition(1.0f, 2.0f, 3.0f);
    rig->setLocalEulerAngles(10.0f, 20.0f, 30.0f);
    auto* light = static_cast<LightComponent*>(rig->addComponent<LightComponent>());
    rig->addComponent<CameraComponent>();
    auto* render = static_cast<RenderComponent*>(rig->addComponent<RenderComponent>());
    render->setType("box");
    auto material = std::make_shared<StandardMaterial>();
    material->setGloss(0.3f);
    render->setMaterial(material.get());
    rig->addComponent<ScreenComponent>();
    // Camera controls on the rig, which has a camera (an entity without one is refused).
    check(registry.type("cameraControls")->addTo(*rig), "the rig takes camera controls");
    check(!registry.type("cameraControls")->addTo(*car), "an entity without a camera does not");
    addElement(rig);

    for (const auto& type : registry.types()) {
        void* target = type->resolve ? type->resolve(*rig) : nullptr;
        if (type->name == "standard") {
            target = material.get();
        } else if (type->name == "scene") {
            target = engine->scene().get();
        }
        if (check(target != nullptr, type->name + " has an object on the test entity")) {
            sweep(*type, target);
        }
    }
    for (const char* name : {"node", "light", "camera", "render", "element", "screen", "cameraControls", "scene", "standard"}) {
        check(registry.type(name) != nullptr, std::string("the built-in type ") + name + " is registered");
    }

    // --- Components by type name --------------------------------------------------------
    const TypeDesc* lightType = registry.type("light");
    check(lightType->isComponent() && !registry.type("node")->isComponent() && !registry.type("standard")->isComponent(),
        "component types are marked; the node and the material are not");
    check(lightType->removeFrom(*rig) && !rig->findComponent<LightComponent>() && !lightType->removeFrom(*rig),
        "a component is removed by its type, once");
    check(lightType->addTo(*rig) && rig->findComponent<LightComponent>() != nullptr, "and added back");
    light = rig->findComponent<LightComponent>();

    // --- Reading and writing through the registry ----------------------------------------
    check(registry.set(*rig, "light.intensity", PropertyValue(2.5f)) == PropertyStatus::Ok &&
        light->intensity() == 2.5f, "a set reaches the component");
    PropertyValue read;
    check(registry.get(*rig, "light.intensity", read) == PropertyStatus::Ok && std::get<float>(read) == 2.5f,
        "a get reads it back");
    check(registry.set(*rig, "light.intensity", PropertyValue(3)) == PropertyStatus::TypeMismatch &&
        light->intensity() == 2.5f, "a value of the wrong type is refused and changes nothing");
    check(registry.set(*rig, "light.type", PropertyValue(99)) == PropertyStatus::InvalidValue,
        "an enum value that is none of the options is refused");
    check(registry.set(*rig, "node.id", PropertyValue(EntityId{5})) == PropertyStatus::ReadOnly,
        "a read-only property is refused");
    check(registry.get(*wheel, "light.intensity", read) == PropertyStatus::NoTarget,
        "an entity without the component has no target");
    check(registry.get(*rig, "torch.intensity", read) == PropertyStatus::NoType &&
        registry.get(*rig, "light.brightness", read) == PropertyStatus::NoProperty &&
        registry.get(*rig, "intensity", read) == PropertyStatus::NoType, "unknown names are told apart");
    check(registry.get(*wheel, "node.parent", read) == PropertyStatus::Ok && std::get<EntityId>(read) == car->id(),
        "node.parent names the parent by id");

    // Rotation as Euler angles in degrees, including the pitch of 90 where X and Z are one axis.
    check(registry.set(*rig, "node.rotation", PropertyValue(Vector3(-40.0f, 25.0f, 70.0f))) == PropertyStatus::Ok &&
        registry.get(*rig, "node.rotation", read) == PropertyStatus::Ok &&
        valuesEqual(read, PropertyValue(Vector3(-40.0f, 25.0f, 70.0f))), "node.rotation reads back its angles");
    registry.set(*rig, "node.rotation", PropertyValue(Vector3(30.0f, 90.0f, 0.0f)));
    registry.get(*rig, "node.rotation", read);
    const Quaternion gimbal = Quaternion::fromEulerAngles(std::get<Vector3>(read).getX(),
        std::get<Vector3>(read).getY(), std::get<Vector3>(read).getZ());
    const Quaternion expected = Quaternion::fromEulerAngles(30.0f, 90.0f, 0.0f);
    check(std::abs(std::abs(gimbal.getX() * expected.getX() + gimbal.getY() * expected.getY() +
        gimbal.getZ() * expected.getZ() + gimbal.getW() * expected.getW()) - 1.0f) < 1e-4f,
        "at a pitch of 90 the angles read describe the same rotation");

    // --- Text: assignments and the dump --------------------------------------------------
    check(applyPropertyAssignment(*engine, "Rig:light.color=1, 0.5, 0.25") == PropertyStatus::Ok &&
        light->color().g == 0.5f && light->color().a == 1.0f, "an assignment sets a colour from rgb");
    check(applyPropertyAssignment(*engine, "Rig:light.type=spot") == PropertyStatus::Ok &&
        light->type() == LightType::LIGHTTYPE_SPOT, "an assignment sets an enum by name");
    check(applyPropertyAssignment(*engine, "Car/Wheel:node.name=Front") == PropertyStatus::Ok &&
        wheel->name() == "Front", "an assignment reaches an entity by path");
    check(applyPropertyAssignment(*engine, "Nowhere:node.name=x") == PropertyStatus::NoTarget &&
        applyPropertyAssignment(*engine, "Rig:light.intensity=bright") == PropertyStatus::TypeMismatch &&
        applyPropertyAssignment(*engine, "Rig light.intensity") == PropertyStatus::TypeMismatch,
        "a bad path, bad text and a malformed assignment are refused");

    const std::string dump = describeEntityProperties(*rig);
    check(dump.find("light.type = spot\n") != std::string::npos && dump.find("standard.gloss = 0.3\n") !=
        std::string::npos && dump.find("node.id = ") != std::string::npos &&
        dump.find("(read-only)") != std::string::npos, "the dump lists the entity's types and its material");

    return finish("property-table");
}
