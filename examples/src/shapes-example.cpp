// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/shapes.
//
// Every built-in primitive (box, plane, cone, cylinder, sphere, capsule) and a rounded box built
// from RoundedBoxGeometry stand on a street in two rows, the front row first, each in its own
// metal (silver, gold, copper, anodised aluminium in blue, red and green; metalness 0.9, gloss
// 0.9). The wide-street HDRI is the skybox, projected onto a dome (scale 30, tripod at 0.05) so
// the shapes stand on its ground, and the environment lighting (exposure 0.7). A warm directional
// light casts soft PCSS shadows, caught on the dome's ground by a shadow catcher. The camera (fov
// 45, ACES) orbits (0, 0.5, 0) from (0, 3, 7), pitch -89 to -5, distance 2 to 20.
//
// A UV test texture can be applied to every shape: an 8x8 grid of 64-pixel cells whose hue
// changes along u and whose lightness alternates like a checkerboard, each cell labelled with its
// column and row, so the orientation and any stretching of the UVs are easy to see. Each shape can
// show its wireframe: a child entity drawing the shape's mesh in the wireframe style
// (RENDERSTYLE_WIREFRAME) with an unlit white material, scaled up a little so the surface does not
// hide its lines.
//
// Keys standing in for upstream's controls panel:
//   T      texture on / off (off to begin with)
//   W      wireframe on / off (off to begin with)
//   1 / 2  metalness -/+ 0.05, 0..1
//   3 / 4  gloss -/+ 0.05, 0..1
//   [ / ]  rounded box radius -/+ 0.02, 0..0.5 (0.2 to begin with)
//   - / =  rounded box segments -/+ 1, 2..16 (8 to begin with)
//
// DEVIATION: upstream draws the cell labels with the browser's bold sans-serif font. There is
// no text rasteriser here, so the labels are drawn with a small embedded 5x7 bitmap font of the
// digits and the comma, scaled by 2.
// DEVIATION: upstream gives the texture an anisotropy of 8; a texture here has no anisotropy
// setting of its own (the samplers take the device's).
// DEVIATION: upstream caps the pixel ratio at 2; the examples render at one pixel per point.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/script/shadowCatcher.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/graphics/envLighting.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kCells = 8;
    constexpr int kCellSize = 64;
    constexpr int kTextureSize = kCells * kCellSize;

    // Each glyph is 7 rows of a fixed width, 'X' for an inked pixel.
    struct Glyph
    {
        int width;
        std::array<const char*, 7> rows;
    };

    const std::array<Glyph, 8> kDigits{{
        {5, {".XXX.", "X...X", "X..XX", "X.X.X", "XX..X", "X...X", ".XXX."}},
        {5, {"..X..", ".XX..", "..X..", "..X..", "..X..", "..X..", ".XXX."}},
        {5, {".XXX.", "X...X", "....X", "...X.", "..X..", ".X...", "XXXXX"}},
        {5, {"XXXXX", "...X.", "..X..", "...X.", "....X", "X...X", ".XXX."}},
        {5, {"...X.", "..XX.", ".X.X.", "X..X.", "XXXXX", "...X.", "...X."}},
        {5, {"XXXXX", "X....", "XXXX.", "....X", "....X", "X...X", ".XXX."}},
        {5, {"..XX.", ".X...", "X....", "XXXX.", "X...X", "X...X", ".XXX."}},
        {5, {"XXXXX", "....X", "...X.", "..X..", ".X...", ".X...", ".X..."}},
    }};

    const Glyph kComma{2, {"..", "..", "..", "..", "XX", ".X", "X."}};

    constexpr int kGlyphScale = 2;
    constexpr int kGlyphSpacing = 1;

    // CSS hsl() to 8-bit RGB: hue in degrees, saturation and lightness in [0, 1].
    std::array<uint8_t, 3> hslToRgb(const float hue, const float saturation, const float lightness)
    {
        const float chroma = (1.0f - std::abs(2.0f * lightness - 1.0f)) * saturation;
        const float h = hue / 60.0f;
        const float x = chroma * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (h < 1.0f) {
            r = chroma; g = x;
        } else if (h < 2.0f) {
            r = x; g = chroma;
        } else if (h < 3.0f) {
            g = chroma; b = x;
        } else if (h < 4.0f) {
            g = x; b = chroma;
        } else if (h < 5.0f) {
            r = x; b = chroma;
        } else {
            r = chroma; b = x;
        }
        const float m = lightness - chroma * 0.5f;
        const auto toByte = [m](const float value) {
            return static_cast<uint8_t>(std::lround((value + m) * 255.0f));
        };
        return {toByte(r), toByte(g), toByte(b)};
    }

    // The UV grid, row 0 at the top as the canvas it stands in for.
    std::vector<uint8_t> paintUvGrid()
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(kTextureSize) * kTextureSize * 4);
        const auto setPixel = [&pixels](const int x, const int y, const std::array<uint8_t, 3>& rgb) {
            if (x < 0 || y < 0 || x >= kTextureSize || y >= kTextureSize) {
                return;
            }
            uint8_t* pixel = &pixels[(static_cast<size_t>(y) * kTextureSize + x) * 4];
            pixel[0] = rgb[0];
            pixel[1] = rgb[1];
            pixel[2] = rgb[2];
            pixel[3] = 255;
        };

        for (int column = 0; column < kCells; ++column) {
            for (int row = 0; row < kCells; ++row) {
                const auto fill = hslToRgb(static_cast<float>(column * 360) / kCells, 0.7f,
                    (column + row) % 2 ? 0.45f : 0.65f);
                for (int y = 0; y < kCellSize; ++y) {
                    for (int x = 0; x < kCellSize; ++x) {
                        setPixel(column * kCellSize + x, row * kCellSize + y, fill);
                    }
                }

                // The "column,row" label, centred in the cell, in black
                const std::array<const Glyph*, 3> label{&kDigits[column], &kComma, &kDigits[row]};
                int textWidth = -kGlyphSpacing;
                for (const Glyph* glyph : label) {
                    textWidth += glyph->width + kGlyphSpacing;
                }
                int penX = column * kCellSize + (kCellSize - textWidth * kGlyphScale) / 2;
                const int top = row * kCellSize + (kCellSize - 7 * kGlyphScale) / 2;
                for (const Glyph* glyph : label) {
                    for (int gy = 0; gy < 7; ++gy) {
                        for (int gx = 0; gx < glyph->width; ++gx) {
                            if (glyph->rows[gy][gx] != 'X') {
                                continue;
                            }
                            for (int sy = 0; sy < kGlyphScale; ++sy) {
                                for (int sx = 0; sx < kGlyphScale; ++sx) {
                                    setPixel(penX + gx * kGlyphScale + sx, top + gy * kGlyphScale + sy, {0, 0, 0});
                                }
                            }
                        }
                    }
                    penX += (glyph->width + kGlyphSpacing) * kGlyphScale;
                }
            }
        }
        return pixels;
    }
}

namespace
{
    struct ShapeSpec
    {
        const char* type;   // a render component primitive, or "rounded-box"
        Color color;
    };
}

class ShapesExample final: public ExampleApp
{
public:
    ShapesExample(): ExampleApp({.title = "Shapes"}) {}

protected:
    bool create() override
    {
        _hdriAsset = std::make_unique<Asset>("hdri", AssetType::TEXTURE, assetPath("hdri/wide-street.hdr"),
            AssetData{.mipmaps = false});
        Texture* hdri = _hdriAsset->resourceAs<Texture>();
        if (!hdri) {
            spdlog::error("Failed to load the wide-street HDRI");
            return false;
        }

        // Use the HDRI for the skybox, and for the lighting and reflections
        _skybox.reset(EnvLighting::generateSkyboxCubemap(device().get(), hdri));
        _envAtlas.reset(EnvLighting::generateAtlas(device().get(), hdri));
        scene()->setSkybox(_skybox.get());
        scene()->setEnvAtlas(_envAtlas.get());

        // Project the skybox onto a dome, so the shapes stand on the street
        scene()->setExposure(0.7f);
        scene()->setSkyType(SKYTYPE_DOME);
        scene()->sky()->node()->setLocalScale(Vector3(30.0f, 30.0f, 30.0f));
        scene()->sky()->setCenter(Vector3(0.0f, 0.05f, 0.0f));

        // The UV test texture: mipmapped, its mips generated on upload
        TextureOptions options;
        options.name = "uv-grid";
        options.width = kTextureSize;
        options.height = kTextureSize;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = true;
        _uvTexture = std::make_shared<Texture>(device().get(), options);
        const std::vector<uint8_t> pixels = paintUvGrid();
        _uvTexture->setLevelData(0, pixels.data(), pixels.size());
        _uvTexture->upload();

        // An unlit material for the wireframes
        _wireframeMaterial = std::make_shared<StandardMaterial>();
        _wireframeMaterial->setUseLighting(false);
        _wireframeMaterial->setUseSkybox(false);
        _wireframeMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _wireframeMaterial->setEmissive(Color(0.9f, 0.9f, 0.9f, 1.0f));

        // The shapes in two rows, the front one first, each in its own metal. The rounded box is
        // not a primitive shape type, so its mesh is generated from a RoundedBoxGeometry.
        const std::vector<std::vector<ShapeSpec>> rows = {
            {{"box", Color(0.95f, 0.94f, 0.91f)}, {"rounded-box", Color(1.0f, 0.77f, 0.34f)},
             {"plane", Color(0.95f, 0.64f, 0.54f)}, {"cone", Color(0.25f, 0.45f, 0.9f)}},
            {{"cylinder", Color(0.85f, 0.2f, 0.25f)}, {"sphere", Color(0.95f, 0.94f, 0.91f)},
             {"capsule", Color(0.25f, 0.7f, 0.45f)}},
        };
        for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const auto& row = rows[rowIndex];
            for (size_t index = 0; index < row.size(); ++index) {
                const std::string type = row[index].type;
                const bool rounded = type == "rounded-box";
                auto material = std::make_shared<StandardMaterial>();
                material->setUseMetalness(true);
                material->setDiffuse(row[index].color);
                _materials.push_back(material);

                Entity* entity;
                if (rounded) {
                    entity = new Entity();
                    entity->setEngine(engine());
                    entity->setName("rounded-box");
                    entity->addComponent<RenderComponent>();
                    root()->addChild(entity);
                    _roundedBox = entity;
                    _roundedBoxMaterial = material;
                } else {
                    entity = createPrimitive(type.c_str(), material.get());
                    entity->setName(type);
                }

                // The wireframe is drawn over the shape, scaled up a little so the surface does not
                // hide its lines, which works as all the shapes are convex. It is also lifted a
                // little for the plane.
                auto* wireframe = new Entity();
                wireframe->setEngine(engine());
                wireframe->setName("wireframe");
                auto* render = static_cast<RenderComponent*>(wireframe->addComponent<RenderComponent>());
                render->setCastShadows(false);
                render->setRenderStyle(RENDERSTYLE_WIREFRAME);
                wireframe->setLocalScale(1.002f, 1.002f, 1.002f);
                wireframe->setLocalPosition(0.0f, 0.002f, 0.0f);
                entity->addChild(wireframe);
                _wireframes.push_back(wireframe);
                if (rounded) {
                    _roundedBoxWireframe = wireframe;
                    updateRoundedBox();
                } else {
                    auto* shape = entity->findComponent<RenderComponent>();
                    render->addMeshInstance(std::make_unique<MeshInstance>(shape->meshInstances()[0]->mesh(),
                        _wireframeMaterial.get(), wireframe));
                }

                // Stand the shape on the ground, centring each row. The plane is lifted a little, to
                // stay above the ground of the dome and the shadow catcher.
                const auto* mesh = entity->findComponent<RenderComponent>()->meshInstances()[0]->mesh();
                const float minY = mesh->aabb().center().getY() - mesh->aabb().halfExtents().getY();
                entity->setLocalPosition((static_cast<float>(index) - static_cast<float>(row.size() - 1) / 2.0f) * 1.6f,
                    std::max(-minY, 0.02f), rowIndex ? -1.0f : 1.0f);
            }
        }
        applyMaterials();
        applyWireframe();

        // A directional light casting soft shadows
        Entity* light = createDirectionalLight(Vector3(40.0f, -50.0f, 0.0f), Color(1.0f, 0.95f, 0.85f, 1.0f), 1.5f, true);
        if (auto* component = light->findComponent<LightComponent>()) {
            component->setShadowBias(0.1f);
            component->setShadowNormalBias(0.1f);
            component->setShadowDistance(15.0f);
            component->setShadowResolution(2048);
            component->setShadowType(SHADOW_PCSS_32F);
            component->setPenumbraSize(0.05f);
            component->setPenumbraFalloff(4.0f);
            component->setShadowSamples(16);
            component->setShadowBlockerSamples(16);
        }

        // Catch the shadows on the ground of the dome
        auto* shadowCatcher = new Entity();
        shadowCatcher->setEngine(engine());
        shadowCatcher->setName("shadow-catcher");
        shadowCatcher->addComponent<ScriptComponent>();
        shadowCatcher->script()->create<ShadowCatcher>()->setPlaneScale(20.0f);
        shadowCatcher->setLocalPosition(0.0f, 0.01f, 0.0f);
        root()->addChild(shadowCatcher);

        // A camera orbiting the shapes
        auto* camera = createCamera(Vector3(0.0f, 3.0f, 7.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setFov(45.0f);
            component->setToneMapping(TONEMAP_ACES);
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.5f, 0.0f))) {
            controls->setPitchRange(Vector2(-89.0f, -5.0f));
            controls->setZoomRange(Vector2(2.0f, 20.0f));
            controls->storeResetState();
        }

        spdlog::info("Keys: T texture, W wireframe, 1/2 metalness, 3/4 gloss, [ ] radius, - = segments");
        return true;
    }

    void update(float /*dt*/) override
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        if (keyboard->wasPressed(Key::T)) {
            _textured = !_textured;
            applyMaterials();
        } else if (keyboard->wasPressed(Key::W)) {
            _wireframe = !_wireframe;
            applyWireframe();
        } else if (keyboard->wasPressed(Key::Digit1) || keyboard->wasPressed(Key::Digit2)) {
            _metalness = std::clamp(_metalness + (keyboard->wasPressed(Key::Digit2) ? 0.05f : -0.05f), 0.0f, 1.0f);
            applyMaterials();
        } else if (keyboard->wasPressed(Key::Digit3) || keyboard->wasPressed(Key::Digit4)) {
            _gloss = std::clamp(_gloss + (keyboard->wasPressed(Key::Digit4) ? 0.05f : -0.05f), 0.0f, 1.0f);
            applyMaterials();
        } else if (keyboard->wasPressed(Key::LeftBracket) || keyboard->wasPressed(Key::RightBracket)) {
            _radius = std::clamp(_radius + (keyboard->wasPressed(Key::RightBracket) ? 0.02f : -0.02f), 0.0f, 0.5f);
            updateRoundedBox();
        } else if (keyboard->wasPressed(Key::Minus) || keyboard->wasPressed(Key::Equals)) {
            _radiusSegments = std::clamp(_radiusSegments + (keyboard->wasPressed(Key::Equals) ? 1 : -1), 2, 16);
            updateRoundedBox();
        } else {
            return;
        }
        spdlog::info("texture {} wireframe {} metalness {:.2f} gloss {:.2f} radius {:.2f} segments {}",
            _textured, _wireframe, _metalness, _gloss, _radius, _radiusSegments);
    }

private:
    // Regenerate the rounded box mesh from its settings. The box and its wireframe co-own the mesh,
    // so replacing both mesh instances frees the previous one.
    void updateRoundedBox()
    {
        RoundedBoxGeometryOptions geometry;
        geometry.radius = _radius;
        geometry.radiusSegments = _radiusSegments;
        std::shared_ptr<Mesh> mesh = createMeshFromGeometry(device(), createRoundedBoxGeometry(geometry));
        auto* box = _roundedBox->findComponent<RenderComponent>();
        auto* wireframe = _roundedBoxWireframe->findComponent<RenderComponent>();
        box->clearMeshInstances();
        wireframe->clearMeshInstances();
        box->addMeshInstance(std::make_unique<MeshInstance>(mesh, _roundedBoxMaterial, _roundedBox));
        wireframe->addMeshInstance(std::make_unique<MeshInstance>(mesh, _wireframeMaterial, _roundedBoxWireframe));
    }

    // Apply the material settings to all shapes
    void applyMaterials() const
    {
        for (const auto& material : _materials) {
            material->setDiffuseMap(_textured ? _uvTexture.get() : nullptr);
            material->setMetalness(_metalness);
            material->setGloss(_gloss);
        }
    }

    // Show the wireframes when the wireframe toggle is on
    void applyWireframe() const
    {
        for (Entity* wireframe : _wireframes) {
            wireframe->setEnabled(_wireframe);
        }
    }

    std::unique_ptr<Asset> _hdriAsset;
    std::unique_ptr<Texture> _skybox;
    std::unique_ptr<Texture> _envAtlas;
    std::shared_ptr<Texture> _uvTexture;
    std::shared_ptr<StandardMaterial> _wireframeMaterial;
    std::shared_ptr<StandardMaterial> _roundedBoxMaterial;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Entity*> _wireframes;
    Entity* _roundedBox = nullptr;
    Entity* _roundedBoxWireframe = nullptr;

    bool _textured = false;
    bool _wireframe = false;
    float _metalness = 0.9f;
    float _gloss = 0.9f;
    float _radius = 0.2f;
    int _radiusSegments = 8;
};

VISUTWIN_EXAMPLE_MAIN(ShapesExample)
