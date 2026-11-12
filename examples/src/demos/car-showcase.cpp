// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.11.2026
//
// Port of upstream demos/car-showcase.
//
// A car configurator in a showroom. The showroom was modelled and lit in Blender: the lighting of
// its walls, ceiling, floor and platform is baked into their emissive textures, and the environment
// atlas is the empty room rendered from its centre, without the ceiling strips, prefiltered into the
// reflections and the ambient lighting. The environment lights the car at twice the studio
// brightness (1.3), the reflections box-projected onto the room (centre (0, 2.5, 0), half extents
// (9, 2.5, 11)) so the reflections of its lights are where the lights are; the room's meshes take
// only the lights that affect lightmapped meshes. The floor and the platform top, mostly metallic
// (0.7 / 0.85, diffuse (0.5, 0.5, 0.52)), sit on a Floor layer the reflection camera does not
// render, and reflect the planar reflection of the World layer mirrored by the platform's plane
// (height 0.12), rendered at full resolution into a mipmapped HDR texture. Four rectangular area
// lights in place of the ceiling strips (intensity 30, range 12, inverse-squared) light the car
// where its reflections show them. The car (the Khronos Car Concept, CC BY 4.0: see
// assets/models/car-concept.txt) stands on the platform; its materials with an occlusion map
// occlude the direct light too, the brakes and discs take only the lightmapped mask, the windows
// are tinted blended glass (opacity 0.35, keeping their specular), the blacks of the cabin are
// lifted to a few percent, the car lights are emissive many times the glTF's (headlights x5,
// brake and signal lights x12) over the look they have when off, and the indicators blink as
// hazard lights. Two spot lights in the headlights (intensity 10, range 16, cones 12 / 28, set
// back 0.3 into the body, scattering in the haze) light the room and the car. The butterfly doors
// and the clamshells swing open on their hinges over 1.3 s. The paint sweeps over the car from its
// front to its back with a warm glowing front, the trim of the interior with a cool one: the
// materials exist in two banks, and copies of the mesh instances keep the old materials ahead of
// the front. Three looks of the paint (candy, pearl, graphite) come from the glTF's material
// variants, the other paints tint the candy or re-film the pearl; the wheels, calipers and the trim
// recolour their materials. The camera orbits the car (pitch -24 to -8 degrees, 2 to 8 away), moves
// smoothly to the paint and wheels views, flies into the cabin through a door (the doors open for
// it) along a cubic spline, orbits a point just in front of the seat there on a circle through it,
// and after 3 s without input slowly orbits the car. The camera frame renders with neutral tone
// mapping, bloom (blur level 7), a vignette, and volumetric fog as the haze the headlights scatter
// in (density 0.04, no height falloff, no ambient, 16 local steps, anisotropy 0.5), with TAA and
// sharpening or 4x MSAA.
//
// Keys stand in for upstream's configurator panel:
//   1 / 2 / 3 / 4   Paint / Wheels / Calipers / Interior (the camera moves to the view)
//   [ / ]           the previous / next option of the group
//   L               the car lights                     S       the studio lights
//   D               the doors                          H       the clamshells
//   T               TAA (else 4x MSAA)                 B       bloom
//   V               the headlight beams (the volumetric fog)
//   Mouse           orbit (the camera controls); any input wakes the idle orbit
//   VISUTWIN_CAR_KEYS=<keys> presses one of those keys a second from the second second on, for
//   a capture (which ignores the keyboard): `VISUTWIN_CAR_KEYS=']D4'` changes the paint, opens the
//   doors and flies into the cabin.
//
// DEVIATIONS: the sweep's front is a chunk override of the `forward-fragment-user` hook, one
// source per bank and side with the glow baked in, and the front's position rides in the scene's
// user shader data (lane x the paint, y the trim) where upstream sets per-material uniforms; the
// HTML overlay is a few text elements and the keys above; the first camera reset glides from the
// start pose rather than snapping (the controls take over at the first frame).
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "exampleApp.h"
#include "uiElements.h"
#include "core/shape/boundingBox.h"
#include "extras/script/cameraControls.h"
#include "extras/script/planarRenderer.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/handlers/resourceLoader.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kFloorLayerId = 30;

    float smoothstep01(const float t)
    {
        const float x = std::clamp(t, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }

    float signOf(const float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

    Vector3 forwardOf(GraphNode* node)
    {
        return Vector3(node->worldTransform().getColumn(2)).normalized() * -1.0f;
    }

    Vector3 upOf(GraphNode* node)
    {
        return Vector3(node->worldTransform().getColumn(1)).normalized();
    }

    bool boxContains(const BoundingBox& box, const Vector3& p)
    {
        const Vector3 d = (p - box.center()).abs();
        return d.getX() <= box.halfExtents().getX() && d.getY() <= box.halfExtents().getY() &&
            d.getZ() <= box.halfExtents().getZ();
    }

    Color hex(const std::string& value)
    {
        Color color;
        color.fromString(value);
        return color;
    }

    struct CameraPose
    {
        Vector3 position;
        Vector3 focus;
        float pace = 1.0f;   // how many times slower the camera moves to this pose than the average
    };

    // A smooth flight of the camera through a few poses. The positions, as well as the points the
    // camera looks at, follow a cubic spline through the poses. It is timed by the distances between
    // the positions, so the camera moves at an even speed, easing in and out over the whole flight.
    class CameraPath
    {
    public:
        float time = 0.0f;
        float duration = 0.0f;
        float length = 0.0f;
        std::vector<float> knots;   // the progress of the flight at each pose, 0 to 1

        CameraPath(const std::vector<CameraPose>& poses, const std::function<float(float)>& durationOf)
        {
            std::vector<float> lengths;
            for (size_t i = 0; i < poses.size(); ++i) {
                _positions.push_back(poses[i].position);
                _focuses.push_back(poses[i].focus);
                lengths.push_back(i > 0 ? poses[i].position.distance(poses[i - 1].position) * poses[i].pace : 0.0f);
                length += lengths.back();
            }
            duration = durationOf(length);
            const float total = std::max(length, 1e-6f);
            float sum = 0.0f;
            for (const float l : lengths) {
                sum += l;
                knots.push_back(sum / total);
            }
            _positionTangents = tangents(_positions);
            _focusTangents = tangents(_focuses);
        }

        bool done() const { return time >= duration; }

        float progressAt(const float t) const
        {
            return smoothstep01(duration > 0.0f ? t / duration : 1.0f);
        }

        void evaluate(Vector3& position, Vector3& focus) const
        {
            const float progress = progressAt(time);
            size_t i = 0;
            while (i + 2 < knots.size() && progress > knots[i + 1]) {
                ++i;
            }
            const float span = knots[i + 1] - knots[i];
            const float u = span > 0.0f ? (progress - knots[i]) / span : 1.0f;
            position = hermite(_positions, _positionTangents, i, u, span);
            focus = hermite(_focuses, _focusTangents, i, u, span);
        }

    private:
        // The tangents of a spline through points, by the progress, so its speed does not change
        // at the points.
        std::vector<Vector3> tangents(const std::vector<Vector3>& points) const
        {
            std::vector<Vector3> result;
            for (size_t i = 0; i < points.size(); ++i) {
                const size_t prev = i > 0 ? i - 1 : 0;
                const size_t next = std::min(i + 1, points.size() - 1);
                const float span = knots[next] - knots[prev];
                result.push_back((points[next] - points[prev]) * (span > 0.0f ? 1.0f / span : 0.0f));
            }
            return result;
        }

        static Vector3 hermite(const std::vector<Vector3>& points, const std::vector<Vector3>& tangents,
            const size_t i, const float u, const float span)
        {
            const float u2 = u * u;
            const float u3 = u2 * u;
            return points[i] * (2.0f * u3 - 3.0f * u2 + 1.0f) + tangents[i] * ((u3 - 2.0f * u2 + u) * span) +
                points[i + 1] * (3.0f * u2 - 2.0f * u3) + tangents[i + 1] * ((u3 - u2) * span);
        }

        std::vector<Vector3> _positions;
        std::vector<Vector3> _focuses;
        std::vector<Vector3> _positionTangents;
        std::vector<Vector3> _focusTangents;
    };

    // The lit-colour hook of the standard material, extended by the sweep: a front moving along the
    // world Z axis, ragged by a noise. The new materials keep the part of the car the front has
    // passed, the old materials the part it has not reached, and the front glows. The side and the
    // glow are baked into the source; the front's position is the scene's user data (lane `%LANE%`).
    const char* const kSweepChunk = R"slang(
float sweepHash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1) * 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float sweepNoise(float3 x)
{
    const float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(
        lerp(lerp(sweepHash(i), sweepHash(i + float3(1.0, 0.0, 0.0)), f.x),
             lerp(sweepHash(i + float3(0.0, 1.0, 0.0)), sweepHash(i + float3(1.0, 1.0, 0.0)), f.x), f.y),
        lerp(lerp(sweepHash(i + float3(0.0, 0.0, 1.0)), sweepHash(i + float3(1.0, 0.0, 1.0)), f.x),
             lerp(sweepHash(i + float3(0.0, 1.0, 1.0)), sweepHash(i + float3(1.0, 1.0, 1.0)), f.x), f.y),
        f.z);
}

float3 forwardUserLitColor(float3 color, float3 albedo, float3 worldNormal, float3 worldPos)
{
    const float front = worldPos.z + (sweepNoise(worldPos * 9.0) - 0.5) * 0.3 - lighting.userData[0].%LANE%;
    if (front * (%SIDE%) < 0.0) {
        discard;
    }
    return color + float3(%GLOW%) * (1.0 - smoothstep(0.0, 0.05, abs(front)));
}

float forwardUserFogFactor(float fogFactor, float3 worldPos)
{
    return fogFactor;
}

float3 forwardUserFragmentEnd(float3 color, float3 worldPos, float3 worldNormal)
{
    return color;
}
)slang";

    std::string replaceAll(std::string text, const std::string_view from, const std::string& to)
    {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
        }
        return text;
    }

    // A sweep position which keeps all of the car with the current materials.
    constexpr float kSweepIdle = -1000.0f;

    using MaterialPtr = std::shared_ptr<StandardMaterial>;

    // A material of the container by its glTF name; "Untitled" is the file's unnamed material, which the parser names "glTF-material".
    MaterialPtr findMaterial(const GlbContainerResource* container, const std::string& name)
    {
        const std::string wanted = name == "Untitled" ? "glTF-material" : name;
        for (const auto& payload : container->meshPayloads()) {
            if (payload.material && payload.material->name() == wanted) {
                return std::dynamic_pointer_cast<StandardMaterial>(payload.material);
            }
            for (const auto& [variant, material] : payload.variantMaterials) {
                if (material && material->name() == wanted) {
                    return std::dynamic_pointer_cast<StandardMaterial>(material);
                }
            }
        }
        spdlog::warn("car-showcase: no material '{}'", name);
        return nullptr;
    }

    // Changes materials of the car with a glowing front sweeping along it. The materials exist
    // twice, in two banks. A change sets up the other bank with the new materials and gives it to
    // the car, while copies of the mesh instances keep rendering the old materials, which the
    // front then reveals the new ones under. The materials are in slots, each with a variant for
    // each look, named `<slot> <look>` in the glTF; a change of the look swaps the variants of all
    // slots.
    class MaterialSweep
    {
    public:
        struct Options
        {
            const GlbContainerResource* container = nullptr;
            Entity* entity = nullptr;
            std::shared_ptr<Layer> layer;
            std::vector<std::string> slots;
            std::vector<std::string> looks;
            float start = 0.0f;   // the world Z position the front starts at
            float end = 0.0f;     // and ends at
            Color glow;
            float intensity = 1.0f;
            std::string lane;     // the lane of the scene's user data holding the front
            std::function<void(float)> setPosition;   // writes that lane
        };

        float duration = 1.6f;

        explicit MaterialSweep(Options options) : _o(std::move(options))
        {
            _glow = std::format("{}, {}, {}", _o.glow.r * _o.intensity, _o.glow.g * _o.intensity,
                _o.glow.b * _o.intensity);
            // the first bank holds the materials of the car, the second their clones
            for (const auto& slot : _o.slots) {
                for (const auto& look : _o.looks) {
                    MaterialPtr material = findMaterial(_o.container, slot + " " + look);
                    if (!material) {
                        continue;
                    }
                    _banks[0][slot][look] = material;
                    _banks[1][slot][look] = std::dynamic_pointer_cast<StandardMaterial>(material->clone());
                }
            }
            setSide(0, 1.0f);
            setSide(1, 1.0f);
            _o.setPosition(kSweepIdle);

            // the mesh instances using the materials, the car starts with the first look
            for (auto* render : _o.entity->findComponents<RenderComponent>()) {
                for (auto* meshInstance : render->meshInstances()) {
                    const std::string& name = meshInstance->material() ? meshInstance->material()->name() : "";
                    for (const auto& slot : _o.slots) {
                        if (name.starts_with(slot)) {
                            _instances.push_back({meshInstance, slot});
                            break;
                        }
                    }
                }
            }
        }

        ~MaterialSweep() { finish(); }

        /// Changes the materials to `look`, `configure` setting the new materials up by their
        /// slots before the car shows them; `animate` sweeps them over the old ones.
        void apply(const std::string& look, const std::function<void(std::map<std::string, MaterialPtr>&)>& configure,
            const bool animate)
        {
            finish();
            auto& bank = _banks[1 - _bank];
            std::map<std::string, MaterialPtr> materials;
            for (const auto& slot : _o.slots) {
                materials[slot] = bank[slot][look];
            }
            configure(materials);

            if (animate) {
                // the copies keep the old materials, which stay ahead of the front
                std::vector<MeshInstance*> raw;
                for (const auto& [meshInstance, slot] : _instances) {
                    auto copy = std::make_unique<MeshInstance>(meshInstance->meshShared(),
                        meshInstance->materialShared(), meshInstance->node());
                    copy->setCastShadows(false);
                    copy->setMask(meshInstance->mask());
                    raw.push_back(copy.get());
                    _copies.push_back(std::move(copy));
                }
                _o.layer->addMeshInstances(raw);
                _time = 0.0f;
                setSide(_bank, -1.0f);
                setSide(1 - _bank, 1.0f);
                _o.setPosition(_o.start);
            }

            _bank = 1 - _bank;
            for (const auto& [meshInstance, slot] : _instances) {
                if (materials[slot]) {
                    meshInstance->setMaterial(materials[slot]);
                }
            }
        }

        void update(const float dt)
        {
            if (_copies.empty()) {
                return;
            }
            _time += dt;
            const float t = std::min(_time / duration, 1.0f);
            _o.setPosition(_o.start + (_o.end - _o.start) * smoothstep01(t));
            if (t >= 1.0f) {
                finish();
            }
        }

    private:
        void finish()
        {
            if (_copies.empty()) {
                return;
            }
            std::vector<MeshInstance*> raw;
            for (const auto& copy : _copies) {
                raw.push_back(copy.get());
            }
            _o.layer->removeMeshInstances(raw);
            _copies.clear();
            setSide(_bank, 1.0f);
            _o.setPosition(kSweepIdle);
        }

        // 1 keeps the part the front has passed, -1 the part it has not reached.
        void setSide(const int bank, const float side)
        {
            const std::string source = replaceAll(replaceAll(replaceAll(kSweepChunk, "%LANE%", _o.lane),
                "%SIDE%", std::format("{:.1f}", side)), "%GLOW%", _glow);
            for (auto& [slot, looks] : _banks[bank]) {
                for (auto& [look, material] : looks) {
                    material->setShaderChunk("forward-fragment-user", source);
                }
            }
        }

        struct Instance
        {
            MeshInstance* meshInstance;
            std::string slot;
        };

        Options _o;
        std::string _glow;
        std::array<std::map<std::string, std::map<std::string, MaterialPtr>>, 2> _banks;
        int _bank = 0;
        std::vector<Instance> _instances;
        std::vector<std::unique_ptr<MeshInstance>> _copies;
        float _time = 0.0f;
    };

    struct Paint
    {
        const char* name;
        const char* look;
        const char* trim;
        const char* tint = nullptr;   // a tint of the candy paint
        // a base and film of the pearl paint
        const char* filmBase = nullptr;
        float filmThickness[2] = {0.0f, 0.0f};
        float filmIridescence = 0.0f;
    };

    // The paints. Carmine Candy, Pearly Swirly and Torched Graphite are the material variants of the
    // car. Aurora Swirly gives the pearl paint a lavender base and a thinner film, which turns its
    // swirls blue and violet. The others tint the metallic candy paint. Each paint comes with a trim
    // of the interior, the variants with their own, which the interior options then change.
    constexpr std::array<Paint, 9> kPaints = {{
        {"Carmine Candy", "Carmine", "Carmine Red"},
        {"Pearly Swirly", "Pearl", "Ivory"},
        {"Aurora Swirly", "Pearl", "Ocean Blue", nullptr, "#8e9ccc", {560.0f, 700.0f}, 1.0f},
        {"Torched Graphite", "Graphite", "Ocean Blue"},
        {"Midnight Blue", "Carmine", "Ivory", "#102a66"},
        {"Racing Green", "Carmine", "Cognac", "#0f4a30"},
        {"Sunset Orange", "Carmine", "Onyx Black", "#d9530f"},
        {"Solar Yellow", "Carmine", "Onyx Black", "#e6ad12"},
        {"Liquid Silver", "Carmine", "Carmine Red", "#b4b9bf"},
    }};
    constexpr int kDefaultPaint = 0;

    struct Finish
    {
        const char* name;
        const char* color;
        float metalness;
        float roughness;
    };
    constexpr std::array<Finish, 4> kWheels = {{
        {"Gloss Black", "#111214", 0.0f, 0.4f},
        {"Satin Silver", "#c3c7cc", 1.0f, 0.3f},
        {"Bronze", "#9a7443", 1.0f, 0.35f},
        {"Gunmetal", "#4a4e54", 1.0f, 0.25f},
    }};

    struct Named
    {
        const char* name;
        const char* color;
    };
    constexpr std::array<Named, 4> kCalipers = {{
        {"Racing Red", "#d40000"}, {"Signal Yellow", "#f2b705"}, {"Electric Blue", "#1d5fd6"}, {"Carbon Black", "#1a1b1d"}}};
    // The trims of the interior colour the leather of the seats, the steering wheel and the doors.
    constexpr std::array<Named, 5> kTrims = {{
        {"Carmine Red", "#801000"}, {"Onyx Black", "#1d1f22"}, {"Cognac", "#7a3f1d"}, {"Ivory", "#e8e2d6"},
        {"Ocean Blue", "#273f59"}}};

    constexpr std::array<const char*, 4> kGroups = {"Paint", "Wheels", "Calipers", "Interior"};

    // Parts of the car on hinges, which swing open together.
    struct Hinged
    {
        struct Part
        {
            GraphNode* node;
            Vector3 angles;   // when open
        };
        std::vector<Part> parts;
        float level = 0.0f;   // how far open, 0 to 1
    };

    constexpr float kHingeTime = 1.3f;
    constexpr float kPlatformHeight = 0.12f;
    constexpr float kStudioBrightness = 1.3f;
    // The room is lit by its lightmaps, the environment lights mostly the car, brighter still so
    // the car stands out of the dark room.
    constexpr float kEnvIntensity = 2.0f * kStudioBrightness;
    constexpr float kStripLightIntensity = 30.0f;
    constexpr float kHeadlightIntensity = 10.0f;
    constexpr float kHeadlightSetback = 0.3f;
    constexpr float kBeamsLit = 0.24f;
    constexpr float kBeamsDark = 2.0f;
    constexpr float kIdleTime = 3.0f;
    constexpr float kIdleOrbitSpeed = 6.0f;   // degrees a second
    constexpr float kSeatCircleRadius = 0.3f;
    constexpr float kDefaultZoomSpeed = 0.001f;   // the camera controls' own

    struct Ranges
    {
        Vector2 pitch;
        Vector2 zoom;
        float zoomSpeed;
    };
    // Orbit around the car. The ranges keep the camera inside the room and below the ceiling lights,
    // and looking down by at least 8 degrees, as the reflection of the car stretches at lower angles.
    const Ranges kOrbitRanges{Vector2(-24.0f, -8.0f), Vector2(2.0f, 8.0f), kDefaultZoomSpeed};
    // In the seat, the camera orbits a point just in front of it, so it looks around without
    // moving. It looks down by up to 40 degrees, as further down it sees little but the seat.
    const Ranges kSeatRanges{Vector2(-40.0f, 30.0f), Vector2(0.02f, 0.05f), 0.0f};

    const Vector3 kFocus(0.0f, 0.6f, 0.15f);
    // The views the camera moves to, all of them looking down by more than its lowest angle.
    const CameraPose kPaintShot{Vector3(5.2f, 2.75f, 5.0f), kFocus};
    const CameraPose kWheelsShot{Vector3(3.7f, 1.0f, 3.3f), Vector3(0.9f, 0.45f, 1.3f)};
    // The driver sits in the middle of the car.
    const CameraPose kSeat{Vector3(0.0f, 1.05f, 0.25f), Vector3(0.0f, 0.98f, 1.3f), 2.0f};
    // The camera leaves the box of the cabin and the doors through a door.
    const BoundingBox kCabinBox(Vector3(0.0f, 0.8f, 0.2f), Vector3(1.5f, 0.6f, 1.3f));
    // The bounds of the car with its doors and shells open, padded by 0.3 m for the camera's near plane.
    const BoundingBox kCarClearance(Vector3(0.0f, 1.2f, 0.2f), Vector3(1.75f, 1.5f, 3.05f));

    struct Flight
    {
        std::unique_ptr<CameraPath> path;
        bool entering = false;
        float gate = 0.0f;      // the progress the camera waits at for the doors to open
        float outside = 0.0f;   // the progress at which it is just outside of the door
    };

    struct LightMaterial
    {
        MaterialPtr material;
        float intensity;
    };
}

class CarShowcaseExample final : public ExampleApp
{
public:
    CarShowcaseExample() : ExampleApp({.width = 1280, .height = 720, .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _carAsset = std::make_unique<Asset>("car", AssetType::CONTAINER, assetPath("models/car-concept.glb"));
        _roomAsset = std::make_unique<Asset>("room", AssetType::CONTAINER, assetPath("models/car-showroom.glb"));
        _envAtlasAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/car-showroom-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _car = dynamic_cast<GlbContainerResource*>(_carAsset->resourceAs<ContainerResource>());
        _room = dynamic_cast<GlbContainerResource*>(_roomAsset->resourceAs<ContainerResource>());
        Texture* envAtlas = _envAtlasAsset->resourceAs<Texture>();
        if (!_car || !_room || !envAtlas) {
            spdlog::error("car-showcase needs models/car-concept.glb, models/car-showroom.glb and the showroom env atlas");
            return false;
        }

        createShowroom(envAtlas);
        createCar();
        createCameras();
        createText();

        applyPaint(kDefaultPaint, false);
        applyCalipers(0);
        applyRendering();
        updateLabels();
        spdlog::info("car-showcase: 1-4 paint / wheels / calipers / interior, [ ] option, L lights, S studio, "
                     "D doors, H shells, T TAA, B bloom, V beams");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN) {
            _idleTime = 0.0f;
            return pressKey(event.key.key);
        }
        switch (event.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_FINGER_DOWN:
            ++_pressedPointers;
            _idleTime = 0.0f;
            return false;
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            _pressedPointers = std::max(_pressedPointers - 1, 0);
            return false;
        case SDL_EVENT_MOUSE_WHEEL:
            _idleTime = 0.0f;
            return false;
        default:
            return false;
        }
    }

    bool pressKey(const SDL_Keycode key)
    {
        switch (key) {
        case SDLK_1: selectGroup(0); break;
        case SDLK_2: selectGroup(1); break;
        case SDLK_3: selectGroup(2); break;
        case SDLK_4: selectGroup(3); break;
        case SDLK_LEFTBRACKET: stepOption(-1); break;
        case SDLK_RIGHTBRACKET: stepOption(1); break;
        case SDLK_L: _lights = !_lights; break;
        case SDLK_S: _studio = !_studio; break;
        case SDLK_D: _doorsOpen = !_doorsOpen; break;
        case SDLK_H: _clamshellsOpen = !_clamshellsOpen; break;
        case SDLK_T: _taa = !_taa; applyRendering(); break;
        case SDLK_B: _bloom = !_bloom; applyRendering(); break;
        case SDLK_V: _beams = !_beams; applyRendering(); break;
        default: return false;
        }
        updateLabels();
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        playScriptedKeys();

        // The lights fade on and off; the indicators blink with a quick fade, like bulbs.
        const float lightsTarget = _lights ? 1.0f : 0.0f;
        if (_lightsLevel != lightsTarget) {
            _lightsLevel = std::clamp(_lightsLevel + signOf(lightsTarget - _lightsLevel) * dt * 3.0f, 0.0f, 1.0f);
            for (auto& [material, intensity] : _carLightMaterials) {
                material->setEmissiveIntensity(intensity * _lightsLevel);
            }
            for (auto* light : _headlights) {
                light->findComponent<LightComponent>()->setIntensity(kHeadlightIntensity * _lightsLevel);
                light->setEnabled(_lightsLevel > 0.0f);
            }
        }
        const float blink = std::clamp(std::sin(_time * std::numbers::pi_v<float> * 1.6f) * 4.0f, 0.0f, 1.0f) * _lightsLevel;
        if (blink != _indicatorLevel) {
            _indicatorLevel = blink;
            _indicator.material->setEmissiveIntensity(_indicator.intensity * blink);
        }

        // The studio lights dim to a glow, where the headlights light the room; the beams show more
        // in the dark room.
        const float studioTarget = _studio ? 1.0f : 0.1f;
        if (_studioLevel != studioTarget) {
            _studioLevel = std::clamp(_studioLevel + signOf(studioTarget - _studioLevel) * dt * 1.5f, 0.1f, 1.0f);
            for (auto& [material, intensity] : _studioMaterials) {
                material->setEmissiveIntensity(intensity * _studioLevel);
            }
            scene()->setSkyboxIntensity(kEnvIntensity * _studioLevel);
            _cameraComp->volumetricFog().localIntensity =
                std::lerp(kBeamsLit, kBeamsDark, (1.0f - _studioLevel) / 0.9f);
        }

        // the doors open for the flights of the camera, and close again when the camera is in the
        // seat or at the view outside
        swing(_doors, _doorsOpen || _flight != nullptr, dt);
        swing(_clamshells, _clamshellsOpen, dt);

        _paintSweep->update(dt);
        _trimSweep->update(dt);
        updateFlight(dt);
        updateIdleOrbit(dt);
    }

    // VISUTWIN_CAR_KEYS=<keys> presses one of the keys above a second, from the second second on,
    // for a capture of the interactions (a capture ignores the keyboard).
    void playScriptedKeys()
    {
        if (_scriptedKeys.empty()) {
            if (const char* keys = std::getenv("VISUTWIN_CAR_KEYS"); keys && !_scriptedKeysRead) {
                _scriptedKeys = keys;
            }
            _scriptedKeysRead = true;
            if (_scriptedKeys.empty()) {
                return;
            }
        }
        const auto due = static_cast<size_t>(std::max(_time - 1.0f, 0.0f));
        while (_scriptedKeysPressed < std::min(due, _scriptedKeys.size())) {
            const char c = _scriptedKeys[_scriptedKeysPressed++];
            spdlog::info("car-showcase: scripted key '{}'", c);
            pressKey(static_cast<SDL_Keycode>(std::tolower(static_cast<unsigned char>(c))));
        }
    }

    void preRender() override
    {
        // After the camera controls placed the camera this frame.
        updateSeatCircle();
        if (Texture* texture = _planarRenderer->frameUpdate()) {
            for (const auto& material : _reflectiveMaterials) {
                material->setPlanarReflectionMap(texture);
                material->setPlanarReflectionMatrix(_planarRenderer->viewProjectionMatrix());
            }
        }
    }

    void destroy() override
    {
        _paintSweep.reset();
        _trimSweep.reset();
        for (const auto& material : _reflectiveMaterials) {
            material->setPlanarReflectionMap(nullptr);
        }
    }

private:
    // ----- The showroom -----

    void createShowroom(Texture* envAtlas)
    {
        // The environment lights the car and gives everything its reflections. The cameras do not
        // render the skybox layer, the room surrounds them.
        scene()->setEnvAtlas(envAtlas);
        scene()->setSkyboxIntensity(kEnvIntensity);

        // The reflections are projected onto the box of the room, from the centre of which the
        // environment was rendered, so the reflections of its lights are where the lights are.
        const BoundingBox roomBox(Vector3(0.0f, 2.5f, 0.0f), Vector3(9.0f, 2.5f, 11.0f));
        for (const auto* container : {_room, _car}) {
            for (const auto& payload : container->meshPayloads()) {
                std::vector<std::shared_ptr<Material>> materials = {payload.material};
                for (const auto& [variant, material] : payload.variantMaterials) {
                    materials.push_back(material);
                }
                for (const auto& material : materials) {
                    if (auto standard = std::dynamic_pointer_cast<StandardMaterial>(material)) {
                        standard->setCubeMapProjection(CubeProjection::CUBEPROJ_BOX);
                        standard->setCubeMapProjectionBox(roomBox);
                    }
                }
            }
        }

        // The floor and the platform are on their own layer, rendered by the main camera but not
        // by the reflection camera, as they cannot be in their own reflection.
        const auto layers = scene()->layers();
        _worldLayer = layers->getLayerById(LAYERID_WORLD);
        _floorLayer = std::make_shared<Layer>("Floor", kFloorLayerId);
        layers->insert(_floorLayer, 1);

        _roomEntity = _room->instantiateRenderEntity();
        _roomEntity->setEngine(engine());
        root()->addChild(_roomEntity);
        struct Reflective
        {
            const char* name;
            float metalness;
        };
        for (const Reflective& r : {Reflective{"Floor", 0.7f}, Reflective{"PlatformTop", 0.85f}}) {
            if (auto* node = _roomEntity->findByName(r.name)) {
                if (auto* render = static_cast<Entity*>(node)->findComponent<RenderComponent>()) {
                    render->setLayers({kFloorLayerId});
                }
            }
            // mostly metallic, so the reflection shows clearly, and not only at grazing angles. The
            // baked lighting is emissive, the diffuse colour only takes the light of the headlights.
            MaterialPtr material = findMaterial(_room, r.name);
            material->setDiffuse(Color(0.5f, 0.5f, 0.52f, 1.0f));
            material->setMetalness(r.metalness);
            _reflectiveMaterials.push_back(material);
        }

        // The baked surfaces are lit by their emissive lighting, and not by the environment as well.
        const auto prepareRoomMaterial = [&](const char* name, const float boost) {
            MaterialPtr material = findMaterial(_room, name);
            if (material->emissiveMap()) {
                material->setAmbient(Color::BLACK);
            }
            material->setEmissiveIntensity(material->emissiveIntensity() * boost);
            return LightMaterial{material, material->emissiveIntensity()};
        };
        // The stone panels are the lights of the studio, which dim together with the baked lighting,
        // and are brighter than they were baked, so they bloom more.
        for (const auto& [name, boost] : std::initializer_list<std::pair<const char*, float>>{
                 {"Wall", 1.0f}, {"Ceiling", 1.0f}, {"Floor", 1.0f}, {"PlatformTop", 1.0f}, {"Panel", 1.5f}}) {
            _studioMaterials.push_back(prepareRoomMaterial(name, boost * kStudioBrightness));
        }
        // The ceiling strips stay on when the studio dims, like the light of the platform. Their
        // light on the room is baked together with the light of the panels, and dims with it.
        const MaterialPtr stripMaterial = prepareRoomMaterial("LightStrip", 1.3f).material;

        // The room is lit by its baked lighting, and only by the lights which affect lightmapped meshes.
        for (auto* render : _roomEntity->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                meshInstance->setMask(MASK_AFFECT_LIGHTMAPPED);
            }
        }

        // Rectangular area lights in place of the strips light the car, which reflects them exactly
        // where they are. The environment does not show the strips, so the car does not reflect
        // them twice. None of the lights casts shadows.
        scene()->lighting().areaLightsEnabled = true;
        scene()->lighting().shadowsEnabled = false;
        // All the area lights are rectangles, so the cluster loop carries no disk or sphere code.
        scene()->lighting().areaLightDiskEnabled = false;
        scene()->lighting().areaLightSphereEnabled = false;
        auto* strips = static_cast<Entity*>(_roomEntity->findByName("LightStrips"));
        const BoundingBox stripBounds = strips ? strips->findComponent<RenderComponent>()->meshInstances()[0]->aabb()
                                               : BoundingBox(Vector3(0.0f, 4.8f, 0.0f), Vector3(6.0f, 0.05f, 3.0f));
        for (const float x : {-5.6f, -1.7f, 1.7f, 5.6f}) {
            auto* light = new Entity();
            light->setEngine(engine());
            light->setName("StripLight");
            auto* component = static_cast<LightComponent*>(light->addComponent<LightComponent>());
            component->setType(LightType::LIGHTTYPE_OMNI);
            component->setShape(LightShape::LIGHTSHAPE_RECT);
            component->setColor(stripMaterial->emissive());
            component->setIntensity(kStripLightIntensity);
            component->setRange(12.0f);
            component->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
            // the rectangle is in the XZ plane of the light, which shines down
            light->setLocalScale(Vector3(0.07f, 1.0f, stripBounds.halfExtents().getZ() * 2.0f));
            light->setLocalPosition(x, stripBounds.center().getY() - stripBounds.halfExtents().getY(),
                stripBounds.center().getZ());
            root()->addChild(light);
        }
    }

    // ----- The car -----

    void createCar()
    {
        // The car stands on the platform.
        _carEntity = _car->instantiateRenderEntity();
        _carEntity->setEngine(engine());
        _carEntity->setLocalPosition(0.0f, kPlatformHeight, 0.0f);
        root()->addChild(_carEntity);

        // The strip lights cast no shadows. The ambient occlusion of the car darkens their light as
        // well, so they light its interior and the insides of its wheel arches through the roof and
        // the body much less. The brakes, deep in the wheel arches, they do not light at all.
        for (const auto& payload : _car->meshPayloads()) {
            if (auto standard = std::dynamic_pointer_cast<StandardMaterial>(payload.material);
                standard && standard->aoMap()) {
                standard->setOccludeDirect(true);
            }
        }
        for (auto* render : _carEntity->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                const std::string& name = meshInstance->material() ? meshInstance->material()->name() : "";
                if (name == "Brake" || name == "Disc") {
                    meshInstance->setMask(MASK_AFFECT_LIGHTMAPPED);
                }
            }
        }

        // The windows are tinted glass, blended over the interior, keeping their reflections.
        if (MaterialPtr glass = findMaterial(_car, "Glass")) {
            glass->setUseDynamicRefraction(false);
            glass->setTransmissionFactor(0.0f);
            glass->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
            glass->setTransparent(true);
            glass->setDiffuse(Color(0.02f, 0.02f, 0.025f, 1.0f));
            glass->setOpacity(0.35f);
            glass->setOpacityFadesSpecular(false);
            glass->setDepthState(std::make_shared<DepthState>(DepthState::noWrite()));
        }

        // The blacks of the glTF reflect less than half a percent of the light, so in the cabin they
        // show only their specular highlights. Real black leather, plastic and rubber reflect a few
        // percent, which shows their shapes and the textures of their normal maps in the dim light.
        for (const auto& [name, color] : std::initializer_list<std::pair<const char*, const char*>>{
                 // the dash, the door panels and the steering wheel, the centres of the seats and the console
                 {"Interior 1", "#2a2a2a"}, {"Interior 2", "#2a2a2a"}, {"Panel Sides", "#2a2a2a"},
                 // the trims: the cage, the seat frames, the gaskets and the insides of the clamshells
                 {"Untitled", "#222222"}, {"Floormat", "#1c1c1c"}}) {
            if (MaterialPtr material = findMaterial(_car, name)) {
                material->setDiffuse(hex(color));
            }
        }

        // The butterfly doors are hinged at the front of their openings, and swing up and out. The
        // front clamshell, the bonnet, is hinged at the nose and tilts forward with the headlights.
        // The rear one, the boot, is hinged at the tail and lifts the rear window and lights.
        _doors = hinged({{"BodyDoorLColor1", Vector3(62.0f, 0.0f, -16.0f)}, {"BodyDoorRColor1", Vector3(62.0f, 0.0f, 16.0f)}});
        _clamshells = hinged({{"BodyHood", Vector3(50.0f, 0.0f, 0.0f)}, {"BodyRearPanelsColor1", Vector3(-55.0f, 0.0f, 0.0f)}});

        // The emissive materials of the car lights, much brighter than in the glTF, so they bloom.
        // The glTF gives them a black base, which shows when they are off, so they look like what
        // is behind their glass instead: the chrome reflectors of the headlights, the red and amber
        // lenses of the rear lights, and the black glass of the dashboard display.
        struct CarLight
        {
            const char* name;
            float boost;
            const char* color;
            float metalness;
            float roughness;
        };
        const auto prepareCarLight = [&](const CarLight& light) {
            MaterialPtr material = findMaterial(_car, light.name);
            material->setDiffuse(hex(light.color));
            material->setMetalness(light.metalness);
            material->setGloss(1.0f - light.roughness);
            return LightMaterial{material, material->emissiveIntensity() * light.boost};
        };
        for (const CarLight& light : {CarLight{"Headlight", 5.0f, "#c4cad2", 1.0f, 0.15f},
                 CarLight{"Brakelight", 12.0f, "#5a0808", 0.0f, 0.1f}, CarLight{"Dashboard", 1.0f, "#040404", 0.0f, 0.1f}}) {
            _carLightMaterials.push_back(prepareCarLight(light));
        }
        _indicator = prepareCarLight({"Signallight", 12.0f, "#6a4206", 0.0f, 0.1f});

        // A spot light in each headlight, shining forward and slightly down. The car faces the +Z
        // axis. The lights are on the bonnet, which lifts them with the headlights. They sit back
        // inside the body, so the sharp tips of their cones in the haze hide behind it, and the beams
        // leave the headlights wide.
        GraphNode* bonnet = _clamshells.parts[0].node;
        auto* headlightsNode = static_cast<Entity*>(_carEntity->findByName("BodyHeadlights"));
        const BoundingBox headlightBounds = headlightsNode
            ? headlightsNode->findComponent<RenderComponent>()->meshInstances()[0]->aabb()
            : BoundingBox(Vector3(0.0f, 0.7f, 2.2f), Vector3(0.8f, 0.1f, 0.2f));
        for (const float side : {-1.0f, 1.0f}) {
            auto* light = new Entity();
            light->setEngine(engine());
            light->setName("Headlight");
            auto* component = static_cast<LightComponent*>(light->addComponent<LightComponent>());
            component->setType(LightType::LIGHTTYPE_SPOT);
            component->setLayers({LAYERID_WORLD, kFloorLayerId});
            component->setColor(Color(0.85f, 0.92f, 1.0f, 1.0f));
            component->setIntensity(kHeadlightIntensity);
            component->setRange(16.0f);
            // the headlights light the room as well as the car
            component->setMask(MASK_AFFECT_DYNAMIC | MASK_AFFECT_LIGHTMAPPED);
            component->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
            component->setInnerConeAngle(12.0f);
            component->setOuterConeAngle(28.0f);
            // the beams scatter in the haze of the room
            component->setVolumetricScattering(1.0f);
            bonnet->addChild(light);
            const Vector3 centre = headlightBounds.center();
            const Vector3 half = headlightBounds.halfExtents();
            light->setPosition(Vector3(centre.getX() + side * half.getX() * 0.7f, centre.getY(),
                centre.getZ() + half.getZ()));
            // spot lights shine down their negative Y axis
            light->setRotation(Quaternion::fromEulerAngles(-84.0f, 0.0f, 0.0f));
            light->setPosition(light->position() + upOf(light) * kHeadlightSetback);
            _headlights.push_back(light);
        }

        // a subtler normal map of the metallic flakes, which otherwise reads as noise at a distance
        for (const char* name : {"Paint 1 Carmine", "Paint 2 Carmine"}) {
            if (MaterialPtr material = findMaterial(_car, name)) {
                material->setBumpiness(0.08f);
            }
        }
        // The paints share one shader, so a change of the paint compiles nothing. Each paint
        // material has every feature any paint uses: the normal map of the flakes, the clear coat,
        // and the iridescent film with its thickness texture. Where its own look has none of them,
        // they are set to have no effect.
        {
            const MaterialPtr flakes = findMaterial(_car, "Paint 1 Carmine");
            const MaterialPtr film = findMaterial(_car, "Paint 1 Pearl");
            // a clear coat or iridescence too faint to see, as a value of 0 leaves it out of the shader
            constexpr float kFaint = 1e-9f;
            for (const char* look : {"Carmine", "Pearl", "Graphite"}) {
                for (const char* slot : {"Paint 1", "Paint 2"}) {
                    const MaterialPtr material = findMaterial(_car, std::string(slot) + " " + look);
                    if (!material || !flakes || !film) {
                        continue;
                    }
                    if (!material->normalMap()) {
                        // the flakes, flattened out by a bumpiness of 0
                        material->setNormalMap(flakes->normalMap());
                        material->setNormalMapTiling(flakes->normalMapTiling());
                        material->setNormalMapOffset(flakes->normalMapOffset());
                        material->setBumpiness(0.0f);
                    }
                    if (material->clearCoat() <= 0.0f) {
                        // the gloss is set as the coat of the flakes sets it, which the shader reads the same way
                        material->setClearCoat(kFaint);
                        material->setClearCoatGloss(flakes->clearCoatGloss());
                        material->setClearCoatGlossInvert(flakes->clearCoatGlossInvert());
                    }
                    if (material->iridescenceIntensity() <= 0.0f) {
                        material->setIridescenceIntensity(kFaint);
                    }
                    if (!material->iridescenceThicknessMap()) {
                        // the texture blends between two equal thicknesses, so the film keeps its one thickness
                        material->setIridescenceThicknessMap(film->iridescenceThicknessMap());
                        material->setIridescenceThicknessMapUv(film->iridescenceThicknessMapUv());
                        material->setIridescenceThicknessMin(material->iridescenceThicknessMax());
                    }
                }
            }
        }

        // The candy paint is tinted for the colours other than its own, and the pearl paint takes
        // other base colours and film thicknesses for other colours of its swirls.
        _candyColor = findMaterial(_car, "Paint 1 Carmine")->diffuse();
        if (MaterialPtr pearl = findMaterial(_car, "Paint 1 Pearl")) {
            _pearlBase = pearl->diffuse();
            _pearlThickness = {pearl->iridescenceThicknessMin(), pearl->iridescenceThicknessMax()};
            _pearlIridescence = pearl->iridescenceIntensity();
        }

        // A new paint sweeps over the car from its front to its back, with a warm glowing front. A
        // new trim of the interior sweeps the same way, with a cool one, so a paint and its trim
        // change together.
        const auto lane = [this](const int index) {
            return [this, index](const float value) {
                _userData[index] = value;
                scene()->setUserShaderData(0, Vector4(_userData[0], _userData[1], _userData[2], _userData[3]));
            };
        };
        _paintSweep = std::make_unique<MaterialSweep>(MaterialSweep::Options{.container = _car, .entity = _carEntity,
            .layer = _worldLayer, .slots = {"Paint 1", "Paint 2"}, .looks = {"Carmine", "Pearl", "Graphite"},
            .start = 2.8f, .end = -2.4f, .glow = Color(1.0f, 0.45f, 0.15f, 1.0f), .intensity = 14.0f, .lane = "x",
            .setPosition = lane(0)});
        _trimSweep = std::make_unique<MaterialSweep>(MaterialSweep::Options{.container = _car, .entity = _carEntity,
            .layer = _worldLayer, .slots = {"Interior 3"}, .looks = {"Carmine"}, .start = 2.8f, .end = -2.4f,
            .glow = Color(0.3f, 0.6f, 1.0f, 1.0f), .intensity = 10.0f, .lane = "y", .setPosition = lane(1)});

        _rimMaterial = findMaterial(_car, "Rim1");
        _caliperMaterial = findMaterial(_car, "Brake");
        _caliperRed = _caliperMaterial->diffuse();
        // The calipers hide behind the spokes, in the dark wheel arches. Painted instead of metallic,
        // without the ambient occlusion and with a little glow of their colour, they show through
        // the spokes.
        _caliperMaterial->setMetalness(0.0f);
        _caliperMaterial->setAoMap(nullptr);
        _caliperMaterial->setEmissiveIntensity(0.3f);
    }

    Hinged hinged(const std::vector<std::pair<const char*, Vector3>>& parts)
    {
        Hinged result;
        for (const auto& [name, angles] : parts) {
            if (GraphNode* node = _carEntity->findByName(name)) {
                result.parts.push_back({node, angles});
            } else {
                spdlog::warn("car-showcase: no node '{}'", name);
            }
        }
        return result;
    }

    // Swings hinged parts towards open or closed, easing in and out.
    static void swing(Hinged& parts, const bool open, const float dt)
    {
        const float target = open ? 1.0f : 0.0f;
        if (parts.level == target) {
            return;
        }
        parts.level = std::clamp(parts.level + signOf(target - parts.level) * dt / kHingeTime, 0.0f, 1.0f);
        const float t = smoothstep01(parts.level);
        for (auto& [node, angles] : parts.parts) {
            node->setLocalEulerAngles(angles.getX() * t, angles.getY() * t, angles.getZ() * t);
        }
    }

    int trimOf(const int paint) const
    {
        for (size_t i = 0; i < kTrims.size(); ++i) {
            if (std::string_view(kTrims[i].name) == kPaints[static_cast<size_t>(paint)].trim) {
                return static_cast<int>(i);
            }
        }
        return 0;
    }

    void applyTrim(const int index, const bool animate = true)
    {
        if (index == _currentTrim) {
            return;
        }
        _currentTrim = index;
        const Color color = hex(kTrims[static_cast<size_t>(index)].color);
        _trimSweep->apply("Carmine", [&](std::map<std::string, MaterialPtr>& materials) {
            if (materials["Interior 3"]) {
                materials["Interior 3"]->setDiffuse(color);
            }
        }, animate);
    }

    // Changes the paint, and the trim to the one the paint comes with.
    void applyPaint(const int index, const bool animate = true)
    {
        const Paint& paint = kPaints[static_cast<size_t>(index)];
        _paintSweep->apply(paint.look, [&](std::map<std::string, MaterialPtr>& materials) {
            MaterialPtr material = materials["Paint 1"];
            if (!material) {
                return;
            }
            if (std::string_view(paint.look) == "Carmine") {
                material->setDiffuse(paint.tint ? hex(paint.tint) : _candyColor);
            } else if (std::string_view(paint.look) == "Pearl") {
                material->setDiffuse(paint.filmBase ? hex(paint.filmBase) : _pearlBase);
                material->setIridescenceThicknessMin(paint.filmBase ? paint.filmThickness[0] : _pearlThickness[0]);
                material->setIridescenceThicknessMax(paint.filmBase ? paint.filmThickness[1] : _pearlThickness[1]);
                material->setIridescenceIntensity(paint.filmBase ? paint.filmIridescence : _pearlIridescence);
            }
        }, animate);
        _selected[3] = trimOf(index);
        applyTrim(_selected[3], animate);
    }

    void applyWheels(const int index)
    {
        const Finish& wheels = kWheels[static_cast<size_t>(index)];
        _rimMaterial->setDiffuse(hex(wheels.color));
        _rimMaterial->setMetalness(wheels.metalness);
        _rimMaterial->setGloss(1.0f - wheels.roughness);
    }

    void applyCalipers(const int index)
    {
        const Color color = index == 0 ? _caliperRed : hex(kCalipers[static_cast<size_t>(index)].color);
        _caliperMaterial->setDiffuse(color);
        _caliperMaterial->setEmissive(color);
    }

    // ----- The configurator -----

    int optionCount(const int group) const
    {
        switch (group) {
        case 0: return static_cast<int>(kPaints.size());
        case 1: return static_cast<int>(kWheels.size());
        case 2: return static_cast<int>(kCalipers.size());
        default: return static_cast<int>(kTrims.size());
        }
    }

    const char* optionName(const int group, const int index) const
    {
        const auto i = static_cast<size_t>(index);
        switch (group) {
        case 0: return kPaints[i].name;
        case 1: return kWheels[i].name;
        case 2: return kCalipers[i].name;
        default: return kTrims[i].name;
        }
    }

    void selectGroup(const int group)
    {
        _group = group;
        _bodyView = group == 0;
        if (group == 3) {
            enterCabin();
        } else {
            moveCamera(group == 0 ? kPaintShot : kWheelsShot);
        }
    }

    void stepOption(const int step)
    {
        const int count = optionCount(_group);
        const int index = (_selected[static_cast<size_t>(_group)] + step + count) % count;
        _selected[static_cast<size_t>(_group)] = index;
        switch (_group) {
        case 0: applyPaint(index); break;
        case 1: applyWheels(index); break;
        case 2: applyCalipers(index); break;
        default: applyTrim(index); break;
        }
    }

    void applyRendering()
    {
        // TAA resolves the edges, otherwise MSAA does
        auto& rendering = _cameraComp->rendering();
        _cameraComp->taa().enabled = _taa;
        rendering.samples = _taa ? 1 : 4;
        rendering.sharpness = _taa ? 0.5f : 0.0f;
        rendering.bloomIntensity = _bloom ? 0.04f : 0.0f;
        _cameraComp->volumetricFog().enabled = _beams;
    }

    // ----- The cameras -----

    void createCameras()
    {
        _camera = createCamera(kPaintShot.position);
        _camera->setName("MainCamera");
        _cameraComp = _camera->findComponent<CameraComponent>();
        _cameraComp->camera()->setNearClip(0.1f);
        _cameraComp->camera()->setFarClip(50.0f);
        _cameraComp->setLayers({LAYERID_WORLD, kFloorLayerId, LAYERID_UI});
        updateFov();

        _controls = addOrbitControls(_camera, kFocus);
        _controls->setEnablePan(false);
        setRanges(kOrbitRanges);

        // Rendering with a neutral tone mapping, bloom making the lights glow, and the volumetric fog
        // as the haze the headlights scatter in, lit only by the spot lights: less forward scattering
        // than the default, so looking into the headlights does not wash out the front of the car.
        _cameraComp->setToneMapping(TONEMAP_NEUTRAL);
        auto& rendering = _cameraComp->rendering();
        rendering.bloomBlurLevel = 7;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.3f;
        auto& fog = _cameraComp->volumetricFog();
        fog.density = 0.04f;
        fog.heightFalloff = 0.0f;
        fog.ambientIntensity = 0.0f;
        fog.maxDistance = 30.0f;
        fog.localSpotLights = true;
        fog.localSteps = 16;
        fog.anisotropy = 0.5f;
        fog.localIntensity = kBeamsLit;

        // The reflection camera renders the World layer mirrored by the plane of the platform, into
        // a texture with mipmaps for the blurred reflections, and a high dynamic range, so the
        // reflections of the lights are as bright as the lights. The floor around the platform uses
        // the same reflection.
        _reflectionCamera = new Entity();
        _reflectionCamera->setEngine(engine());
        _reflectionCamera->setName("ReflectionCamera");
        auto* reflectionComp = static_cast<CameraComponent*>(_reflectionCamera->addComponent<CameraComponent>());
        reflectionComp->setLayers({LAYERID_WORLD});
        reflectionComp->setPriority(-1);
        _reflectionCamera->addComponent<ScriptComponent>();
        _planarRenderer = _reflectionCamera->script()->create<PlanarRenderer>();
        _planarRenderer->setSceneCameraEntity(_camera);
        _planarRenderer->setMipmaps(true);
        _planarRenderer->setHdr(true);
        // at the full resolution of the screen, the sharp reflections are not pixelated
        _planarRenderer->setScale(1.0f);
        _planarRenderer->setPlane(Vector3(0.0f, kPlatformHeight, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
        root()->addChild(_reflectionCamera);
    }

    // A horizontal field of view of 60 degrees keeps the car in view in narrow windows, limited to a
    // vertical one of 70 degrees in portrait windows. In the cabin, it widens to 80 and 85 degrees,
    // and the near clip plane moves closer, so it does not cut open the wings of the seat.
    void updateFov()
    {
        const auto [width, height] = device()->size();
        const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const float blend = smoothstep01(_cabinFov);
        const float horizontal = std::lerp(60.0f, 80.0f, blend) * std::numbers::pi_v<float> / 180.0f;
        _cameraComp->camera()->setFov(std::min(std::lerp(70.0f, 85.0f, blend),
            2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect) * 180.0f / std::numbers::pi_v<float>));
        _cameraComp->camera()->setNearClip(std::lerp(0.1f, 0.02f, blend));
    }

    void setRanges(const Ranges& ranges)
    {
        _controls->setPitchRange(ranges.pitch);
        _controls->setZoomRange(ranges.zoom);
        _controls->setZoomSpeed(ranges.zoomSpeed);
    }

    // The views of the camera passing through a door, outside of it and in its doorway. The camera
    // passes through the doorway behind the hinge, as the bottom edge of the open door slopes down
    // towards it.
    static CameraPose doorOutside(const float side)
    {
        return {Vector3(2.3f * side, 1.0f, -0.05f), Vector3(0.0f, 0.95f, 0.1f)};
    }
    static CameraPose doorway(const float side)
    {
        return {Vector3(1.15f * side, 0.98f, -0.05f), Vector3(0.0f, 0.97f, 0.6f), 2.0f};
    }

    // A flight of the camera through poses from where it is, while the camera controls are off.
    void fly(std::vector<CameraPose> poses, const bool entering,
        const std::function<std::pair<float, float>(const std::vector<float>&)>& stops)
    {
        const Vector3 position = _camera->position();
        poses.insert(poses.begin(), {position, position + forwardOf(_camera) * 2.0f});
        auto path = std::make_unique<CameraPath>(poses, [](const float length) {
            return std::clamp(1.4f + length * 0.16f, 1.6f, 3.4f);
        });
        const auto [gate, outside] = stops(path->knots);
        _flight = std::make_unique<Flight>(Flight{std::move(path), entering, gate, outside});
        _seated = false;
        _controls->setEnabled(false);
    }

    // The camera controls take the camera back from a flight where it is: setting their focus point
    // moves them to the camera at once, while a reset would move them smoothly from before the flight.
    void handBack(const Vector3& focus, const Ranges& ranges)
    {
        setRanges(ranges);
        _controls->setEnabled(true);
        _controls->setFocusPoint(focus);
    }

    // Moves the camera smoothly to a view, out of the cabin through a door if it is in.
    void moveCamera(const CameraPose& shot)
    {
        const Vector3 position = _camera->position();
        const float side = signOf(shot.position.getX());
        if (boxContains(kCabinBox, position)) {
            // backing out through the door on the side of the view, once the door opens
            CameraPose outside = doorOutside(side);
            outside.pace = 2.0f;
            fly({doorway(side), outside, shot}, false, [](const std::vector<float>& knots) {
                return std::pair{knots[1] * 0.35f, knots[2]};
            });
        } else if (_flight && signOf(position.getX()) == side) {
            // on to the view, from outside of the car on the side of the view
            fly({shot}, false, [](const std::vector<float>&) { return std::pair{1.0f, 0.0f}; });
        } else {
            // the camera controls orbit the car to the view
            if (_flight) {
                _flight.reset();
                handBack(_flightFocus, kOrbitRanges);
            }
            _controls->reset(shot.focus, shot.position);
        }
    }

    // Gets in through the door on the side of the camera.
    void enterCabin()
    {
        if (_seated || (_flight && _flight->entering)) {
            return;
        }
        const Vector3 position = _camera->position();
        if (boxContains(kCabinBox, position)) {
            fly({kSeat}, true, [](const std::vector<float>&) { return std::pair{0.0f, 0.0f}; });
        } else {
            const float side = position.getX() < 0.0f ? -1.0f : 1.0f;
            fly({doorOutside(side), doorway(side), kSeat}, true, [](const std::vector<float>& knots) {
                return std::pair{knots[1], knots[1]};
            });
        }
    }

    void updateFlight(const float dt)
    {
        // the field of view widens in the cabin, from the outside of the door to the seat
        const float progress = _flight ? _flight->path->progressAt(_flight->path->time) : 0.0f;
        const bool inCabin = _flight ? (_flight->entering ? progress > _flight->outside : progress < _flight->outside)
                                     : _seated;
        const float fovTarget = inCabin ? 1.0f : 0.0f;
        if (_cabinFov != fovTarget) {
            _cabinFov = std::clamp(_cabinFov + signOf(fovTarget - _cabinFov) * dt * 1.5f, 0.0f, 1.0f);
        }
        updateFov();   // the window may have been resized too

        if (!_flight) {
            return;
        }
        CameraPath& path = *_flight->path;
        if (_doors.level >= 1.0f || path.progressAt(path.time + dt) <= _flight->gate) {
            path.time = std::min(path.time + dt, path.duration);
        }
        path.evaluate(_flightPosition, _flightFocus);
        _camera->setPosition(_flightPosition);
        _camera->lookAt(_flightFocus);

        if (path.done()) {
            const bool entering = _flight->entering;
            _flight.reset();
            _seated = entering;
            if (entering) {
                handBack(_flightPosition + forwardOf(_camera) * 0.03f, kSeatRanges);
            } else {
                handBack(_flightFocus, kOrbitRanges);
            }
        }
    }

    // Whether the camera orbiting the focus point cannot get into the car: it is above the car, or
    // its circle around the focus point encloses the car.
    static bool clearsCar(const Vector3& position, const Vector3& focus)
    {
        const Vector3 min = kCarClearance.center() - kCarClearance.halfExtents();
        const Vector3 max = kCarClearance.center() + kCarClearance.halfExtents();
        if (position.getY() > max.getY()) {
            return true;
        }
        const float cornerX = std::max(std::abs(min.getX() - focus.getX()), std::abs(max.getX() - focus.getX()));
        const float cornerZ = std::max(std::abs(min.getZ() - focus.getZ()), std::abs(max.getZ() - focus.getZ()));
        const float x = position.getX() - focus.getX();
        const float z = position.getZ() - focus.getZ();
        return x * x + z * z > cornerX * cornerX + cornerZ * cornerZ;
    }

    // After a few seconds without any input, the camera slowly orbits the car, easing in, until the
    // next input takes the control back. It orbits a position of its own, which the controls follow.
    // It only orbits in the view of the body, and only where it cannot swing into the car.
    void updateIdleOrbit(const float dt)
    {
        const bool orbits = _pressedPointers == 0 && _bodyView && !_flight && !_seated &&
            clearsCar(_camera->position(), _controls->focusPoint());
        _idleTime = orbits ? _idleTime + dt : 0.0f;
        if (_idleTime < kIdleTime) {
            _idleSpeed = 0.0f;
            return;
        }
        if (_idleSpeed == 0.0f) {
            _idlePosition = _camera->position();
        }
        _idleSpeed = std::min(_idleSpeed + (kIdleOrbitSpeed / 2.0f) * dt, kIdleOrbitSpeed);
        const Vector3 focus = _controls->focusPoint();
        const Quaternion rotation = Quaternion::fromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), _idleSpeed * dt);
        _idlePosition = rotation * (_idlePosition - focus) + focus;
        _controls->reset(focus, _idlePosition);
    }

    // In the seat, the camera moves on a circle through the seat, to the side opposite of where it
    // looks: it stays in the seat looking ahead, moves right and forward looking at the left door,
    // and 0.6 m forward looking back. It sees more of the cabin that way, and past the wings of the
    // seat beside the head instead of into them.
    void updateSeatCircle()
    {
        if (!_seated) {
            return;
        }
        const Vector3 forward = forwardOf(_camera);
        const float scale = kSeatCircleRadius / std::max(std::hypot(forward.getX(), forward.getZ()), 1e-6f);
        const Vector3 position = _camera->position();
        _camera->setPosition(Vector3(position.getX() - forward.getX() * scale, position.getY(),
            position.getZ() + kSeatCircleRadius - forward.getZ() * scale));
    }

    // ----- The labels, in place of the overlay -----

    void createText()
    {
        auto* font = _fontAsset->resourceAs<FontResource>();
        if (!font) {
            spdlog::warn("car-showcase: the font did not load; no labels");
            return;
        }
        _screen = createScreen();
        const auto addText = [&](const Vector4& anchor, const Vector2& pivot, const int size, const Color& color) {
            auto* entity = new Entity();
            entity->setEngine(engine());
            auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
            element->setup({.type = ElementType::Text, .anchor = anchor, .pivot = pivot});
            element->setFontResource(font);
            element->setFontSize(size);
            element->setColor(color);
            element->setLayers({LAYERID_UI});
            _screen->entity()->addChild(entity);
            return element;
        };
        const Color dim(0.75f, 0.75f, 0.75f, 1.0f);
        _eyebrow = addText(Vector4(0.03f, 0.9f, 0.03f, 0.9f), Vector2(0.0f, 0.5f), 12, Color(0.6f, 0.6f, 0.6f, 1.0f));
        _eyebrow->setText("SHOWROOM");
        _title = addText(Vector4(0.03f, 0.85f, 0.03f, 0.85f), Vector2(0.0f, 0.5f), 36, Color(1.0f, 1.0f, 1.0f, 1.0f));
        _title->setText("Concept");
        _subtitle = addText(Vector4(0.03f, 0.8f, 0.03f, 0.8f), Vector2(0.0f, 0.5f), 14, dim);
        _option = addText(Vector4(0.5f, 0.08f, 0.5f, 0.08f), Vector2(0.5f, 0.5f), 16, Color(1.0f, 1.0f, 1.0f, 1.0f));
        _hint = addText(Vector4(0.5f, 0.04f, 0.5f, 0.04f), Vector2(0.5f, 0.5f), 12, dim);
    }

    void updateLabels()
    {
        if (!_option) {
            return;
        }
        _subtitle->setText(kPaints[static_cast<size_t>(_selected[0])].name);
        _option->setText(std::string(kGroups[static_cast<size_t>(_group)]) + "  ·  " +
            optionName(_group, _selected[static_cast<size_t>(_group)]));
        _hint->setText(std::format("1-4 group   [ ] option   L lights {}   S studio {}   D doors {}   H shells {}   "
                                   "T TAA {}   B bloom {}   V beams {}",
            _lights ? "on" : "off", _studio ? "on" : "off", _doorsOpen ? "open" : "closed",
            _clamshellsOpen ? "open" : "closed", _taa ? "on" : "off", _bloom ? "on" : "off", _beams ? "on" : "off"));
    }

    std::unique_ptr<Asset> _carAsset;
    std::unique_ptr<Asset> _roomAsset;
    std::unique_ptr<Asset> _envAtlasAsset;
    std::unique_ptr<Asset> _fontAsset;
    GlbContainerResource* _car = nullptr;
    GlbContainerResource* _room = nullptr;
    Entity* _carEntity = nullptr;
    Entity* _roomEntity = nullptr;
    std::shared_ptr<Layer> _worldLayer;
    std::shared_ptr<Layer> _floorLayer;
    std::vector<MaterialPtr> _reflectiveMaterials;
    std::vector<LightMaterial> _studioMaterials;
    std::vector<LightMaterial> _carLightMaterials;
    LightMaterial _indicator;
    std::vector<Entity*> _headlights;
    Hinged _doors;
    Hinged _clamshells;
    std::unique_ptr<MaterialSweep> _paintSweep;
    std::unique_ptr<MaterialSweep> _trimSweep;
    std::array<float, 4> _userData = {kSweepIdle, kSweepIdle, 0.0f, 0.0f};
    Color _candyColor;
    Color _pearlBase;
    std::array<float, 2> _pearlThickness = {100.0f, 400.0f};
    float _pearlIridescence = 1.0f;
    MaterialPtr _rimMaterial;
    MaterialPtr _caliperMaterial;
    Color _caliperRed;
    int _currentTrim = -1;

    Entity* _camera = nullptr;
    CameraComponent* _cameraComp = nullptr;
    CameraControls* _controls = nullptr;
    Entity* _reflectionCamera = nullptr;
    PlanarRenderer* _planarRenderer = nullptr;
    float _cabinFov = 0.0f;
    std::unique_ptr<Flight> _flight;
    Vector3 _flightPosition;
    Vector3 _flightFocus;
    bool _seated = false;
    bool _bodyView = true;
    int _pressedPointers = 0;
    float _idleTime = 0.0f;
    float _idleSpeed = 0.0f;
    Vector3 _idlePosition;

    // The settings and the selections of the configurator.
    int _group = 0;
    std::array<int, 4> _selected = {kDefaultPaint, 0, 0, 0};
    bool _lights = true;
    bool _studio = true;
    bool _doorsOpen = false;
    bool _clamshellsOpen = false;
    bool _taa = true;
    bool _bloom = true;
    bool _beams = true;
    float _lightsLevel = 0.0f;
    float _studioLevel = 1.0f;
    float _indicatorLevel = 0.0f;
    float _time = 0.0f;

    std::string _scriptedKeys;
    bool _scriptedKeysRead = false;
    size_t _scriptedKeysPressed = 0;

    ScreenComponent* _screen = nullptr;
    ElementComponent* _eyebrow = nullptr;
    ElementComponent* _title = nullptr;
    ElementComponent* _subtitle = nullptr;
    ElementComponent* _option = nullptr;
    ElementComponent* _hint = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(CarShowcaseExample)
