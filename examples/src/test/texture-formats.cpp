// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream test/texture-formats (a hidden test example).
//
// One texture per file format, to exercise each texture reader: the PlayCanvas logo as PNG, DDS
// (DXT1) and KTX2 (Basis Universal, ETC1S) in the top row, and the seaside rocks colour texture
// (Basis) and the St Peter's Square HDRI in the bottom row, each shown in a tile of a fifth of the
// window's width and three tenths of its height over a camera clearing to (0.1, 0.1, 0.1). The three
// logos must look alike. A DDS block format the device cannot sample is decoded to RGBA8 as it loads.
//
// DEVIATIONS:
// - The Basis tile reads the KTX2 re-encode of seaside-rocks01-color.basis: .basis files do not load
//   here (libktx reads KTX2 only), as for texture-basis.
// - The previews are TexturePreviewPasses (examples/texturePreview.h), upstream's TextureRenderer;
//   the HDR tile, a float texture, is gamma-encoded as that renderer does.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <array>
#include <memory>
#include <vector>

#include "exampleApp.h"
#include "texturePreview.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/texture.h"

using namespace visutwin::canvas;

class TextureFormatsExample final : public ExampleApp
{
public:
    TextureFormatsExample() : ExampleApp({}) {}

protected:
    bool create() override
    {
        struct Tile
        {
            const char* name;
            const char* path;
            float x;
            float y;
        };
        // Grid layout in normalized viewport coordinates, one tile per format
        constexpr std::array<Tile, 5> tiles{{
            {"png", "textures/playcanvas.png", 0.15f, 0.14f},
            {"dds", "textures/playcanvas.dds", 0.4f, 0.14f},
            {"ktx2", "textures/playcanvas.ktx2", 0.65f, 0.14f},
            {"basis", "textures/seaside-rocks01-color.ktx2", 0.275f, 0.56f},
            {"hdr", "hdri/st-peters-square.hdr", 0.525f, 0.56f},
        }};
        for (const auto& tile : tiles) {
            auto asset = std::make_unique<Asset>(tile.name, AssetType::TEXTURE, assetPath(tile.path));
            Texture* texture = asset->resourceAs<Texture>();
            if (!texture) {
                spdlog::error("texture-formats: {} did not load", tile.path);
                return false;
            }
            auto preview = std::make_shared<TexturePreviewPass>(device(), texture);
            preview->init(nullptr);
            engine()->renderer()->addAppendPass(preview);
            _previews.push_back({preview, tile.x, tile.y});
            _assets.push_back(std::move(asset));
        }

        // A camera is required to render the texture previews
        Entity* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        return true;
    }

    void update(float /*dt*/) override
    {
        const auto [width, height] = device()->size();
        const auto fw = static_cast<float>(width);
        const auto fh = static_cast<float>(height);
        for (const auto& [preview, x, y] : _previews) {
            preview->setViewport(Vector4(x * fw, y * fh, 0.2f * fw, 0.3f * fh));
            preview->setScissor(preview->viewport());
        }
    }

    void destroy() override
    {
        // The passes are registered with the renderer, so they go while the engine is alive.
        if (engine() && engine()->renderer()) {
            for (const auto& placed : _previews) {
                engine()->renderer()->removeAppendPass(placed.preview);
            }
        }
        _previews.clear();
    }

private:
    struct PlacedPreview
    {
        std::shared_ptr<TexturePreviewPass> preview;
        float x;
        float y;
    };

    std::vector<std::unique_ptr<Asset>> _assets;
    std::vector<PlacedPreview> _previews;
};

VISUTWIN_EXAMPLE_MAIN(TextureFormatsExample)
