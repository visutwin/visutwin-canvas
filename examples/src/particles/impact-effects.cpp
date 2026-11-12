// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// Port of upstream particles/impact-effects.
//
// Bullet impacts, explosions and magic bursts in the planar-reflection room, each effect built
// from several one-shot particle emitters, an optional light flash, optional decals left on the
// surface and an optional camera shake. Click the room to hit it, or let auto fire do it: a shot
// is a physics raycast from the camera against static mesh colliders of the room, the statue,
// the chrome sphere and the gold torus, and a bullet plays a different effect on metal (the room,
// the sphere, the torus) and on stone (the statue), told by the tag of the collider it hit. Each
// effect faces along the normal of the surface it hit.
//
// An effect is data (createImpactEffects): emitters with their particle options and delays, a
// light, decals. ParticleEffectPool plays them by name, reusing the instances that have finished,
// up to each effect's instance limit, after which the instance that has played the longest
// restarts at the new place. The sprites are the lens-flare textures, the smoke and the fireball
// 8x8 WebP flipbooks played once over each particle's life; the chips and debris are lit box
// mesh particles. The soft particles fade where they meet the room, through the camera's scene
// depth map. The polished floor reflects everything, the effects included, through a planar
// reflection rendered by a second camera; the camera frame (4x MSAA, ACES) adds bloom and a
// vignette.
//
// Keys stand in for upstream's controls panel:
//   1 / 2 / 3 / 4   effect: mixed / bullets / explosions / magic
//   F               auto fire on / off
//   - / =           shots per second -/+ (0.5 - 30)
//   X               soft particles on / off
//   I               log the effects playing and the instances created (upstream's Stats panel)
// A left click that moves the pointer no more than 5 points fires at the point under it; a
// drag orbits the camera.
//
// A decal's texture alpha is its coverage. A lit decal reads it through its diffuse map alone:
// here a base colour map's alpha already multiplies the opacity, and setting the same texture
// as the opacity map too would square it. A glowing decal, which has no diffuse map, takes it
// through its opacity map, as upstream's both do.
//
// DEVIATIONS:
// - Upstream shoots through the Ammo physics engine; the port supplies the Jolt world. The torus
//   collider is built from the torus's own render mesh rather than a separate model.
// - The magic burst's orbs spawn anywhere inside their 0.15 sphere: the particle system here has
//   no inner emitter radius (upstream spawns them on a shell from 0.12 to 0.15).
// - Upstream clamps a non-looping flipbook at its last frame; the particle program here wraps.
//   At an animation speed of 1, which every flipbook here plays at, a frame never wraps within a
//   particle's life, so the two agree.
// - The glTF materials are gathered from the instantiated meshes rather than the container's
//   material list, and our glTF materials keep gloss as 1 - roughness (upstream's loader stores
//   the roughness in gloss), so the floor's roughness 0.1 is written as gloss 0.9.
// - The camera shake and the reflection camera's update run after the engine update (in
//   preRender), where the camera controls have set the camera's transform for the frame: the
//   example's update runs before them here.
// - The random numbers come from a seeded generator, so a run is repeatable.
// - The stats panel is a log line on a key; the examples render at one pixel per point, where
//   upstream renders at up to two.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "exampleApp.h"
#include "core/math/curve.h"
#include "core/math/curveSet.h"
#include "core/math/quaternion.h"
#include "extras/script/cameraControls.h"
#include "extras/script/planarRenderer.h"
#include "framework/assets/asset.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/physics/jolt/joltPhysicsWorld.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/texture.h"
#include "platform/input/mouse.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // The layer of the floor, rendered only by the main camera: a surface cannot be in the
    // reflection it displays.
    constexpr int LAYERID_FLOOR = 100;

    // ----- Curves -----

    Curve curve(const std::initializer_list<float> keys)
    {
        Curve result{std::vector<float>(keys)};
        result.type = CURVE_LINEAR;
        return result;
    }

    CurveSet curves(const std::vector<float>& x, const std::vector<float>& y, const std::vector<float>& z)
    {
        CurveSet result({x, y, z});
        result.setType(CURVE_LINEAR);
        return result;
    }

    // A constant vector or colour.
    CurveSet constant(const float x, const float y, const float z)
    {
        return curves({0.0f, x}, {0.0f, y}, {0.0f, z});
    }

    // A colour gradient over the lifetime, from stops of time, red, green and blue.
    CurveSet gradient(const std::initializer_list<std::array<float, 4>> stops)
    {
        std::vector<float> r, g, b;
        for (const auto& [t, red, green, blue] : stops) {
            r.insert(r.end(), {t, red});
            g.insert(g.end(), {t, green});
            b.insert(b.end(), {t, blue});
        }
        return curves(r, g, b);
    }

    // The world velocity of a falling particle over its lifetime.
    CurveSet gravity(const float acceleration, const float lifetime)
    {
        return curves({0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, -acceleration * lifetime}, {0.0f, 0.0f});
    }

    // The options every layer starts from: the pool, its lifetime and the seconds between births
    // (0, a burst). The lifetime has no random range here, as upstream's has none.
    ParticleEmitterOptions emitter(const uint32_t count, const float lifetime, const float rate)
    {
        ParticleEmitterOptions o;
        o.numParticles = count;
        o.lifetime = lifetime;
        o.lifetime2 = lifetime;
        o.rate = rate;
        return o;
    }

    // Plays an 8x8 flipbook once over each particle's life.
    void flipbook(ParticleEmitterOptions& o, Texture* texture)
    {
        o.colorMap = texture;
        o.animTilesX = 8;
        o.animTilesY = 8;
        o.animNumFrames = 64;
        o.animSpeed = 1.0f;
    }

    // ----- Effect definitions -----

    // One emitter of an effect, made a one-shot by the effect.
    struct EffectLayer
    {
        std::string name;
        ParticleEmitterOptions options;
        // The seconds from the start of the effect until this layer emits.
        float delay = 0.0f;
        // The position in the effect's space, where +Y points along the surface normal.
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
    };

    // A light flash that fades out over its duration.
    struct EffectLight
    {
        Color color;
        float intensity = 1.0f;
        float range = 1.0f;
        float duration = 1.0f;
        float offset = 0.5f;    // along the surface normal
        float flicker = 0.0f;   // 0-1
    };

    // A mark left on the surface that fades out at its end.
    struct EffectDecal
    {
        std::shared_ptr<Mesh> mesh;                   // a unit plane facing +Y
        std::shared_ptr<StandardMaterial> material;   // shared by every instance of the effect
        float sizeMin = 1.0f;
        float sizeMax = 1.0f;
        float duration = 1.0f;   // including the fade
        float fade = 1.0f;
        float delay = 0.0f;
    };

    struct EffectDefinition
    {
        std::vector<EffectLayer> layers;
        std::optional<EffectLight> light;
        std::vector<EffectDecal> decals;
        float shake = 0.0f;
        // The most instances playing at once; with all playing, the oldest restarts.
        size_t maxInstances = 16;
    };

    // A decal's material: lit and tinted, or glowing with `glow` as its emissive intensity.
    std::shared_ptr<StandardMaterial> createDecalMaterial(Texture* texture, const Color& color, const float glow)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setTransparent(true);
        // Pulled toward the camera in the depth test, so the surface does not hide it, even
        // at a grazing angle.
        auto depth = std::make_shared<DepthState>();
        depth->setDepthTest(true);
        depth->setDepthWrite(false);
        depth->setDepthBias(-0.1f);
        depth->setSlopeDepthBias(-2.0f);
        material->setDepthState(depth);
        if (glow > 0.0f) {
            material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
            material->setEmissive(color);
            material->setEmissiveMap(texture);
            material->setEmissiveIntensity(glow);
            material->setOpacityMap(texture);   // its alpha, the coverage
            material->setUseLighting(false);
            material->setBlendState(std::make_shared<BlendState>(BlendState::additiveBlend()));
        } else {
            material->setDiffuse(color);
            material->setDiffuseMap(texture);   // its alpha, the coverage
            material->setGloss(0.2f);
            material->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        }
        return material;
    }

    // The rotation turning +Y onto `to`, the shortest arc.
    Quaternion rotationFromUp(const Vector3& to)
    {
        const float d = 1.0f + to.getY();
        if (d < 1e-6f) {
            return Quaternion(0.0f, 0.0f, 1.0f, 0.0f);   // straight down: half a turn about Z
        }
        return Quaternion(to.getZ(), 0.0f, -to.getX(), d).normalized();
    }

    // ----- The pool -----

    // One instance of an effect: an entity with a child per emitter, a light and the decals.
    class ParticleEffect
    {
    public:
        ParticleEffect(Engine* engine, Entity* parent, const std::string& name, const EffectDefinition& definition,
            const std::vector<int>& lightLayers, const bool softParticles)
            : _definition(definition)
        {
            _entity = new Entity();
            _entity->setEngine(engine);
            _entity->setName(name);
            parent->addChild(_entity);
            _entity->setEnabled(false);

            for (const auto& layer : definition.layers) {
                auto* entity = new Entity();
                entity->setEngine(engine);
                entity->setName(layer.name);
                entity->setLocalPosition(layer.position);
                _entity->addChild(entity);
                auto* ps = static_cast<ParticleSystemComponent*>(entity->addComponent<ParticleSystemComponent>());
                ps->options() = layer.options;
                ps->options().loop = false;
                ps->options().autoPlay = false;
                if (!softParticles) {
                    ps->options().depthSoftening = 0.0f;
                }
                ps->apply();
                // A layer waiting for its delay stays disabled, so the previous play does not show.
                entity->setEnabled(false);
                _layers.push_back({entity, ps, layer.delay, false});

                // A one-shot emits for rate * numParticles seconds, and the last particle then
                // lives for the lifetime.
                const auto& o = layer.options;
                const float emitDuration = std::max(o.rate, o.rate2.value_or(o.rate)) * static_cast<float>(o.numParticles);
                _duration = std::max(_duration, layer.delay + emitDuration + o.lifetime);
            }

            if (definition.light) {
                const EffectLight& light = *definition.light;
                auto* entity = new Entity();
                entity->setEngine(engine);
                entity->setName("Light");
                entity->setLocalPosition(0.0f, light.offset, 0.0f);
                _entity->addChild(entity);
                _light = static_cast<LightComponent*>(entity->addComponent<LightComponent>());
                _light->setType(LightType::LIGHTTYPE_OMNI);
                _light->setColor(light.color);
                _light->setIntensity(0.0f);
                _light->setRange(light.range);
                _light->setCastShadows(false);
                _light->setLayers(lightLayers);
                _duration = std::max(_duration, light.duration);
            }

            for (size_t i = 0; i < definition.decals.size(); ++i) {
                const EffectDecal& decal = definition.decals[i];
                auto* entity = new Entity();
                entity->setEngine(engine);
                entity->setName("Decal");
                // Lifted off the surface, so it does not fight with it in the depth buffer, and
                // each decal a little higher than the one before.
                entity->setLocalPosition(0.0f, 0.02f + static_cast<float>(i) * 0.005f, 0.0f);
                _entity->addChild(entity);
                auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
                render->setCastShadows(false);
                auto meshInstance = std::make_unique<MeshInstance>(decal.mesh, decal.material, entity);
                meshInstance->setCastShadows(false);
                MeshInstance* instance = render->addMeshInstance(std::move(meshInstance));
                entity->setEnabled(false);
                _decals.push_back({entity, instance, &decal, -1.0f});
                _duration = std::max(_duration, decal.delay + decal.duration);
            }
        }

        // Starts the effect at `position`, its +Y along `normal`, restarting it if it plays.
        void play(const Vector3& position, const Vector3& normal, std::mt19937& rng)
        {
            _entity->setPosition(position);
            _entity->setRotation(rotationFromUp(normal));
            _entity->setEnabled(true);

            for (auto& layer : _layers) {
                layer.entity->setEnabled(false);
                layer.started = false;
            }

            // Every decal is a little different.
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            for (auto& decal : _decals) {
                const float size = decal.definition->sizeMin + unit(rng) * (decal.definition->sizeMax - decal.definition->sizeMin);
                decal.entity->setLocalScale(size, 1.0f, size);
                decal.entity->setLocalEulerAngles(0.0f, unit(rng) * 360.0f, 0.0f);
                decal.entity->setEnabled(false);
            }

            _time = 0.0f;
            _playing = true;
            update(0.0f, rng);
        }

        // Starts the layers whose delay has passed, fades the light and the decals.
        void update(const float dt, std::mt19937& rng)
        {
            if (!_playing) {
                return;
            }
            _time += dt;

            for (auto& layer : _layers) {
                if (!layer.started && _time >= layer.delay) {
                    layer.started = true;
                    layer.entity->setEnabled(true);
                    layer.ps->reset();
                    layer.ps->play();
                }
            }

            if (_light && _definition.light) {
                const EffectLight& light = *_definition.light;
                const float fade = std::max(0.0f, 1.0f - _time / light.duration);
                const float flicker = 1.0f - light.flicker * std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
                _light->setIntensity(light.intensity * fade * fade * flicker);
            }

            for (auto& decal : _decals) {
                const EffectDecal& d = *decal.definition;
                const float age = _time - d.delay;
                decal.entity->setEnabled(age >= 0.0f && age < d.duration);
                const float opacity = std::clamp((d.duration - age) / d.fade, 0.0f, 1.0f);
                if (opacity != decal.opacity) {
                    decal.opacity = opacity;
                    decal.meshInstance->setParameter("material_opacity", opacity);
                }
            }

            if (_time >= _duration) {
                _playing = false;
                _entity->setEnabled(false);
            }
        }

        // Applies the soft-particle setting to the existing emitters.
        void setSoftParticles(const bool soft)
        {
            for (size_t i = 0; i < _layers.size(); ++i) {
                _layers[i].ps->options().depthSoftening = soft ? _definition.layers[i].options.depthSoftening : 0.0f;
                _layers[i].ps->apply();
            }
        }

        [[nodiscard]] bool playing() const { return _playing; }
        [[nodiscard]] float time() const { return _time; }

    private:
        struct Layer
        {
            Entity* entity = nullptr;
            ParticleSystemComponent* ps = nullptr;
            float delay = 0.0f;
            bool started = false;
        };

        struct Decal
        {
            Entity* entity = nullptr;
            MeshInstance* meshInstance = nullptr;
            const EffectDecal* definition = nullptr;
            float opacity = -1.0f;   // the opacity last set
        };

        const EffectDefinition& _definition;
        Entity* _entity = nullptr;
        std::vector<Layer> _layers;
        LightComponent* _light = nullptr;
        std::vector<Decal> _decals;
        float _time = 0.0f;
        float _duration = 0.0f;
        bool _playing = false;
    };

    // Plays effects by name, reusing the instances that have finished. Instances are created on
    // demand, up to each effect's maxInstances.
    class ParticleEffectPool
    {
    public:
        ParticleEffectPool(Engine* engine, Entity* parent, std::map<std::string, EffectDefinition> definitions,
            std::vector<int> lightLayers)
            : _engine(engine), _parent(parent), _definitions(std::move(definitions)), _lightLayers(std::move(lightLayers))
        {
        }

        // Plays an effect; returns its camera shake strength.
        float spawn(const std::string& name, const Vector3& position, const Vector3& normal, std::mt19937& rng)
        {
            const auto it = _definitions.find(name);
            if (it == _definitions.end()) {
                spdlog::error("impact-effects: unknown particle effect '{}'", name);
                return 0.0f;
            }
            const EffectDefinition& definition = it->second;
            auto& instances = _instances[name];

            // Reuse a finished instance, create a new one, or restart the one that played longest.
            ParticleEffect* effect = nullptr;
            for (const auto& instance : instances) {
                if (!instance->playing()) {
                    effect = instance.get();
                    break;
                }
            }
            if (!effect) {
                if (instances.size() < definition.maxInstances) {
                    instances.push_back(std::make_unique<ParticleEffect>(_engine, _parent, name, definition,
                        _lightLayers, _softParticles));
                    effect = instances.back().get();
                } else {
                    effect = std::max_element(instances.begin(), instances.end(),
                        [](const auto& a, const auto& b) { return a->time() < b->time(); })->get();
                }
            }
            effect->play(position, normal, rng);
            return definition.shake;
        }

        void update(const float dt, std::mt19937& rng)
        {
            for (auto& [name, instances] : _instances) {
                for (auto& instance : instances) {
                    instance->update(dt, rng);
                }
            }
        }

        // The instances still to be created and the existing ones.
        void setSoftParticles(const bool soft)
        {
            _softParticles = soft;
            for (auto& [name, instances] : _instances) {
                for (auto& instance : instances) {
                    instance->setSoftParticles(soft);
                }
            }
        }

        [[nodiscard]] size_t activeCount() const
        {
            size_t count = 0;
            for (const auto& [name, instances] : _instances) {
                count += static_cast<size_t>(std::count_if(instances.begin(), instances.end(),
                    [](const auto& instance) { return instance->playing(); }));
            }
            return count;
        }

        [[nodiscard]] size_t instanceCount() const
        {
            size_t count = 0;
            for (const auto& [name, instances] : _instances) {
                count += instances.size();
            }
            return count;
        }

    private:
        Engine* _engine;
        Entity* _parent;
        // A map, so a definition's address (which each instance keeps) never moves.
        std::map<std::string, EffectDefinition> _definitions;
        std::vector<int> _lightLayers;
        std::map<std::string, std::vector<std::unique_ptr<ParticleEffect>>> _instances;
        bool _softParticles = true;
    };

    // The textures and meshes of the effects.
    struct EffectResources
    {
        Texture* smoke = nullptr;       // 8x8 flipbook of a billowing smoke puff, baked lighting
        Texture* explosion = nullptr;   // 8x8 flipbook of a fireball cooling into smoke
        Texture* glow = nullptr;        // a soft round glow
        Texture* star = nullptr;        // a star-shaped flash
        Texture* spike = nullptr;       // a thin four-pointed glint
        Texture* streak = nullptr;      // a horizontal lens streak
        Texture* halo = nullptr;        // a soft ring, for shockwaves
        Texture* scorch = nullptr;      // a scorch mark decal
        Texture* bulletHole = nullptr;  // a bullet hole decal
        Texture* rune = nullptr;        // a rune circle decal
        Texture* white = nullptr;       // plain white, the mesh particles' colour map
        std::shared_ptr<Mesh> chip;     // a small box, for stone chips and debris
        std::shared_ptr<Mesh> quad;     // a unit plane facing +Y, for decals
    };

    // The effects: bullet hits on metal and stone, an explosion and a magic burst. Each faces
    // along the surface normal: +Y of the emitters points away from the surface, so local
    // velocities push particles out of it, while gravity and rising smoke are world velocities.
    // Sizes are in metres (the room is 16 wide); a sprite's scale is half its width.
    std::map<std::string, EffectDefinition> createImpactEffects(const EffectResources& res)
    {
        // A single short flash, at a random angle.
        const auto flash = [](Texture* texture, const std::array<float, 3>& color, const float size,
                               const float lifetime, const float intensity, const bool rotate = true) {
            EffectLayer layer{.name = "Flash", .options = emitter(1, lifetime, 0.0f)};
            auto& o = layer.options;
            o.colorMap = texture;
            o.blendType = ParticleBlendType::BLEND_ADDITIVE;
            o.depthWrite = false;
            o.startAngle = 0.0f;
            o.startAngle2 = rotate ? 360.0f : 0.0f;
            o.scaleGraph = curve({0.0f, size * 0.7f, 1.0f, size});
            o.colorGraph = constant(color[0], color[1], color[2]);
            o.alphaGraph = curve({0.0f, 1.0f, 1.0f, 0.0f});
            o.intensity = intensity;
            return layer;
        };

        // Hot sparks thrown out of the surface, stretched along their motion and falling.
        const auto sparks = [&res](const uint32_t count, const std::array<float, 2>& speed, const float spread,
                                const float lifetime, const float size, const float intensity) {
            EffectLayer layer{.name = "Sparks", .options = emitter(count, lifetime, 0.0f)};
            auto& o = layer.options;
            o.colorMap = res.glow;
            o.blendType = ParticleBlendType::BLEND_ADDITIVE;
            o.depthWrite = false;
            o.localVelocityGraph = constant(-spread, speed[0], -spread);
            o.localVelocityGraph2 = constant(spread, speed[1], spread);
            o.velocityGraph = gravity(9.8f, lifetime);
            o.alignToMotion = true;
            o.stretch = 0.08f;
            o.scaleGraph = curve({0.0f, size, 1.0f, size * 0.3f});
            o.scaleGraph2 = curve({0.0f, size * 0.5f, 1.0f, size * 0.2f});
            o.colorGraph = gradient({{0.0f, 1.0f, 0.95f, 0.8f}, {0.3f, 1.0f, 0.65f, 0.25f}, {1.0f, 0.9f, 0.25f, 0.05f}});
            o.alphaGraph = curve({0.0f, 1.0f, 0.7f, 1.0f, 1.0f, 0.0f});
            o.intensity = intensity;
            return layer;
        };

        // A puff of smoke or dust from the smoke flipbook, bursting out of the surface and
        // slowing down as it grows.
        struct Puff
        {
            const char* name;
            uint32_t count;
            std::array<float, 3> color;
            std::array<float, 2> size;
            std::array<float, 2> speed;
            float spread;
            float lifetime;
            float opacity;
            float rise = 0.0f;
            float delay = 0.0f;
        };
        const auto puff = [&res](const Puff& p) {
            EffectLayer layer{.name = p.name, .options = emitter(p.count, p.lifetime, 0.0f), .delay = p.delay};
            auto& o = layer.options;
            flipbook(o, res.smoke);
            o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
            o.emitterRadius = p.size[0] * 0.3f;
            o.blendType = ParticleBlendType::BLEND_NORMAL;
            o.depthWrite = false;
            o.localVelocityGraph = curves({0.0f, -p.spread, 0.3f, -p.spread * 0.15f, 1.0f, 0.0f},
                {0.0f, p.speed[0], 0.3f, p.speed[0] * 0.15f, 1.0f, 0.0f},
                {0.0f, -p.spread, 0.3f, -p.spread * 0.15f, 1.0f, 0.0f});
            o.localVelocityGraph2 = curves({0.0f, p.spread, 0.3f, p.spread * 0.15f, 1.0f, 0.0f},
                {0.0f, p.speed[1], 0.3f, p.speed[1] * 0.15f, 1.0f, 0.0f},
                {0.0f, p.spread, 0.3f, p.spread * 0.15f, 1.0f, 0.0f});
            o.velocityGraph = constant(0.0f, p.rise, 0.0f);
            o.startAngle = 0.0f;
            o.startAngle2 = 360.0f;
            o.rotationSpeedGraph = curve({0.0f, -30.0f});
            o.rotationSpeedGraph2 = curve({0.0f, 30.0f});
            o.scaleGraph = curve({0.0f, p.size[0], 0.4f, p.size[1] * 0.8f, 1.0f, p.size[1]});
            o.scaleGraph2 = curve({0.0f, p.size[0] * 0.8f, 0.4f, p.size[1] * 0.6f, 1.0f, p.size[1] * 0.75f});
            o.colorGraph = constant(p.color[0], p.color[1], p.color[2]);
            o.alphaGraph = curve({0.0f, 0.0f, 0.05f, p.opacity, 1.0f, p.opacity});
            o.depthSoftening = 0.08f;
            return layer;
        };

        // Solid mesh pieces thrown out of the surface, tumbling as they fall.
        const auto debris = [&res](const char* name, const uint32_t count, const std::array<float, 3>& color,
                                const float size, const std::array<float, 2>& speed, const float spread,
                                const float lifetime) {
            EffectLayer layer{.name = name, .options = emitter(count, lifetime, 0.0f)};
            auto& o = layer.options;
            o.mesh = res.chip;
            o.colorMap = res.white;
            o.blendType = ParticleBlendType::BLEND_NONE;
            o.depthWrite = true;
            o.lighting = true;
            o.halfLambert = true;
            o.localVelocityGraph = constant(-spread, speed[0], -spread);
            o.localVelocityGraph2 = constant(spread, speed[1], spread);
            o.velocityGraph = gravity(14.0f, lifetime);
            o.startAngle = 0.0f;
            o.startAngle2 = 360.0f;
            o.rotationSpeedGraph = curve({0.0f, -720.0f});
            o.rotationSpeedGraph2 = curve({0.0f, 720.0f});
            o.scaleGraph = curve({0.0f, size, 0.85f, size, 1.0f, 0.0f});
            o.scaleGraph2 = curve({0.0f, size * 0.5f, 0.85f, size * 0.5f, 1.0f, 0.0f});
            o.colorGraph = constant(color[0], color[1], color[2]);
            return layer;
        };

        // A ring expanding across the surface.
        const auto shockwave = [&res](const std::array<float, 3>& color, const float size, const float lifetime,
                                   const float intensity, const float delay = 0.0f) {
            // Lifted off the surface, so it does not intersect it.
            EffectLayer layer{.name = "Shockwave", .options = emitter(1, lifetime, 0.0f), .delay = delay,
                .position = Vector3(0.0f, 0.05f, 0.0f)};
            auto& o = layer.options;
            o.colorMap = res.halo;
            o.blendType = ParticleBlendType::BLEND_ADDITIVE;
            o.depthWrite = false;
            // Lies flat on the surface: the particle normal is +Y of the emitter.
            o.orientation = ParticleOrientation::EMITTER;
            o.particleNormal = Vector3(0.0f, 1.0f, 0.0f);
            o.scaleGraph = curve({0.0f, size * 0.1f, 0.4f, size * 0.75f, 1.0f, size});
            o.colorGraph = constant(color[0], color[1], color[2]);
            o.alphaGraph = curve({0.0f, 1.0f, 1.0f, 0.0f});
            o.intensity = intensity;
            return layer;
        };

        // Small glowing motes that drift up and twinkle.
        struct Motes
        {
            const char* name;
            Texture* texture;
            uint32_t count;
            float spawnTime;   // the seconds over which they spawn
            float radius;
            std::array<float, 2> rise;
            float lifetime;
            float size;
            CurveSet color;
            float intensity;
            float delay = 0.0f;
            bool flat = false;   // spawn on a flat disc instead of a sphere
        };
        const auto motes = [](const Motes& m) {
            EffectLayer layer{.name = m.name, .options = emitter(m.count, m.lifetime,
                m.spawnTime / static_cast<float>(m.count)), .delay = m.delay};
            auto& o = layer.options;
            o.emitterShape = m.flat ? ParticleEmitterShape::EMITTERSHAPE_BOX : ParticleEmitterShape::EMITTERSHAPE_SPHERE;
            o.emitterExtents = Vector3(m.radius * 2.0f, 0.0f, m.radius * 2.0f);
            o.emitterRadius = m.radius;
            o.colorMap = m.texture;
            o.blendType = ParticleBlendType::BLEND_ADDITIVE;
            o.depthWrite = false;
            o.localVelocityGraph = constant(-0.3f, m.rise[0], -0.3f);
            o.localVelocityGraph2 = constant(0.3f, m.rise[1], 0.3f);
            o.startAngle = 0.0f;
            o.startAngle2 = 360.0f;
            o.rotationSpeedGraph = curve({0.0f, -90.0f});
            o.rotationSpeedGraph2 = curve({0.0f, 90.0f});
            o.scaleGraph = curve({0.0f, m.size, 1.0f, m.size * 0.3f});
            o.scaleGraph2 = curve({0.0f, m.size * 0.5f, 1.0f, m.size * 0.15f});
            o.colorGraph = m.color;
            // twinkle
            o.alphaGraph = curve({0.0f, 0.0f, 0.1f, 1.0f, 0.25f, 0.3f, 0.4f, 1.0f, 0.55f, 0.3f, 0.7f, 1.0f, 1.0f, 0.0f});
            o.intensity = m.intensity;
            return layer;
        };

        const auto decal = [&res](Texture* texture, const Color& color, const float sizeMin, const float sizeMax,
                               const float duration, const float fade, const float delay = 0.0f,
                               const float glow = 0.0f) {
            return EffectDecal{.mesh = res.quad, .material = createDecalMaterial(texture, color, glow), .sizeMin = sizeMin,
                .sizeMax = sizeMax, .duration = duration, .fade = fade, .delay = delay};
        };

        std::map<std::string, EffectDefinition> effects;

        {
            EffectDefinition& e = effects["bullet-metal"];
            e.maxInstances = 24;
            e.light = EffectLight{.color = Color(1.0f, 0.65f, 0.35f, 1.0f), .intensity = 3.0f, .range = 2.5f,
                .duration = 0.08f, .offset = 0.2f};
            // bare steel shows around the hole
            e.decals.push_back(decal(res.bulletHole, Color(0.9f, 0.92f, 0.95f, 1.0f), 0.3f, 0.38f, 10.0f, 2.0f));
            e.layers.push_back(flash(res.star, {1.0f, 0.85f, 0.6f}, 0.35f, 0.06f, 8.0f));
            e.layers.push_back(flash(res.glow, {1.0f, 0.6f, 0.3f}, 0.4f, 0.08f, 4.0f));
            e.layers.push_back(sparks(36, {3.0f, 8.0f}, 3.5f, 0.5f, 0.025f, 8.0f));
            e.layers.push_back(puff({.name = "Smoke", .count = 3, .color = {0.55f, 0.55f, 0.58f}, .size = {0.05f, 0.3f},
                .speed = {0.3f, 0.8f}, .spread = 0.15f, .lifetime = 1.2f, .opacity = 0.45f, .rise = 0.3f}));
        }

        {
            EffectDefinition& e = effects["bullet-stone"];
            e.maxInstances = 24;
            // pale chipped stone shows around the hole
            e.decals.push_back(decal(res.bulletHole, Color(0.95f, 0.88f, 0.78f, 1.0f), 0.32f, 0.4f, 10.0f, 2.0f));
            e.layers.push_back(flash(res.spike, {1.0f, 0.8f, 0.55f}, 0.2f, 0.05f, 6.0f));
            e.layers.push_back(puff({.name = "Dust", .count = 5, .color = {0.62f, 0.56f, 0.48f}, .size = {0.08f, 0.6f},
                .speed = {0.8f, 2.0f}, .spread = 0.6f, .lifetime = 1.6f, .opacity = 0.75f, .rise = 0.15f}));
            e.layers.push_back(debris("Chips", 12, {0.45f, 0.42f, 0.38f}, 0.05f, {2.0f, 4.0f}, 1.5f, 1.0f));
            e.layers.push_back(sparks(5, {2.0f, 4.0f}, 1.5f, 0.2f, 0.015f, 4.0f));
        }

        {
            EffectDefinition& e = effects["explosion"];
            e.maxInstances = 6;
            e.shake = 0.7f;
            e.light = EffectLight{.color = Color(1.0f, 0.55f, 0.25f, 1.0f), .intensity = 14.0f, .range = 12.0f,
                .duration = 1.0f, .offset = 1.0f, .flicker = 0.25f};
            // a scorch mark, and hot embers glowing on it as they cool down
            e.decals.push_back(decal(res.scorch, Color(1.0f, 1.0f, 1.0f, 1.0f), 4.5f, 5.5f, 14.0f, 4.0f, 0.1f));
            e.decals.push_back(decal(res.scorch, Color(1.0f, 0.35f, 0.08f, 1.0f), 2.5f, 3.0f, 4.0f, 3.5f, 0.1f, 3.0f));
            e.layers.push_back(flash(res.glow, {1.0f, 0.85f, 0.6f}, 3.0f, 0.15f, 6.0f));
            e.layers.push_back(flash(res.streak, {1.0f, 0.7f, 0.4f}, 4.0f, 0.25f, 3.0f, false));
            {
                // The fireball flipbook cools from fire into dark smoke. The intensity above 1
                // makes its fire bloom.
                EffectLayer layer{.name = "Fireball", .options = emitter(7, 1.7f, 0.0f)};
                auto& o = layer.options;
                flipbook(o, res.explosion);
                o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
                o.emitterRadius = 0.35f;
                o.blendType = ParticleBlendType::BLEND_NORMAL;
                o.depthWrite = false;
                o.radialSpeedGraph = curve({0.0f, 2.5f, 0.3f, 0.3f, 1.0f, 0.0f});
                o.radialSpeedGraph2 = curve({0.0f, 1.2f, 0.3f, 0.2f, 1.0f, 0.0f});
                o.localVelocityGraph = constant(0.0f, 0.4f, 0.0f);
                o.localVelocityGraph2 = constant(0.0f, 1.2f, 0.0f);
                o.startAngle = 0.0f;
                o.startAngle2 = 360.0f;
                o.rotationSpeedGraph = curve({0.0f, -20.0f});
                o.rotationSpeedGraph2 = curve({0.0f, 20.0f});
                o.scaleGraph = curve({0.0f, 0.9f, 1.0f, 1.7f});
                o.scaleGraph2 = curve({0.0f, 0.7f, 1.0f, 1.3f});
                // fades into the lighter smoke column, instead of ending as a dark cloud
                o.alphaGraph = curve({0.0f, 1.0f, 0.55f, 1.0f, 1.0f, 0.0f});
                o.intensity = 3.0f;
                o.depthSoftening = 0.1f;
                e.layers.push_back(std::move(layer));
            }
            {
                // starts as the fireball turns to smoke, and keeps rising
                EffectLayer layer{.name = "Smoke", .options = emitter(12, 4.5f, 0.03f), .delay = 0.5f};
                auto& o = layer.options;
                flipbook(o, res.smoke);
                o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
                o.emitterRadius = 0.6f;
                o.blendType = ParticleBlendType::BLEND_NORMAL;
                o.depthWrite = false;
                o.localVelocityGraph = curves({0.0f, -0.2f}, {0.0f, 1.2f, 1.0f, 0.3f}, {0.0f, -0.2f});
                o.localVelocityGraph2 = curves({0.0f, 0.2f}, {0.0f, 2.0f, 1.0f, 0.6f}, {0.0f, 0.2f});
                o.startAngle = 0.0f;
                o.startAngle2 = 360.0f;
                o.rotationSpeedGraph = curve({0.0f, -15.0f});
                o.rotationSpeedGraph2 = curve({0.0f, 15.0f});
                o.scaleGraph = curve({0.0f, 1.0f, 1.0f, 2.6f});
                o.scaleGraph2 = curve({0.0f, 0.8f, 1.0f, 2.0f});
                o.colorGraph = gradient({{0.0f, 0.55f, 0.5f, 0.46f}, {1.0f, 0.62f, 0.62f, 0.66f}});
                o.alphaGraph = curve({0.0f, 0.0f, 0.08f, 0.5f, 1.0f, 0.5f});
                o.depthSoftening = 0.12f;
                e.layers.push_back(std::move(layer));
            }
            {
                // dust kicked up and pushed out along the ground
                EffectLayer layer{.name = "Ground Dust", .options = emitter(14, 2.4f, 0.0f)};
                auto& o = layer.options;
                flipbook(o, res.smoke);
                o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_BOX;
                o.emitterExtents = Vector3(0.8f, 0.0f, 0.8f);
                o.blendType = ParticleBlendType::BLEND_NORMAL;
                o.depthWrite = false;
                o.radialSpeedGraph = curve({0.0f, 5.0f, 0.3f, 0.9f, 1.0f, 0.0f});
                o.radialSpeedGraph2 = curve({0.0f, 3.0f, 0.3f, 0.6f, 1.0f, 0.0f});
                o.localVelocityGraph = constant(0.0f, 0.15f, 0.0f);
                o.localVelocityGraph2 = constant(0.0f, 0.45f, 0.0f);
                o.startAngle = 0.0f;
                o.startAngle2 = 360.0f;
                o.scaleGraph = curve({0.0f, 0.4f, 1.0f, 1.4f});
                o.scaleGraph2 = curve({0.0f, 0.3f, 1.0f, 1.1f});
                o.colorGraph = constant(0.42f, 0.36f, 0.29f);
                o.alphaGraph = curve({0.0f, 0.0f, 0.05f, 0.7f, 1.0f, 0.6f});
                o.depthSoftening = 0.08f;
                e.layers.push_back(std::move(layer));
            }
            {
                EffectLayer layer{.name = "Sparks", .options = emitter(70, 1.3f, 0.0f)};
                auto& o = layer.options;
                o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
                o.emitterRadius = 0.2f;
                o.colorMap = res.glow;
                o.blendType = ParticleBlendType::BLEND_ADDITIVE;
                o.depthWrite = false;
                o.radialSpeedGraph = curve({0.0f, 5.0f});
                o.radialSpeedGraph2 = curve({0.0f, 11.0f});
                o.localVelocityGraph = constant(0.0f, 1.5f, 0.0f);
                o.localVelocityGraph2 = constant(0.0f, 4.0f, 0.0f);
                o.velocityGraph = gravity(9.8f, 1.3f);
                o.alignToMotion = true;
                o.stretch = 0.05f;
                o.scaleGraph = curve({0.0f, 0.025f, 1.0f, 0.008f});
                o.scaleGraph2 = curve({0.0f, 0.015f, 1.0f, 0.005f});
                o.colorGraph = gradient({{0.0f, 1.0f, 0.9f, 0.6f}, {0.3f, 1.0f, 0.55f, 0.15f}, {1.0f, 0.8f, 0.15f, 0.03f}});
                o.alphaGraph = curve({0.0f, 1.0f, 0.75f, 1.0f, 1.0f, 0.0f});
                o.intensity = 6.0f;
                e.layers.push_back(std::move(layer));
            }
            e.layers.push_back(motes({.name = "Embers", .texture = res.glow, .count = 40, .spawnTime = 1.0f, .radius = 1.0f,
                .rise = {0.3f, 1.2f}, .lifetime = 2.5f, .size = 0.025f,
                .color = gradient({{0.0f, 1.0f, 0.6f, 0.2f}, {1.0f, 0.9f, 0.2f, 0.05f}}), .intensity = 6.0f,
                .delay = 0.3f}));
            e.layers.push_back(debris("Debris", 22, {0.22f, 0.2f, 0.18f}, 0.08f, {3.0f, 7.0f}, 3.5f, 1.6f));
            e.layers.push_back(shockwave({1.0f, 0.7f, 0.45f}, 3.5f, 0.4f, 2.0f));
        }

        {
            EffectDefinition& e = effects["magic"];
            e.maxInstances = 8;
            e.light = EffectLight{.color = Color(0.45f, 0.6f, 1.0f, 1.0f), .intensity = 5.0f, .range = 6.0f,
                .duration = 1.2f, .offset = 0.5f, .flicker = 0.2f};
            e.decals.push_back(decal(res.rune, Color(0.4f, 0.7f, 1.0f, 1.0f), 2.6f, 3.0f, 2.2f, 1.2f, 0.0f, 4.0f));
            e.layers.push_back(flash(res.glow, {0.5f, 0.75f, 1.0f}, 1.0f, 0.3f, 4.0f));
            e.layers.push_back(flash(res.star, {0.8f, 0.9f, 1.0f}, 0.9f, 0.25f, 6.0f));
            {
                EffectLayer layer{.name = "Orbs", .options = emitter(40, 1.5f, 0.0f)};
                auto& o = layer.options;
                // Upstream spawns on a thin shell (inner radius 0.12), so all orbs start at the
                // same distance and burst out evenly; see the DEVIATION in the header.
                o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
                o.emitterRadius = 0.15f;
                o.colorMap = res.glow;
                o.blendType = ParticleBlendType::BLEND_ADDITIVE;
                o.depthWrite = false;
                o.radialSpeedGraph = curve({0.0f, 4.5f, 0.25f, 0.5f, 1.0f, 0.0f});
                o.radialSpeedGraph2 = curve({0.0f, 2.5f, 0.25f, 0.3f, 1.0f, 0.0f});
                o.localVelocityGraph = constant(0.0f, 0.2f, 0.0f);
                o.localVelocityGraph2 = constant(0.0f, 1.0f, 0.0f);
                o.alignToMotion = true;
                o.stretch = 0.04f;
                o.scaleGraph = curve({0.0f, 0.08f, 1.0f, 0.0f});
                o.scaleGraph2 = curve({0.0f, 0.05f, 1.0f, 0.0f});
                o.colorGraph = gradient({{0.0f, 0.6f, 0.95f, 1.0f}, {1.0f, 0.65f, 0.35f, 1.0f}});
                o.intensity = 5.0f;
                e.layers.push_back(std::move(layer));
            }
            e.layers.push_back(motes({.name = "Sparkles", .texture = res.spike, .count = 30, .spawnTime = 0.75f,
                .radius = 0.8f, .rise = {0.3f, 1.2f}, .lifetime = 1.6f, .size = 0.12f,
                .color = constant(0.8f, 0.9f, 1.0f), .intensity = 6.0f}));
            e.layers.push_back(motes({.name = "Motes", .texture = res.glow, .count = 50, .spawnTime = 1.0f, .radius = 0.9f,
                .rise = {0.6f, 1.8f}, .lifetime = 1.6f, .size = 0.04f,
                .color = gradient({{0.0f, 0.5f, 0.85f, 1.0f}, {1.0f, 0.7f, 0.4f, 1.0f}}), .intensity = 6.0f,
                .flat = true}));
            e.layers.push_back(shockwave({0.5f, 0.8f, 1.0f}, 2.4f, 0.6f, 3.0f));
            e.layers.push_back(shockwave({0.7f, 0.45f, 1.0f}, 1.6f, 0.6f, 3.0f, 0.12f));
        }

        return effects;
    }
}

class ImpactEffectsExample final : public ExampleApp
{
public:
    ImpactEffectsExample() : ExampleApp({}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ParticleSystemComponentSystem>();
        options.registerComponentSystem<CollisionComponentSystem>();
        options.registerComponentSystem<RigidBodyComponentSystem>();
        // The shots are raycast against the room through the physics world.
        options.physicsWorld = createJoltPhysicsWorld();
    }

    bool create() override
    {
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        // The empty room rendered into a cubemap from its centre, prefiltered into an
        // environment atlas.
        _roomEnvAtlas = std::make_unique<Asset>("room-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/area-lights-room-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _room = std::make_unique<Asset>("room", AssetType::CONTAINER, assetPath("models/area-lights-room.glb"));
        _statue = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));

        // The textures of the effects. The flipbooks and decals are generated, see
        // textures/vfx/vfx-textures.txt.
        const auto texture = [this](const char* name, const char* path) -> Texture* {
            _textureAssets.push_back(std::make_unique<Asset>(name, AssetType::TEXTURE, assetPath(path)));
            Texture* result = _textureAssets.back()->resourceAs<Texture>();
            if (!result) {
                spdlog::error("impact-effects: cannot load {}", path);
            }
            return result;
        };
        EffectResources res;
        res.smoke = texture("smoke-flipbook", "textures/vfx/smoke-flipbook.webp");
        res.explosion = texture("explosion-flipbook", "textures/vfx/explosion-flipbook.webp");
        res.scorch = texture("scorch-decal", "textures/vfx/scorch-decal.webp");
        res.bulletHole = texture("bullet-hole-decal", "textures/vfx/bullet-hole-decal.webp");
        res.rune = texture("rune-decal", "textures/vfx/rune-decal.webp");
        res.glow = texture("glow", "textures/lens-flare/glow_soft.png");
        res.star = texture("star", "textures/lens-flare/spike_6.png");
        res.spike = texture("spike", "textures/lens-flare/spike_4.png");
        res.streak = texture("streak", "textures/lens-flare/anamorphic_streak.png");
        res.halo = texture("halo", "textures/lens-flare/halo.png");
        for (const Texture* t : {res.smoke, res.explosion, res.scorch, res.bulletHole, res.rune, res.glow, res.star,
                 res.spike, res.streak, res.halo}) {
            if (!t) {
                return false;
            }
        }

        // A plain white texture for the mesh particles, which need a colour map.
        {
            TextureOptions options;
            options.name = "White";
            options.width = 4;
            options.height = 4;
            options.format = PixelFormat::PIXELFORMAT_RGBA8;
            options.mipmaps = false;
            options.minFilter = FilterMode::FILTER_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;
            _white = std::make_shared<Texture>(device().get(), options);
            const std::vector<uint8_t> pixels(4 * 4 * 4, 255);
            _white->setLevelData(0, pixels.data(), pixels.size());
            _white->upload();
            res.white = _white.get();
        }
        // A small box, for the stone chips and the debris, and a unit plane for the decals.
        {
            RoundedBoxGeometryOptions chip;
            chip.halfExtents[0] = 0.5f;
            chip.halfExtents[1] = 0.35f;
            chip.halfExtents[2] = 0.45f;
            chip.radius = 0.0f;
            res.chip = createMeshFromGeometry(device(), createRoundedBoxGeometry(chip));
            res.quad = createMeshFromGeometry(device(), createPlaneGeometry());
        }

        _rigidBodies = dynamic_cast<RigidBodyComponentSystem*>(engine()->systems()->getById("rigidbody"));
        if (!_rigidBodies) {
            spdlog::error("impact-effects: no rigid body system");
            return false;
        }

        // ----- The room -----

        // The layers: World (the room, everything in it and the effects) and Skybox, rendered by
        // both cameras, and the floor's own, rendered by the main camera alone.
        scene()->layers()->insert(std::make_shared<Layer>("Floor", LAYERID_FLOOR), 1);
        const std::vector<int> lightLayers = {LAYERID_WORLD, LAYERID_FLOOR};

        // The sky shows through the windows and the skylight of the room.
        if (Texture* helipad = _helipad->resourceAs<Texture>()) {
            scene()->setEnvAtlas(helipad);
        }
        scene()->setSkyboxMip(0);
        scene()->setExposure(0.6f);
        // The mesh particles, such as the debris, are lit by the ambient light and the
        // directional lights only.
        scene()->setAmbientLight(0.25f, 0.25f, 0.28f);

        auto* roomContainer = _room->resourceAs<ContainerResource>();
        auto* statueContainer = _statue->resourceAs<ContainerResource>();
        if (!roomContainer || !statueContainer) {
            spdlog::error("impact-effects: a model did not load");
            return false;
        }

        // The metal room, with the stone statue on the pedestal in the middle.
        Entity* room = roomContainer->instantiateRenderEntity();
        room->setEngine(engine());
        root()->addChild(room);

        Entity* statue = statueContainer->instantiateRenderEntity();
        statue->setEngine(engine());
        statue->setLocalScale(0.15f, 0.15f, 0.15f);
        statue->setLocalPosition(0.0f, 0.36f, 0.0f);
        statue->setLocalEulerAngles(0.0f, 30.0f, 0.0f);
        root()->addChild(statue);

        // Every glTF material reflects the room, box projected onto it.
        std::vector<StandardMaterial*> gltfMaterials;
        for (Entity* model : {room, statue}) {
            for (auto* render : model->findComponents<RenderComponent>()) {
                render->setCastShadows(true);
                render->setReceiveShadows(true);
                for (auto* meshInstance : render->meshInstances()) {
                    auto* material = dynamic_cast<StandardMaterial*>(meshInstance ? meshInstance->material() : nullptr);
                    if (material && std::find(gltfMaterials.begin(), gltfMaterials.end(), material) == gltfMaterials.end()) {
                        gltfMaterials.push_back(material);
                    }
                }
            }
        }
        for (auto* material : gltfMaterials) {
            addReflections(material);
            if (material->name() == "Floor") {
                _floorMaterial = material;
            }
        }

        // Static mesh colliders, which the shots hit; the tag names the surface, which picks the
        // bullet effect. Added once the models are placed: a collider takes its entity's scale.
        addMeshColliders(room, "metal");
        addMeshColliders(statue, "stone");

        // A chrome sphere on the left pedestal, and a gold torus standing on the right one.
        _chrome = makeMetal(Color(0.95f, 0.95f, 0.95f, 1.0f), 0.95f);
        Entity* sphere = createPrimitive("sphere", _chrome.get(), Vector3(-4.0f, 1.15f, 0.8f), Vector3(1.2f, 1.2f, 1.2f),
            {LAYERID_WORLD});
        sphere->setName("Sphere");
        // A primitive collision shape does not take the scale of its entity.
        addCollider(sphere, "sphere", "metal", 0.6f);

        _gold = makeMetal(Color(1.0f, 0.77f, 0.34f, 1.0f), 0.75f);
        Entity* torus = createPrimitive("torus", _gold.get(), Vector3(4.0f, 1.29f, 0.8f), Vector3(1.5f, 1.5f, 1.5f),
            {LAYERID_WORLD});
        torus->setName("Torus");
        torus->setLocalEulerAngles(90.0f, -20.0f, 0.0f);
        // A mesh collider made of the torus's own mesh, so the decals land on its surface.
        addCollider(torus, "mesh", "metal");

        // The glowing doors in the back wall, each with a spot light shining into the room.
        const struct
        {
            float x;
            Color color;
        } doors[] = {
            {-4.5f, Color(1.0f, 0.42f, 0.12f, 1.0f)},
            {0.0f, Color(0.25f, 0.8f, 1.0f, 1.0f)},
            {4.5f, Color(1.0f, 0.8f, 0.35f, 1.0f)},
        };
        for (const auto& door : doors) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
            material->setEmissive(door.color);
            material->setEmissiveIntensity(4.0f);
            material->setUseLighting(false);
            _doorMaterials.push_back(material);
            createPrimitive("box", material.get(), Vector3(door.x, 1.7f, -9.45f), Vector3(1.7f, 3.3f, 0.02f),
                {LAYERID_WORLD});

            // A spot light shines down its -Y axis, so it is turned to face the room.
            auto* light = new Entity();
            light->setName("Door Light");
            light->setEngine(engine());
            if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
                lc->setType(LightType::LIGHTTYPE_SPOT);
                lc->setLayers(lightLayers);
                lc->setColor(door.color);
                lc->setIntensity(1.5f);
                lc->setRange(20.0f);
                lc->setInnerConeAngle(50.0f);
                lc->setOuterConeAngle(80.0f);
            }
            light->setLocalPosition(door.x, 1.7f, -9.2f);
            light->setLocalEulerAngles(-90.0f, 0.0f, 0.0f);
            root()->addChild(light);
        }

        // The light of the room, shining on the statue from the front.
        auto* roomLight = new Entity();
        roomLight->setName("Light");
        roomLight->setEngine(engine());
        if (auto* lc = static_cast<LightComponent*>(roomLight->addComponent<LightComponent>())) {
            lc->setType(LightType::LIGHTTYPE_OMNI);
            lc->setLayers(lightLayers);
            lc->setColor(Color(1.0f, 0.95f, 0.9f, 1.0f));
            lc->setIntensity(2.0f);
            lc->setRange(20.0f);
            lc->setCastShadows(true);
            lc->setShadowBias(0.2f);
            lc->setShadowNormalBias(0.05f);
        }
        roomLight->setLocalPosition(1.0f, 5.5f, 4.0f);
        root()->addChild(roomLight);

        // The floor, on its own layer.
        if (auto* floorEntity = dynamic_cast<Entity*>(room->findByName("RoomFloor"))) {
            for (auto* render : floorEntity->findComponents<RenderComponent>()) {
                render->setLayers({LAYERID_FLOOR});
            }
        }
        if (!_floorMaterial) {
            spdlog::error("impact-effects: the room has no 'Floor' material");
            return false;
        }
        // Mostly metallic, so the reflection shows over its diffuse lighting; a glTF material:
        // gloss = 1 - roughness.
        _floorMaterial->setMetalness(0.7f);
        _floorMaterial->setGloss(0.9f);

        // ----- The cameras -----

        // The main camera, low in the room, where the floor reflects the most.
        _camera = createCamera(Vector3(1.5f, 1.8f, 6.8f));
        _camera->setName("Camera");
        _cameraComponent = _camera->findComponent<CameraComponent>();
        _cameraComponent->setLayers({LAYERID_WORLD, LAYERID_FLOOR, LAYERID_SKYBOX, LAYERID_UI});
        _cameraComponent->camera()->setFov(60.0f);
        _cameraComponent->camera()->setNearClip(0.1f);
        _cameraComponent->camera()->setFarClip(100.0f);
        _cameraComponent->setToneMapping(TONEMAP_ACES);

        if (auto* controls = addOrbitControls(_camera, Vector3(0.0f, 1.2f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-45.0f, 5.0f));
            controls->setZoomRange(Vector2(3.5f, 9.0f));
            controls->storeResetState();
        }

        // Bloom makes the doors, the fire and the sparks glow, and their reflections. The soft
        // particles fade where they meet the room, through the scene depth map.
        auto rendering = _cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.03f;
        rendering.bloomBlurLevel = 6;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.35f;
        _cameraComponent->setRendering(rendering);
        _cameraComponent->requestSceneDepthMap(true);

        // The reflection camera renders the World and Skybox layers, with the effects, into the
        // floor's reflection texture, first (its priority is lower).
        _reflectionCamera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _reflectionCamera->setName("ReflectionCamera");
        if (auto* reflectionComponent = _reflectionCamera->findComponent<CameraComponent>()) {
            reflectionComponent->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
            reflectionComponent->setPriority(-1);
            reflectionComponent->setToneMapping(TONEMAP_ACES);
        }
        _reflectionCamera->addComponent<ScriptComponent>();
        _planarRenderer = _reflectionCamera->script()->create<PlanarRenderer>();
        _planarRenderer->setSceneCameraEntity(_camera);
        _planarRenderer->setMipmaps(true);
        _planarRenderer->setHdr(true);
        _planarRenderer->setScale(0.5f);
        _planarRenderer->setPlane(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));

        // ----- The effects -----

        // The lights of the effects light the floor as well.
        _pool = std::make_unique<ParticleEffectPool>(engine(), root(), createImpactEffects(res), lightLayers);

        spdlog::info("impact-effects: click to fire; 1-4 effect, F auto fire, - = rate, X soft particles, I stats");
        logSettings();
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_1: _effect = Effect::Mixed; break;
        case SDLK_2: _effect = Effect::Bullet; break;
        case SDLK_3: _effect = Effect::Explosion; break;
        case SDLK_4: _effect = Effect::Magic; break;
        case SDLK_F: _autoFire = !_autoFire; break;
        case SDLK_MINUS: _rate = std::max(0.5f, _rate <= 1.0f ? _rate - 0.5f : _rate - 1.0f); break;
        case SDLK_EQUALS: _rate = std::min(30.0f, _rate < 1.0f ? _rate + 0.5f : _rate + 1.0f); break;
        case SDLK_X:
            _softParticles = !_softParticles;
            _cameraComponent->requestSceneDepthMap(_softParticles);
            _pool->setSoftParticles(_softParticles);
            break;
        case SDLK_I:
            spdlog::info("impact-effects: {} playing, {} instances", _pool->activeCount(), _pool->instanceCount());
            return true;
        default: return false;
        }
        logSettings();
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        _frameDt = dt;

        // Fire on a click, but not at the end of a drag that orbits the camera.
        if (const Mouse* mouse = engine()->mouse()) {
            if (mouse->wasPressed(MouseButton::Left)) {
                _pointerDown = Vector2(mouse->x(), mouse->y());
            }
            if (mouse->wasReleased(MouseButton::Left)) {
                const float dx = mouse->x() - _pointerDown.x;
                const float dy = mouse->y() - _pointerDown.y;
                if (dx * dx + dy * dy <= 25.0f) {
                    fire(_cameraComponent->screenToWorld(mouse->x(), mouse->y(),
                        _cameraComponent->camera()->farClip()));
                }
            }
        }

        // Auto fire aims at random points in the back half of the room, and shoots on past them.
        if (_autoFire) {
            _autoFireTime += dt;
            const float interval = 1.0f / _rate;
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            while (_autoFireTime >= interval) {
                _autoFireTime -= interval;
                const Vector3 target(unit(_rng) * 14.0f - 7.0f, unit(_rng) * 3.5f, unit(_rng) * 11.0f - 9.0f);
                const Vector3 origin = _camera->position();
                fire(origin + (target - origin) * 10.0f);
            }
        }

        _pool->update(dt, _rng);

        // The frame the engine renders as it starts sees no preRender: its reflection is set here.
        if (dt == 0.0f) {
            updateReflection();
        }
    }

    void preRender() override
    {
        // The camera controls have set the camera's transform for this frame by now, so the
        // shake does not add up.
        if (_shake > 0.0f) {
            const float angle = _shake * _shake * 2.0f;
            _camera->rotateLocal(std::sin(_time * 47.0f) * angle, std::sin(_time * 39.0f + 1.0f) * angle,
                std::sin(_time * 31.0f + 2.0f) * angle * 0.5f);
            _shake = std::max(0.0f, _shake - _frameDt * 1.5f);
        }

        // The reflection camera, after the main camera moved and shook this frame.
        updateReflection();
    }

    void destroy() override
    {
        if (_floorMaterial) {
            _floorMaterial->setPlanarReflectionMap(nullptr);
        }
        _pool.reset();
    }

private:
    enum class Effect { Mixed, Bullet, Explosion, Magic };

    // Mirrors the main camera by the floor; the floor reflects the texture through its
    // view-projection.
    void updateReflection()
    {
        _floorMaterial->setPlanarReflectionMap(_planarRenderer->frameUpdate());
        _floorMaterial->setPlanarReflectionMatrix(_planarRenderer->viewProjectionMatrix());
    }

    // A material reflects the room, box projected onto its inside (the cubemap was rendered
    // from the centre of the room), and is lit by it.
    void addReflections(StandardMaterial* material) const
    {
        if (Texture* atlas = _roomEnvAtlas->resourceAs<Texture>()) {
            material->setEnvAtlas(atlas);
        }
        material->setCubeMapProjection(CUBEPROJ_BOX);
        material->setCubeMapProjectionBox(BoundingBox(Vector3(0.0f, 3.25f, 0.0f), Vector3(8.0f, 3.25f, 9.0f)));
    }

    std::shared_ptr<StandardMaterial> makeMetal(const Color& diffuse, const float gloss) const
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(diffuse);
        material->setUseMetalness(true);
        material->setMetalness(1.0f);
        material->setGloss(gloss);
        addReflections(material.get());
        return material;
    }

    // A static rigid body with a collision shape of `type`, tagged with its surface.
    void addCollider(Entity* entity, const std::string& type, const std::string& surface, const float radius = 0.5f)
    {
        entity->setEngine(engine());
        if (auto* body = static_cast<RigidBodyComponent*>(entity->addComponent<RigidBodyComponent>())) {
            body->setType(RigidBodyType::Static);
        }
        if (auto* collision = static_cast<CollisionComponent*>(entity->addComponent<CollisionComponent>())) {
            collision->setType(type);
            collision->setRadius(radius);
        }
        entity->tags().add(surface);
    }

    // A static mesh collider on every mesh of a model, made of the meshes of its render component.
    void addMeshColliders(Entity* model, const std::string& surface)
    {
        for (auto* render : model->findComponents<RenderComponent>()) {
            if (auto* entity = render->entity()) {
                addCollider(entity, "mesh", surface);
            }
        }
    }

    // Plays the selected effect where a shot from the camera hits the room, facing out of the
    // surface; `end` is the end of the shot, which starts at the camera.
    void fire(const Vector3& end)
    {
        const auto hit = _rigidBodies->raycastFirst(_camera->position(), end);
        if (!hit || !hit->entity) {
            return;
        }

        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        Effect effect = _effect;
        if (effect == Effect::Mixed) {
            effect = unit(_rng) < 0.7f ? Effect::Bullet : (unit(_rng) < 0.5f ? Effect::Explosion : Effect::Magic);
        }

        // A bullet plays the effect of the surface it hits, which the collider's tag names.
        std::string name;
        switch (effect) {
        case Effect::Bullet:
            static const std::string stone = "stone";
            name = hit->entity->tags().has(stone) ? "bullet-stone" : "bullet-metal";
            break;
        case Effect::Explosion: name = "explosion"; break;
        default: name = "magic"; break;
        }
        if (const float strength = _pool->spawn(name, hit->point, hit->normal.normalized(), _rng); strength > 0.0f) {
            // An explosion shakes the camera, less when it is further away.
            const float falloff = std::max(0.0f, 1.0f - _camera->position().distance(hit->point) / 20.0f);
            _shake = std::min(1.0f, _shake + strength * falloff);
        }
    }

    void logSettings() const
    {
        static constexpr const char* names[] = {"mixed", "bullets", "explosions", "magic"};
        spdlog::info("impact-effects: effect {}, auto fire {}, {:.1f} shots/s, soft particles {}",
            names[static_cast<int>(_effect)], _autoFire ? "on" : "off", _rate, _softParticles ? "on" : "off");
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _roomEnvAtlas;
    std::unique_ptr<Asset> _room;
    std::unique_ptr<Asset> _statue;
    std::vector<std::unique_ptr<Asset>> _textureAssets;
    std::shared_ptr<Texture> _white;

    StandardMaterial* _floorMaterial = nullptr;
    std::shared_ptr<StandardMaterial> _chrome;
    std::shared_ptr<StandardMaterial> _gold;
    std::vector<std::shared_ptr<StandardMaterial>> _doorMaterials;
    Entity* _camera = nullptr;
    CameraComponent* _cameraComponent = nullptr;
    Entity* _reflectionCamera = nullptr;
    PlanarRenderer* _planarRenderer = nullptr;
    RigidBodyComponentSystem* _rigidBodies = nullptr;
    std::unique_ptr<ParticleEffectPool> _pool;

    std::mt19937 _rng{20261109};
    Effect _effect = Effect::Mixed;
    bool _autoFire = true;
    float _rate = 10.0f;
    bool _softParticles = true;

    Vector2 _pointerDown = Vector2(0.0f, 0.0f);
    float _autoFireTime = 0.0f;
    float _shake = 0.0f;
    float _time = 0.0f;
    float _frameDt = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ImpactEffectsExample)
