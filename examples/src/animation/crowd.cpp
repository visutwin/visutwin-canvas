// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.11.2026
//
// Port of upstream's animation/crowd example.
//
// A crowd of independently animated bitmoji characters, a scene for profiling the CPU cost of
// skinned animation (run it with VISUTWIN_CPU_STATS=first,last for the phase timings). Each
// character wanders a round area of radius 40: it idles for a few seconds, picks a random point
// and walks there (or, three times in ten, jogs), turning smoothly toward it, and jumps now and
// then on the way. One anim state graph is shared by all: an integer "speed" (0 still, 1 walk,
// 2 jog) moves a character between Idle, Walk and Jog, and a "jump" trigger sends any state to
// Jump, which hands back to Idle at 80% of its clip; the ground speed follows the state, blended
// through a transition. Both random sequences are seeded (mulberry32), so every run spawns the
// same crowd and makes the same choices.
//
// The ground is a 2000 x 2000 grass plane (tiles of 6 units, tinted 0.62 / 0.95 / 0.42, gloss
// 0.15, metalness workflow at 0) that casts no shadow, ringed by 70 shadow-casting low-poly trees
// outside the characters' area. A warm, low sun (intensity 2.2, colour 1 / 0.9 / 0.75, pitch 40,
// yaw 35) casts PCF5 shadows over two cascades (distribution 0.6) out to 45 units at 2048; the
// morning environment atlas lights the scene and draws the sky (mip 1, intensity 1.2), exposure
// 1.1, and a linear fog from 50 to 400 fades the far ground into the sky. The camera (fov 50, far
// 1500, ACES) orbits and flies from (-5, 4.5, 15) around (0, 1, 0), pitch -90 to -2, zoom 1 to
// 150. Twenty characters to begin with, casting shadows, with a fixed bounding box.
//
// Keys stand in for upstream's control panel:
//   [ / ]  characters -/+ 10 (1 to 500)
//   - / =  characters -/+ 100
//   B      the fixed character bounding box (Custom AABB) on / off
//   H      character shadows on / off
// The camera: WASDQE move (Shift fast, Ctrl slow), left / right mouse orbit / fly, Shift or the
// middle button pan, the wheel zooms, R resets the view.
//
// DEVIATIONS:
// - the grass maps get the device's full anisotropy, as every texture here, not 8.
// - the back buffer renders at the examples' one pixel per point, not min(device ratio, 2).
// - the camera starts at its pose instead of gliding there from the origin.
// - the characters are steered before the engine's update, so they read the state their
//   animation reached in the previous frame.
//
// @credit Sparse Grass by Amal Kumar, Poly Haven, CC0 1.0 (assets/textures/terrain/sparse_grass.credits.txt)
// @credit Low-poly Tree with Twisting Branches, Sketchfab, CC BY 4.0 (assets/models/low-poly-tree.txt)
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "animClips.h"
#include "exampleApp.h"
#include "core/shape/boundingBox.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // The radius of the round area the characters wander around in.
    constexpr double kAreaRadius = 40.0;

    // The most characters the count allows.
    constexpr int kMaxCharacters = 500;

    // The grassy ground, reaching far beyond the area the characters wander in.
    constexpr float kGroundSize = 2000.0f;
    constexpr float kGrassTileSize = 6.0f;

    constexpr int kTreeCount = 70;

    // A small seeded random number generator (mulberry32), so every run spawns the same crowd
    // and makes the same choices, which keeps profiling runs comparable. Numbers in [0, 1).
    class Mulberry32
    {
    public:
        explicit Mulberry32(const uint32_t seed): _seed(seed) {}

        double operator()()
        {
            _seed += 0x6d2b79f5u;
            uint32_t t = (_seed ^ (_seed >> 15)) * (1u | _seed);
            t ^= t + (t ^ (t >> 7)) * (61u | t);
            return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
        }

    private:
        uint32_t _seed;
    };

    // The units per second a character moves at in an animation state.
    double speedForState(const std::string& state)
    {
        if (state == "Walk") {
            return 1.0;
        }
        if (state == "Jog") {
            return 4.0;
        }
        return 0.0;
    }

    struct Character
    {
        // The character entity, which the anim component is on.
        Entity* entity = nullptr;
        AnimComponent* anim = nullptr;
        // The position on the ground, and the one the character walks to.
        double x = 0.0;
        double z = 0.0;
        double targetX = 0.0;
        double targetZ = 0.0;
        // The facing angle in degrees.
        double heading = 0.0;
        // Seconds left to stand still before picking a new target.
        double idleTime = 0.0;
        // Seconds until the next jump while moving.
        double jumpTime = 0.0;
    };
}

class CrowdExample final: public ExampleApp
{
public:
    CrowdExample(): ExampleApp({}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        _modelAsset = std::make_unique<Asset>("model", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _idleAsset = std::make_unique<Asset>("idleAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/idle.glb"));
        _walkAsset = std::make_unique<Asset>("walkAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/walk.glb"));
        _jogAsset = std::make_unique<Asset>("jogAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/run.glb"));
        _jumpAsset = std::make_unique<Asset>("jumpAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/jump-flip.glb"));
        _grassAsset = std::make_unique<Asset>("grass", AssetType::TEXTURE,
            assetPath("textures/terrain/sparse_grass_diff_1k.jpg"));
        _grassNormalAsset = std::make_unique<Asset>("grassNormal", AssetType::TEXTURE,
            assetPath("textures/terrain/sparse_grass_nor_1k.jpg"));
        _treeAsset = std::make_unique<Asset>("tree", AssetType::CONTAINER, assetPath("models/low-poly-tree.glb"));
        _envAtlasAsset = std::make_unique<Asset>("morning-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/morning-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});

        _idle = firstAnimTrack(_idleAsset.get());
        _walk = firstAnimTrack(_walkAsset.get());
        _jog = firstAnimTrack(_jogAsset.get());
        _jump = firstAnimTrack(_jumpAsset.get());
        if (!_idle || !_walk || !_jog || !_jump) {
            return false;
        }
        if (!_modelAsset->resourceAs<ContainerResource>()) {
            spdlog::error("bitmoji.glb failed to load");
            return false;
        }

        if (Texture* envAtlas = _envAtlasAsset->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }
        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(1.2f);
        scene()->setExposure(1.1f);

        // A soft haze which fades the far ground into the sky.
        scene()->setFogEnabled(true);
        scene()->setFogType(FogType::FOG_LINEAR);
        scene()->setFogColor(Color(0.66f, 0.8f, 0.88f, 1.0f));
        scene()->setFogLinear(50.0f, 400.0f);

        createGround();

        // A warm, low morning sun.
        Entity* sun = createDirectionalLight(Vector3(40.0f, 35.0f, 0.0f), Color(1.0f, 0.9f, 0.75f, 1.0f), 2.2f, true);
        sun->setName("Sun");
        if (auto* light = sun->findComponent<LightComponent>()) {
            light->setShadowType(SHADOW_PCF5_32F);
            light->setShadowBias(0.2f);
            light->setShadowNormalBias(0.05f);
            light->setShadowDistance(45.0f);
            light->setNumCascades(2);
            light->setCascadeDistribution(0.6f);
            light->setShadowResolution(2048);
        }

        createTrees();

        // The camera orbits (mouse) and flies (WASD with the right mouse button).
        Entity* camera = createCamera(Vector3(-5.0f, 4.5f, 15.0f));
        camera->setName("Camera");
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->setToneMapping(TONEMAP_ACES);
            component->camera()->setFov(50.0f);
            component->camera()->setFarClip(1500.0f);
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 1.0f, 0.0f))) {
            controls->setEnableFly(true);
            controls->setPitchRange(Vector2(-90.0f, -2.0f));
            controls->setZoomRange(Vector2(1.0f, 150.0f));
            controls->storeResetState();
        }

        // The state graph shared by all characters.
        _stateGraph.addIntegerParameter("speed", 0);
        _stateGraph.addTriggerParameter("jump");
        auto& layer = _stateGraph.addLayer("locomotion");
        for (const char* state : {"Idle", "Walk", "Jump", "Jog"}) {
            layer.states.push_back(AnimStateDesc{state});
        }
        using P = AnimPredicate;
        layer.transitions = {
            {.from = "START", .to = "Idle"},
            {.from = "Idle", .to = "Walk", .time = 0.1f, .conditions = {{"speed", P::GREATER_THAN, 0.0f}}},
            {.from = "ANY", .to = "Jump", .time = 0.1f, .conditions = {{"jump", P::EQUAL_TO, 1.0f}}},
            {.from = "Jump", .to = "Idle", .time = 0.2f, .exitTime = 0.8f},
            {.from = "Walk", .to = "Idle", .time = 0.1f, .conditions = {{"speed", P::LESS_THAN_EQUAL_TO, 0.0f}}},
            {.from = "Walk", .to = "Jog", .time = 0.1f, .conditions = {{"speed", P::GREATER_THAN, 1.0f}}},
            {.from = "Jog", .to = "Walk", .time = 0.1f, .conditions = {{"speed", P::LESS_THAN, 2.0f}}},
        };

        setCount(20);

        spdlog::info("Crowd: [ / ] characters -/+ 10, - / = -/+ 100, B the fixed bounding box, "
            "H shadows; {} characters", _characters.size());
        return true;
    }

    void update(const float dt) override
    {
        if (const auto* keyboard = engine()->keyboard()) {
            int count = static_cast<int>(_characters.size());
            if (keyboard->wasPressed(Key::LeftBracket)) {
                count -= 10;
            }
            if (keyboard->wasPressed(Key::RightBracket)) {
                count += 10;
            }
            if (keyboard->wasPressed(Key::Minus)) {
                count -= 100;
            }
            if (keyboard->wasPressed(Key::Equals)) {
                count += 100;
            }
            count = std::clamp(count, 1, kMaxCharacters);
            if (count != static_cast<int>(_characters.size())) {
                setCount(count);
                spdlog::info("Characters: {}", _characters.size());
            }
            if (keyboard->wasPressed(Key::B)) {
                setCustomAabb(!_customAabb);
                spdlog::info("Custom AABB {}", _customAabb ? "on" : "off");
            }
            if (keyboard->wasPressed(Key::H)) {
                setShadows(!_shadows);
                spdlog::info("Shadows {}", _shadows ? "on" : "off");
            }
        }

        for (Character& character : _characters) {
            updateCharacter(character, static_cast<double>(dt));
        }
    }

    void destroy() override
    {
        _characters.clear();
    }

private:
    void createGround()
    {
        Texture* grass = _grassAsset->resourceAs<Texture>();
        Texture* grassNormal = _grassNormalAsset->resourceAs<Texture>();
        for (Texture* map : {grass, grassNormal}) {
            if (map) {
                map->setAddressU(ADDRESS_REPEAT);
                map->setAddressV(ADDRESS_REPEAT);
            }
        }

        _groundMaterial = std::make_shared<StandardMaterial>();
        if (grass) {
            _groundMaterial->setDiffuseMap(grass);
        }
        if (grassNormal) {
            _groundMaterial->setNormalMap(grassNormal);
        }
        _groundMaterial->setDiffuse(Color(0.62f, 0.95f, 0.42f, 1.0f));
        _groundMaterial->setGloss(0.15f);
        _groundMaterial->setUseMetalness(true);
        _groundMaterial->setMetalness(0.0f);
        const float tiling = kGroundSize / kGrassTileSize;
        _groundMaterial->setDiffuseMapTiling(Vector2(tiling, tiling));
        _groundMaterial->setNormalMapTiling(Vector2(tiling, tiling));

        Entity* ground = createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(kGroundSize, 1.0f, kGroundSize));
        ground->setName("Ground");
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }
    }

    // Trees around the park, thinning out with distance. They are static, so they add little to
    // the per-frame CPU cost being profiled.
    void createTrees()
    {
        auto* container = _treeAsset->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("low-poly-tree.glb failed to load");
            return;
        }

        // The tree's size, to scale every tree to a unit height with its base on the ground.
        const std::unique_ptr<Entity> treeTemplate(container->instantiateRenderEntity());
        const BoundingBox treeAabb = entityBounds(treeTemplate.get());
        treeTemplate->destroy();
        const double treeUnitScale = 1.0 / (static_cast<double>(treeAabb.halfExtents().getY()) * 2.0);
        const double treeUnitBase = treeAabb.center().getY() - treeAabb.halfExtents().getY();

        Mulberry32 treeRandom(42);
        for (int i = 0; i < kTreeCount; ++i) {
            // The trees grow outside the area the characters wander in, so nobody walks through them.
            const double angle = treeRandom() * std::numbers::pi * 2.0;
            const double radius = kAreaRadius + 4.0 + treeRandom() * treeRandom() * 45.0;
            const double scale = treeUnitScale * (4.0 + treeRandom() * 3.0);

            auto* tree = new Entity();
            tree->setName("Tree" + std::to_string(i));
            tree->setEngine(engine());
            const auto s = static_cast<float>(scale);
            tree->setLocalScale(s, s, s);
            tree->setLocalEulerAngles(0.0f, static_cast<float>(treeRandom() * 360.0), 0.0f);
            tree->setLocalPosition(static_cast<float>(std::sin(angle) * radius),
                static_cast<float>(-treeUnitBase * scale), static_cast<float>(std::cos(angle) * radius));
            Entity* model = container->instantiateRenderEntity();
            for (auto* render : model->findComponents<RenderComponent>()) {
                render->setCastShadows(true);
            }
            tree->addChild(model);
            root()->addChild(tree);
        }
    }

    // A point uniformly distributed over the round area.
    void randomPoint(double& x, double& z)
    {
        const double angle = _random() * std::numbers::pi * 2.0;
        const double radius = std::sqrt(_random()) * kAreaRadius;
        x = std::sin(angle) * radius;
        z = std::cos(angle) * radius;
    }

    // The character entity of `character`: the model as its child, which the anim component
    // drives, while the entity itself is moved by the wandering logic.
    void buildEntity(Character& character, const int index, const int speed)
    {
        auto* entity = new Entity();
        entity->setName("Character" + std::to_string(index));
        entity->setEngine(engine());

        Entity* renderEntity = _modelAsset->resourceAs<ContainerResource>()->instantiateRenderEntity();
        entity->addChild(renderEntity);
        for (auto* render : entity->findComponents<RenderComponent>()) {
            render->setCastShadows(_shadows);
            render->setCustomAabb(_customAabb ? std::optional<BoundingBox>(_characterAabb) : std::nullopt);
        }

        auto* anim = static_cast<AnimComponent*>(entity->addComponent<AnimComponent>());
        anim->setActivate(true);
        anim->loadStateGraph(_stateGraph);
        anim->assignAnimation("Idle", _idle);
        anim->assignAnimation("Walk", _walk);
        anim->assignAnimation("Jog", _jog);
        anim->assignAnimation("Jump", _jump);
        anim->setInteger("speed", speed);

        character.entity = entity;
        character.anim = anim;
        entity->setLocalPosition(static_cast<float>(character.x), 0.0f, static_cast<float>(character.z));
        entity->setLocalEulerAngles(0.0f, static_cast<float>(character.heading), 0.0f);
        root()->addChild(entity);
    }

    void createCharacter()
    {
        Character character;
        randomPoint(character.x, character.z);
        character.targetX = character.x;
        character.targetZ = character.z;
        character.heading = _random() * 360.0;
        character.idleTime = _random() * 3.0;
        character.jumpTime = 2.0 + _random() * 6.0;
        buildEntity(character, static_cast<int>(_characters.size()), 0);
        _characters.push_back(std::move(character));
    }

    static void destroyEntity(Character& character)
    {
        if (character.entity) {
            // The parent owns the node: destroy tears the components down, remove frees it.
            character.entity->destroy();
            auto removed = character.entity->remove();
            character.entity = nullptr;
            character.anim = nullptr;
        }
    }

    void setCount(const int count)
    {
        while (static_cast<int>(_characters.size()) < count) {
            createCharacter();
        }
        while (static_cast<int>(_characters.size()) > count) {
            destroyEntity(_characters.back());
            _characters.pop_back();
        }
    }

    void setShadows(const bool castShadows)
    {
        _shadows = castShadows;
        for (const Character& character : _characters) {
            for (auto* render : character.entity->findComponents<RenderComponent>()) {
                render->setCastShadows(castShadows);
            }
        }
    }

    void setCustomAabb(const bool enabled)
    {
        _customAabb = enabled;
        for (const Character& character : _characters) {
            for (auto* render : character.entity->findComponents<RenderComponent>()) {
                render->setCustomAabb(enabled ? std::optional<BoundingBox>(_characterAabb) : std::nullopt);
            }
        }
    }

    // Moves a character toward its target, picking a new target once it arrives and an idle
    // pause runs out, and triggers the occasional jump.
    void updateCharacter(Character& character, const double dt)
    {
        AnimComponent* anim = character.anim;
        AnimComponentLayer* layer = anim ? anim->baseLayer() : nullptr;
        if (!layer) {
            return;
        }

        if (anim->getInteger("speed") == 0) {
            character.idleTime -= dt;
            if (character.idleTime <= 0.0) {
                randomPoint(character.targetX, character.targetZ);
                anim->setInteger("speed", _random() < 0.3 ? 2 : 1);
            }
            return;
        }

        // Occasionally jump while on the move.
        character.jumpTime -= dt;
        if (character.jumpTime <= 0.0) {
            character.jumpTime = 3.0 + _random() * 8.0;
            if (layer->activeState() != "Jump") {
                anim->setTrigger("jump");
            }
        }

        // Blend the movement speed during transitions to limit foot sliding.
        double moveSpeed = speedForState(layer->activeState());
        if (layer->transitioning()) {
            const double progress = layer->transitionProgress();
            const double previous = speedForState(layer->previousState());
            moveSpeed = previous + (moveSpeed - previous) * progress;
        }

        const double deltaX = character.targetX - character.x;
        const double deltaZ = character.targetZ - character.z;
        const double distance = std::sqrt(deltaX * deltaX + deltaZ * deltaZ);
        const double step = moveSpeed * dt;

        if (step >= distance) {
            character.x = character.targetX;
            character.z = character.targetZ;
            anim->setInteger("speed", 0);
            character.idleTime = 1.0 + _random() * 3.0;
        } else if (distance > 0.0) {
            character.x += deltaX / distance * step;
            character.z += deltaZ / distance * step;

            // Turn smoothly toward the target.
            const double targetHeading = std::atan2(deltaX, deltaZ) * 180.0 / std::numbers::pi;
            const double diff = std::fmod(targetHeading - character.heading + 540.0, 360.0) - 180.0;
            character.heading += std::min(1.0, dt * 6.0) * diff;
        }

        character.entity->setLocalPosition(static_cast<float>(character.x), 0.0f, static_cast<float>(character.z));
        character.entity->setLocalEulerAngles(0.0f, static_cast<float>(character.heading), 0.0f);
    }

    std::unique_ptr<Asset> _modelAsset;
    std::unique_ptr<Asset> _idleAsset;
    std::unique_ptr<Asset> _walkAsset;
    std::unique_ptr<Asset> _jogAsset;
    std::unique_ptr<Asset> _jumpAsset;
    std::unique_ptr<Asset> _grassAsset;
    std::unique_ptr<Asset> _grassNormalAsset;
    std::unique_ptr<Asset> _treeAsset;
    std::unique_ptr<Asset> _envAtlasAsset;

    std::shared_ptr<AnimTrack> _idle;
    std::shared_ptr<AnimTrack> _walk;
    std::shared_ptr<AnimTrack> _jog;
    std::shared_ptr<AnimTrack> _jump;
    AnimStateGraph _stateGraph;

    std::shared_ptr<StandardMaterial> _groundMaterial;

    // A conservative box around a character in all its poses, jumps included. Without it, the
    // bounds of a skinned mesh are computed from its animated bones every frame, which needs the
    // bone transforms of every character before culling, even of those that end up off screen.
    const BoundingBox _characterAabb{Vector3(0.0f, 0.85f, 0.0f), Vector3(1.05f, 1.05f, 1.05f)};

    std::vector<Character> _characters;
    Mulberry32 _random{1234};
    bool _shadows = true;
    bool _customAabb = true;
};

VISUTWIN_EXAMPLE_MAIN(CrowdExample)
