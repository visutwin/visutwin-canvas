// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 20.11.2026
//
// Port of upstream loaders/compressed-meshes.
//
// The Stanford dragon three times in a row: uncompressed, compressed with meshopt
// (EXT_meshopt_compression with KHR_mesh_quantization) and with Draco
// (KHR_draco_mesh_compression), spaced by 1.35 of the model's width. Under each hangs a
// screen-space label with the format's name in Roboto Bold and, in Roboto Regular, the
// file's size and the vertices and triangles it decoded to. The label sits under the
// lowest bottom corner of its model's bounds on screen, so it clears the model from any
// angle, and hides when the model is behind the camera. The scene is lit by the helipad
// environment (sky at mip 1, intensity 0.4) and a directional light of intensity 1.5;
// an orbit camera with a 60-degree HORIZONTAL field of view and ACES tone mapping looks
// at the row from (0, centre + width, 3.6 widths). On a portrait window the labels use a
// portrait reference resolution.
//
// DEVIATIONS:
// - The engine decodes Draco and meshopt natively in the glTF parser, so upstream's two
//   decoder modules have no counterpart, and a file's size is read from the file system
//   where upstream measures the bytes it fetched.
// - worldToScreen answers in window points (the canvas here), so a label's screen units
//   are points over the screen's scale, where upstream converts CSS pixels by the pixel
//   ratio.
// - The numbers are grouped with commas, where upstream uses the browser's locale.
// - The back buffer renders at one pixel per point, as every example here does, where
//   upstream caps the device pixel ratio at 2.
//
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "exampleApp.h"
#include "uiElements.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "core/shape/boundingBox.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/screen/screenComponent.h"
#include "scene/camera.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    struct ModelDef
    {
        const char* name;
        const char* path;
    };

    // The same model in each format, in the order they stand in the scene.
    constexpr std::array<ModelDef, 3> kModels{{
        {"Uncompressed", "models/stanford-dragon.glb"},
        {"Meshopt", "models/stanford-dragon-meshopt.glb"},
        {"Draco", "models/stanford-dragon-draco.glb"},
    }};

    const Color DARK(0.05f, 0.05f, 0.07f, 1.0f);

    /// `value` with its thousands grouped: 1234567 -> "1,234,567".
    std::string grouped(const uint64_t value)
    {
        std::string text = std::to_string(value);
        for (int at = static_cast<int>(text.size()) - 3; at > 0; at -= 3) {
            text.insert(static_cast<size_t>(at), ",");
        }
        return text;
    }

    struct Placed
    {
        Entity* entity = nullptr;
        Entity* label = nullptr;
    };
}

class CompressedMeshesExample final: public ExampleApp
{
public:
    CompressedMeshesExample(): ExampleApp({}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _bold = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _regular = std::make_unique<Asset>("regular", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        Texture* helipad = _helipad->resourceAs<Texture>();
        FontResource* bold = _bold->resourceAs<FontResource>();
        FontResource* regular = _regular->resourceAs<FontResource>();
        if (!helipad || !bold || !regular) {
            spdlog::error("Failed to load cubemaps/helipad-env-atlas.png or the Roboto fonts");
            return false;
        }

        scene()->setEnvAtlas(helipad);
        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(0.4f);
        scene()->setExposure(1.0f);

        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.5f);

        // The screen the labels are on
        _screen = createScreen();
        Entity* screenEntity = _screen->entity();

        // Elements are centred on their parent unless a call says otherwise.
        const ElementProps centred{.anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f)};

        // Place the models side by side, each with a label under it
        for (const ModelDef& model : kModels) {
            const std::string path = assetPath(model.path);
            auto& asset = _models.emplace_back(
                std::make_unique<Asset>(model.name, AssetType::CONTAINER, path));
            ContainerResource* container = asset->resourceAs<ContainerResource>();
            Entity* entity = container ? container->instantiateRenderEntity() : nullptr;
            if (!entity) {
                spdlog::error("Failed to load {}", model.path);
                return false;
            }
            entity->setEngine(engine());
            root()->addChild(entity);
            if (_placed.empty()) {
                _bounds = entityBounds(entity);
            }

            // What the file decoded to
            uint64_t vertices = 0;
            uint64_t triangles = 0;
            for (auto* render : entity->findComponents<RenderComponent>()) {
                for (const MeshInstance* meshInstance : render->meshInstances()) {
                    const Mesh* mesh = meshInstance ? meshInstance->mesh() : nullptr;
                    if (!mesh) {
                        continue;
                    }
                    if (const auto vertexBuffer = mesh->getVertexBuffer()) {
                        vertices += static_cast<uint64_t>(vertexBuffer->numVertices());
                    }
                    triangles += static_cast<uint64_t>(mesh->getPrimitive().count / 3);
                }
            }
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            const double megabytes = error ? 0.0 : static_cast<double>(size) / (1024.0 * 1024.0);
            spdlog::info("{}: {:.2f} MB, {} vertices, {} triangles", model.name, megabytes, vertices, triangles);

            // Anchored to the screen's bottom-left corner, with its pivot mid top edge so it
            // hangs below its point
            ElementComponent* label = createElement(engine(), screenEntity,
                {.type = ElementType::Group, .anchor = Vector4(0.0f, 0.0f, 0.0f, 0.0f), .pivot = Vector2(0.5f, 1.0f),
                 .width = 220.0f, .height = 96.0f}, centred);
            label->entity()->setName(std::string(model.name) + " label");

            ElementComponent* name = createElement(engine(), label->entity(),
                {.type = ElementType::Text, .anchor = Vector4(0.5f, 1.0f, 0.5f, 1.0f), .pivot = Vector2(0.5f, 1.0f),
                 .font = bold, .text = model.name, .fontSize = 24}, centred);
            name->setOutlineColor(DARK);
            name->setOutlineThickness(0.5f);

            ElementComponent* details = createElement(engine(), label->entity(),
                {.type = ElementType::Text, .color = Color(0.85f, 0.85f, 0.85f, 1.0f),
                 .anchor = Vector4(0.5f, 1.0f, 0.5f, 1.0f), .pivot = Vector2(0.5f, 1.0f), .font = regular,
                 .text = std::format("{:.2f} MB\n{} vertices\n{} triangles", megabytes, grouped(vertices),
                     grouped(triangles)),
                 .fontSize = 16, .lineHeight = 20.0f, .horizontalAlign = ElementHorizontalAlign::Center,
                 .verticalAlign = 0.0f}, centred);
            details->setOutlineColor(DARK);
            details->setOutlineThickness(0.5f);
            details->entity()->setLocalPosition(0.0f, -30.0f, 0.0f);

            _placed.push_back({entity, label->entity()});
        }

        // Space the models by their width, centred on the origin
        const float width = _bounds.halfExtents().getX() * 2.0f;
        const float spacing = width * 1.35f;
        for (size_t i = 0; i < _placed.size(); ++i) {
            _placed[i].entity->setLocalPosition((static_cast<float>(i) - 1.0f) * spacing, 0.0f, 0.0f);
        }

        const Vector3 center(0.0f, _bounds.center().getY(), 0.0f);
        Entity* cameraEntity = createCamera(Vector3(0.0f, center.getY() + width, width * 3.6f));
        _camera = cameraEntity->findComponent<CameraComponent>();
        if (!_camera || !_camera->camera()) {
            return false;
        }
        // A horizontal field of view keeps the row in view on any aspect ratio
        _camera->camera()->setFov(60.0f);
        _camera->camera()->setHorizontalFov(true);
        _camera->camera()->setNearClip(width * 0.01f);
        _camera->camera()->setFarClip(width * 100.0f);
        _camera->setToneMapping(TONEMAP_ACES);

        if (CameraControls* controls = addOrbitControls(cameraEntity, center)) {
            controls->setMoveSpeed(width * 2.0f);
        }

        layout();
        placeLabels();
        return true;
    }

    void update(const float /*dt*/) override
    {
        layout();
        placeLabels();
    }

private:
    // Hang each label under its model.
    void placeLabels()
    {
        const auto [canvasWidth, canvasHeight] = engine()->canvasSize();
        (void)canvasWidth;
        const float units = 1.0f / std::max(_screen->scale(), 1e-6f);
        const float halfX = _bounds.halfExtents().getX();
        const float halfZ = _bounds.halfExtents().getZ();
        const float bottom = _bounds.center().getY() - _bounds.halfExtents().getY();
        const Matrix4 view = _camera->entity()->worldTransform().inverse();
        for (const Placed& placed : _placed) {
            const Vector3 position = placed.entity->position();
            const float x = position.getX();
            const float z = position.getZ();

            // Under the lowest bottom corner on screen, so the label clears the model from
            // any angle
            float lowest = 0.0f;
            for (const float dx : {-halfX, halfX}) {
                for (const float dz : {-halfZ, halfZ}) {
                    lowest = std::max(lowest, _camera->worldToScreen(Vector3(x + dx, bottom, z + dz)).getY());
                }
            }
            const Vector3 point(x, bottom, z);
            const Vector3 onCanvas = _camera->worldToScreen(point);

            // Hide the label when its model is behind the camera
            placed.label->setEnabled(view.transformPoint(point).getZ() < 0.0f);
            placed.label->setLocalPosition(onCanvas.getX() * units,
                (static_cast<float>(canvasHeight) - lowest) * units - 8.0f, 0.0f);
        }
    }

    // Portrait canvases get a portrait reference resolution for the labels
    void layout()
    {
        const auto [w, h] = engine()->canvasSize();
        const bool portrait = h > w;
        if (portrait == _portrait && _laidOut) {
            return;
        }
        _portrait = portrait;
        _laidOut = true;
        _screen->setReferenceResolution(portrait ? Vector2(540.0f, 960.0f) : Vector2(1280.0f, 720.0f));
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _bold;
    std::unique_ptr<Asset> _regular;
    std::vector<std::unique_ptr<Asset>> _models;
    std::vector<Placed> _placed;
    BoundingBox _bounds;
    CameraComponent* _camera = nullptr;
    ScreenComponent* _screen = nullptr;
    bool _portrait = false;
    bool _laidOut = false;
};

VISUTWIN_EXAMPLE_MAIN(CompressedMeshesExample)
