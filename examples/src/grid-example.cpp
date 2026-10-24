// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Port of upstream graphics/grid.
//
// A reference grid with every setting of the Grid. A box, a sphere, a dark plane and a capsule
// stand half through it on a 2 x 2 layout, lit by a default directional light at euler
// (0, 0, -60) with ambient 0.2. Lower the depth offset to see the dark plane hide the grid, and
// pick the SuperSplat preset to see it hide a sphere of gaussian splats with depth write. The
// camera starts at 4 x its aspect on each axis, orbiting the origin (zoom 0.5..500, pitch
// +/-89.999, damping 0.95, no fly), far clip 1000.
//
// Keys stand in for upstream's controls panel:
//   1 2 3  preset: default, editor, SuperSplat (splats in place of the sphere)
//   V      grid visible on / off
//   G      plane, cycled XZ / XY / YZ
//   [ / ]  spacing halved / doubled (0.1..50)
//   - / =  subdivisions -/+ 1 (1..10)
//   I      infinite on / off (a fixed grid is the preset's size, the entity's scale)
//   A      show axes on / off
//   C      contrast on / off (requests the camera's scene colour map)
//   O      depth offset, cycled 0.001 / 0 / 0.01
//   T / W / D  depth test / depth write / depth dither on / off
//   P      projection, perspective / orthographic
//   F      far clip, cycled 1000 / 100 / 2000
// VISUTWIN_GRID_STATE=preset[,plane[,ortho[,contrast]]] starts there (preset 0-2, plane 0 XZ,
// 1 XY, 2 YZ), for a capture.
//
// DEVIATIONS:
// - upstream's Grid script is the Grid extra used directly (Grid::attach).
// - the splat sphere is built as activated splats (GSplatData::fromActivated): 6000 isotropic
//   splats of scale 0.025 on a Fibonacci sphere, coloured by direction and shaded from one side,
//   as upstream fills its simple-format container.
// - no colour pickers, fade sliders or thickness sliders; the presets set them.
// - the camera controls have no `sceneSize` here; with fly off it changes nothing upstream shows.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>

#include "../exampleApp.h"
#include "extras/grid/grid.h"
#include "extras/script/cameraControls.h"
#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/gsplat/gsplatComponentSystem.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    struct GridPreset
    {
        float spacing;
        float subdivisions;
        bool infinite;
        float sizeX;
        float sizeZ;
        float axisLineThickness;
        float majorLineThickness;
        float minorLineThickness;
        GridTheme theme;
        float depthOffset;
        bool depthWrite;
        bool depthDither;
    };

    constexpr float kGrey = 136.0f / 255.0f;

    const std::array<GridPreset, 3> kPresets = {{
        {10.0f, 10.0f, true, 20.0f, 20.0f, 2.0f, 1.5f, 1.0f,
            GridTheme{Color(1.0f, 0.3f, 0.3f, 1.0f), Color(0.3f, 1.0f, 0.3f, 1.0f), Color(0.3f, 0.3f, 1.0f, 1.0f),
                Color(0.8f, 0.8f, 0.8f, 0.6f), Color(0.6f, 0.6f, 0.6f, 0.4f)},
            0.001f, false, false},
        {1.0f, 1.0f, false, 8.0f, 8.0f, 1.0f, 1.0f, 1.0f,
            GridTheme{Color(0.0f, 0.0f, 0.0f, 1.0f), Color(0.0f, 0.0f, 0.0f, 1.0f), Color(0.0f, 0.0f, 0.0f, 1.0f),
                Color(kGrey, kGrey, kGrey, 1.0f), Color(kGrey, kGrey, kGrey, 1.0f)},
            0.0f, false, false},
        {10.0f, 10.0f, true, 20.0f, 20.0f, 2.0f, 1.5f, 1.0f,
            GridTheme{Color(1.0f, 0.2f, 0.2f, 1.0f), Color(0.2f, 1.0f, 0.2f, 1.0f), Color(0.2f, 0.2f, 1.0f, 1.0f),
                Color(0.9f, 0.9f, 0.9f, 1.0f), Color(0.7f, 0.7f, 0.7f, 1.0f)},
            0.0f, true, true},
    }};

    // The degree-0 spherical-harmonic coefficient that displays as `colour`.
    float shDc(const float colour)
    {
        constexpr float kShC0 = 0.28209479177387814f;
        return (colour - 0.5f) / kShC0;
    }
}

class GridExample final : public ExampleApp
{
public:
    // Splats are expensive multisampled, and the grid antialiases itself.
    GridExample() : ExampleApp({.title = "Grid", .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<GSplatComponentSystem>();
    }

    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        const auto colorMaterial = [this](const Color& color) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(color);
            _materials.push_back(material);
            return material.get();
        };

        // Half through the grid.
        createPrimitive("box", colorMaterial(Color(0.8f, 1.0f, 1.0f, 1.0f)), Vector3(1.0f, 0.0f, 1.0f));
        _sphere = createPrimitive("sphere", colorMaterial(Color(1.0f, 0.8f, 1.0f, 1.0f)), Vector3(-1.0f, 0.0f, 1.0f));
        // On the grid, hiding it without a depth offset.
        createPrimitive("plane", colorMaterial(Color(0.35f, 0.35f, 0.3f, 1.0f)), Vector3(-1.0f, 0.0f, -1.0f),
            Vector3(1.5f, 1.0f, 1.5f));
        createPrimitive("capsule", colorMaterial(Color(0.8f, 0.8f, 1.0f, 1.0f)), Vector3(1.0f, 0.0f, -1.0f));
        createSplatSphere();

        _cameraEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _camera = _cameraEntity->findComponent<CameraComponent>();
        if (!_camera || !_camera->camera()) {
            spdlog::error("Failed to create camera");
            return false;
        }
        _camera->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        _camera->camera()->setFarClip(1000.0f);
        const float cameraOffset = 4.0f * _camera->camera()->aspectRatio();
        _cameraEntity->setPosition(Vector3(cameraOffset, cameraOffset, cameraOffset));
        if (auto* controls = addOrbitControls(_cameraEntity, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setRotateDamping(0.95f);
            controls->setMoveDamping(0.95f);
            controls->setZoomDamping(0.95f);
            controls->setPitchRange(Vector2(-89.999f, 89.999f));
            controls->setZoomRange(Vector2(0.5f, 500.0f));
            controls->setEnableFly(false);
            controls->storeResetState();
        }

        auto* light = new Entity();
        light->setEngine(engine());
        light->addComponent<LightComponent>();
        root()->addChild(light);
        light->setLocalEulerAngles(0.0f, 0.0f, -60.0f);

        // The grid, sized by its scale when not infinite.
        _gridEntity = new Entity();
        _gridEntity->setEngine(engine());
        _gridEntity->setName("grid");
        root()->addChild(_gridEntity);
        _grid = std::make_unique<Grid>(device());
        _grid->attach(_gridEntity);
        applyPreset(0);
        applyCaptureState();

        spdlog::info("Keys: 1 2 3 preset | V visible | G plane | [ ] spacing | - = subdivisions | I infinite | "
                     "A axes | C contrast | O depth offset | T W D depth test/write/dither | P projection | F far");
        return true;
    }

    void update(float /*dt*/) override
    {
        handleKeys();
    }

    void destroy() override
    {
        _grid.reset();
    }

private:
    void createSplatSphere()
    {
        constexpr size_t kCount = 6000;
        constexpr float kSize = 0.025f;
        const Vector3 lightDir = Vector3(0.5f, 0.8f, 0.3f).normalized();
        GSplatData::ActivatedSplats splats;
        splats.count = kCount;
        for (size_t i = 0; i < kCount; ++i) {
            // A Fibonacci spiral.
            const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(kCount);
            const float r = std::sqrt(1.0f - y * y);
            const float a = static_cast<float>(i) * std::numbers::pi_v<float> * (3.0f - std::sqrt(5.0f));
            const Vector3 n(std::cos(a) * r, y, std::sin(a) * r);

            // Coloured by direction, lit from one side.
            const float shade = 0.15f + 0.35f * std::max(n.dot(lightDir), 0.0f);
            splats.positions.insert(splats.positions.end(), {n.getX() * 0.5f, n.getY() * 0.5f, n.getZ() * 0.5f});
            splats.rotations.insert(splats.rotations.end(), {0.0f, 0.0f, 0.0f, 1.0f});
            splats.scales.insert(splats.scales.end(), {kSize, kSize, kSize});
            splats.opacities.push_back(1.0f);
            splats.sh0.insert(splats.sh0.end(), {shDc((0.55f + 0.45f * n.getX()) * shade),
                shDc((0.55f + 0.45f * n.getY()) * shade), shDc((0.55f + 0.45f * n.getZ()) * shade)});
        }
        auto data = GSplatData::fromActivated(splats, "splat-sphere");
        if (!data) {
            spdlog::error("grid: the splat sphere could not be built");
            return;
        }
        auto resource = std::make_shared<GSplatResource>(std::move(data), device());

        _splatSphere = new Entity();
        _splatSphere->setName("splat-sphere");
        _splatSphere->setEngine(engine());
        root()->addChild(_splatSphere);
        auto* gsplat = static_cast<GSplatComponent*>(_splatSphere->addComponent<GSplatComponent>());
        gsplat->setResource(resource);
        _splatSphere->setPosition(Vector3(-1.0f, 0.0f, 1.0f));
        _splatSphere->setEnabled(false);
    }

    void applyPreset(const int index)
    {
        _preset = index;
        const GridPreset& preset = kPresets[static_cast<size_t>(index)];
        _plane = GridPlane::XZ;
        _grid->setPlane(_plane);
        _grid->setSpacing(preset.spacing);
        _grid->setSubdivisions(preset.subdivisions);
        _grid->setInfinite(preset.infinite);
        _grid->setFadeRange(Vector2(400.0f, 1000.0f));
        _grid->setShowAxes(true);
        _grid->setAxisLineThickness(preset.axisLineThickness);
        _grid->setMajorLineThickness(preset.majorLineThickness);
        _grid->setMinorLineThickness(preset.minorLineThickness);
        setContrast(false);
        _grid->setTheme(preset.theme);
        _grid->setDepthOffset(preset.depthOffset);
        _grid->setDepthTest(true);
        _grid->setDepthWrite(preset.depthWrite);
        _grid->setDepthDither(preset.depthDither);
        applySize();

        // Splats only with the SuperSplat preset.
        const bool splats = index == 2 && _splatSphere;
        if (_splatSphere) {
            _splatSphere->setEnabled(splats);
        }
        _sphere->setEnabled(!splats);
    }

    void applyCaptureState()
    {
        const char* state = std::getenv("VISUTWIN_GRID_STATE");
        int preset = 0;
        int plane = 0;
        int ortho = 0;
        int contrast = 0;
        if (!state || std::sscanf(state, "%d,%d,%d,%d", &preset, &plane, &ortho, &contrast) < 1) {
            return;
        }
        applyPreset(std::clamp(preset, 0, 2));
        _plane = static_cast<GridPlane>(std::clamp(plane, 0, 2));
        _grid->setPlane(_plane);
        applySize();
        if (ortho) {
            _camera->camera()->setProjection(ProjectionType::Orthographic);
        }
        setContrast(contrast != 0);
    }

    // The size is the entity's scale on the plane's axes.
    void applySize()
    {
        const GridPreset& preset = kPresets[static_cast<size_t>(_preset)];
        const float w = preset.sizeX;
        const float d = preset.sizeZ;
        _gridEntity->setLocalScale(_plane == GridPlane::YZ ? 1.0f : w,
            _plane == GridPlane::XZ ? 1.0f : _plane == GridPlane::XY ? d : w,
            _plane == GridPlane::XY ? 1.0f : d);
    }

    // The scene colour map request is counted, so it is made and released once each.
    void setContrast(const bool value)
    {
        if (value != _grid->contrast()) {
            _camera->requestSceneColorMap(value);
        }
        _grid->setContrast(value);
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        Camera* camera = _camera ? _camera->camera() : nullptr;
        if (!keyboard || !_grid || !camera) {
            return;
        }
        if (keyboard->wasPressed(Key::Digit1)) applyPreset(0);
        if (keyboard->wasPressed(Key::Digit2)) applyPreset(1);
        if (keyboard->wasPressed(Key::Digit3)) applyPreset(2);
        if (keyboard->wasPressed(Key::V)) _grid->setVisible(!_grid->visible());
        if (keyboard->wasPressed(Key::G)) {
            _plane = static_cast<GridPlane>((static_cast<int>(_plane) + 1) % 3);
            _grid->setPlane(_plane);
            applySize();
        }
        if (keyboard->wasPressed(Key::LeftBracket)) _grid->setSpacing(std::max(_grid->spacing() * 0.5f, 0.1f));
        if (keyboard->wasPressed(Key::RightBracket)) _grid->setSpacing(std::min(_grid->spacing() * 2.0f, 50.0f));
        if (keyboard->wasPressed(Key::Minus)) _grid->setSubdivisions(std::max(_grid->subdivisions() - 1.0f, 1.0f));
        if (keyboard->wasPressed(Key::Equals)) _grid->setSubdivisions(std::min(_grid->subdivisions() + 1.0f, 10.0f));
        if (keyboard->wasPressed(Key::I)) _grid->setInfinite(!_grid->infinite());
        if (keyboard->wasPressed(Key::A)) _grid->setShowAxes(!_grid->showAxes());
        if (keyboard->wasPressed(Key::C)) setContrast(!_grid->contrast());
        if (keyboard->wasPressed(Key::O)) {
            constexpr float offsets[] = {0.001f, 0.0f, 0.01f};
            _depthOffsetIndex = (_depthOffsetIndex + 1) % 3;
            _grid->setDepthOffset(offsets[_depthOffsetIndex]);
        }
        if (keyboard->wasPressed(Key::T)) _grid->setDepthTest(!_grid->depthTest());
        if (keyboard->wasPressed(Key::W)) _grid->setDepthWrite(!_grid->depthWrite());
        if (keyboard->wasPressed(Key::D)) _grid->setDepthDither(!_grid->depthDither());
        if (keyboard->wasPressed(Key::P)) {
            camera->setProjection(camera->projection() == ProjectionType::Perspective
                ? ProjectionType::Orthographic : ProjectionType::Perspective);
        }
        if (keyboard->wasPressed(Key::F)) {
            constexpr float fars[] = {1000.0f, 100.0f, 2000.0f};
            _farIndex = (_farIndex + 1) % 3;
            camera->setFarClip(fars[_farIndex]);
        }
    }

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::unique_ptr<Grid> _grid;
    Entity* _gridEntity = nullptr;
    Entity* _cameraEntity = nullptr;
    CameraComponent* _camera = nullptr;
    Entity* _sphere = nullptr;
    Entity* _splatSphere = nullptr;
    GridPlane _plane = GridPlane::XZ;
    int _preset = 0;
    int _depthOffsetIndex = 0;
    int _farIndex = 0;
};

VISUTWIN_EXAMPLE_MAIN(GridExample)
