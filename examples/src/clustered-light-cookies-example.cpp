// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream graphics/clustered-light-cookies.
//
// Dynamic cookie textures under clustered lighting. The chess board, at a quarter of
// its authored size and neither casting nor receiving shadows, is lit only by four
// unshadowed spot lights, one 20 units above the centre of each quadrant and aimed
// straight down. Two of them (warm orange) project a procedural 256x256 cookie of
// moving rings and spokes, regenerated every 10 frames; the other two (white) project
// a stand-in for a video, a 16:9 animated test pattern uploaded every frame. Every
// clustered cookie lives in one shared cookie atlas (2048), which is drawn in the
// bottom-left corner so the copies into it can be seen directly.
//
// Keys: upstream has no control panel. The orbit camera takes the mouse (drag to
// orbit, wheel to zoom) and R resets the view.
//
// DEVIATIONS:
// - upstream's second cookie is a video (a 1280x720 MP4 played through an HTML
//   video element). This engine has no video decoding and the repository carries no
//   video, so that cookie is an animated 320x180 test pattern generated on the CPU
//   and uploaded every frame, advanced by the frame time.
// - `textures.draw(cookieAtlas, 0.05, 0.75, 0.2, 0.2)` has no immediate equivalent:
//   the atlas preview is a RenderPassDownsample appended to the frame graph with the
//   same viewport-fraction rectangle, so it draws over everything, once the atlas
//   exists (the first frame with a cookie light creates it).
// - orbitCamera is CameraControls around the board's bounds centre, from the
//   camera's authored position; distanceMax 200 becomes the zoom range, and
//   CameraControls has no inertia factor.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassDownsample.h"
#include "scene/lighting/lightTextureAtlas.h"
#include "scene/renderer/forwardRenderer.h"

using namespace visutwin::canvas;

namespace
{
    // The procedural cookie is square; the video stand-in keeps the video's 16:9.
    constexpr uint32_t kCookieSize = 256;
    constexpr uint32_t kVideoWidth = 320;
    constexpr uint32_t kVideoHeight = 180;

    // Each spot sits this far from the board centre on X and Z, this high above it.
    constexpr float kQuadrantOffset = 11.0f;
    constexpr float kLightHeight = 20.0f;

    // The atlas preview: left and top edge, width and height, as fractions of the
    // window.
    constexpr float kPreviewX = 0.05f;
    constexpr float kPreviewY = 0.75f;
    constexpr float kPreviewW = 0.2f;
    constexpr float kPreviewH = 0.2f;

    struct Quadrant
    {
        float x;
        float z;
        bool procedural;
        Color color;
    };

    const std::array<Quadrant, 4> kQuadrants = {{
        {1.0f, 1.0f, true, Color(1.0f, 0.6f, 0.3f, 1.0f)},
        {-1.0f, -1.0f, true, Color(1.0f, 0.6f, 0.3f, 1.0f)},
        {1.0f, -1.0f, false, Color(1.0f, 1.0f, 1.0f, 1.0f)},
        {-1.0f, 1.0f, false, Color(1.0f, 1.0f, 1.0f, 1.0f)},
    }};

    std::shared_ptr<Texture> createCookieTexture(GraphicsDevice* device, const char* name,
        const uint32_t width, const uint32_t height)
    {
        TextureOptions options;
        options.name = name;
        options.width = width;
        options.height = height;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_shared<Texture>(device, options);
        texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        return texture;
    }
}

class ClusteredLightCookiesExample final: public ExampleApp
{
public:
    ClusteredLightCookiesExample()
        : ExampleApp({.title = "Clustered Light Cookies"}) {}

protected:
    bool create() override
    {
        // Clustered lighting is what routes the cookies through the shared cookie atlas.
        scene()->setClusteredLightingEnabled(true);

        // Cookies are off by default under clustered lighting; shadows are not needed.
        auto& lighting = scene()->lighting();
        lighting.cookiesEnabled = true;
        lighting.shadowsEnabled = false;

        // Resolution of the cookie atlas holding every cookie.
        lighting.cookieAtlasResolution = 2048;

        // The chess board: the board and its pieces are what the cookies project onto.
        _boardAsset = std::make_unique<Asset>(
            "board", AssetType::CONTAINER, assetPath("models/chess-board.glb"));
        ContainerResource* container = _boardAsset->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("Failed to load chess-board.glb");
            return false;
        }
        Entity* board = container->instantiateRenderEntity();
        if (!board) {
            spdlog::error("Failed to instantiate chess-board.glb");
            return false;
        }
        board->setEngine(engine());
        for (auto* render : board->findComponents<RenderComponent>()) {
            render->setCastShadows(false);
            render->setReceiveShadows(false);
        }
        board->setLocalScale(0.25f, 0.25f, 0.25f);
        root()->addChild(board);

        // Cookie 1: procedural, regenerated every few frames.
        _proceduralCookie = createCookieTexture(device().get(), "proceduralCookie",
            kCookieSize, kCookieSize);
        _proceduralPixels.resize(static_cast<size_t>(kCookieSize) * kCookieSize * 4);
        updateProceduralCookie(0.0f);

        // Cookie 2: the video stand-in, uploaded every frame.
        _videoCookie = createCookieTexture(device().get(), "videoCookie",
            kVideoWidth, kVideoHeight);
        _videoPixels.resize(static_cast<size_t>(kVideoWidth) * kVideoHeight * 4);
        updateVideoCookie(0.0f);

        // Four spot lights, one per quadrant of the board.
        for (size_t i = 0; i < kQuadrants.size(); ++i) {
            createSpotLight(static_cast<int>(i), kQuadrants[i]);
        }

        auto* camera = createCamera(Vector3(0.0f, 50.0f, 70.0f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setClearColor(Color(0.05f, 0.05f, 0.05f, 1.0f));
            comp->camera()->setFarClip(500.0f);
            comp->camera()->setNearClip(0.1f);
            comp->setToneMapping(TONEMAP_ACES);
        }

        // The orbit camera pivots on the board's bounds centre and keeps the camera
        // where it was placed.
        const Vector3 focus = entityBounds(board).center();
        camera->lookAt(focus);
        auto* controls = addOrbitControls(camera, focus);
        controls->setZoomRange(Vector2(0.0f, 200.0f));
        controls->storeResetState();

        spdlog::info("Four cookie spot lights, two procedural and two animated, "
                     "sharing one {} cookie atlas.", lighting.cookieAtlasResolution);
        return true;
    }

    void update(const float dt) override
    {
        ++_frame;
        _videoTime += dt;

        // Regenerate the procedural cookie every 10 frames, a little further on each time.
        if (_frame % 10 == 0) {
            _phase += 0.35f;
            updateProceduralCookie(_phase);
        }

        // Upload the next frame of the video stand-in every frame.
        updateVideoCookie(_videoTime);

        updateAtlasPreview();
    }

    void destroy() override
    {
        // The preview pass is registered with the renderer, so it goes while the engine is alive.
        if (_previewPass) {
            engine()->renderer()->removeAppendPass(_previewPass);
        }
        _previewPass.reset();
    }

private:
    void createSpotLight(const int index, const Quadrant& quadrant)
    {
        auto* entity = new Entity();
        entity->setName("Spot-" + std::to_string(index));
        entity->setEngine(engine());

        if (auto* light = static_cast<LightComponent*>(
                entity->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_SPOT);
            light->setColor(quadrant.color);
            light->setIntensity(8.0f);
            light->setInnerConeAngle(15.0f);
            light->setOuterConeAngle(45.0f);
            light->setRange(60.0f);
            light->setCastShadows(false);
            light->setCookie(quadrant.procedural ? _proceduralCookie.get() : _videoCookie.get());
            light->setCookieChannel(CookieChannel::COOKIE_CHANNEL_RGB);
            light->setCookieIntensity(1.0f);
        }

        // Above the quadrant centre, aimed straight down: lookAt with +X as up (the
        // default up is parallel to the view direction), then a quarter turn so the
        // cone, which points down the entity's -Y, follows the aimed -Z.
        const float x = quadrant.x * kQuadrantOffset;
        const float z = quadrant.z * kQuadrantOffset;
        entity->setLocalPosition(x, kLightHeight, z);
        entity->lookAt(Vector3(x, 0.0f, z), Vector3(1.0f, 0.0f, 0.0f));
        entity->rotateLocal(90.0f, 0.0f, 0.0f);
        root()->addChild(entity);
    }

    // Moving concentric rings times rotating spokes, faded to black at the edge;
    // `phase` shifts each regeneration so the content changes but stays recognizable.
    void updateProceduralCookie(const float phase)
    {
        const float half = static_cast<float>(kCookieSize) * 0.5f;
        for (uint32_t y = 0; y < kCookieSize; ++y) {
            for (uint32_t x = 0; x < kCookieSize; ++x) {
                const float dx = (static_cast<float>(x) - half) / half;
                const float dy = (static_cast<float>(y) - half) / half;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float angle = std::atan2(dy, dx);

                const float rings = std::sin(dist * 18.0f - phase * 3.0f);
                const float spokes = std::sin(angle * 6.0f + phase * 2.0f);
                float v = 0.5f + 0.5f * rings * spokes;
                v *= std::max(0.0f, 1.0f - dist);

                const auto c = static_cast<uint8_t>(std::floor(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                const size_t i = (static_cast<size_t>(y) * kCookieSize + x) * 4;
                _proceduralPixels[i] = c;
                _proceduralPixels[i + 1] = c;
                _proceduralPixels[i + 2] = c;
                _proceduralPixels[i + 3] = 255;
            }
        }
        _proceduralCookie->setLevelData(0, _proceduralPixels.data(), _proceduralPixels.size());
        _proceduralCookie->upload();
    }

    // The video stand-in: seven colour bars scrolling sideways, with a bright disc
    // circling the middle of the frame.
    void updateVideoCookie(const float time)
    {
        static constexpr std::array<std::array<uint8_t, 3>, 7> kBars = {{
            {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
            {191, 0, 191}, {191, 0, 0}, {0, 0, 191},
        }};
        const float width = static_cast<float>(kVideoWidth);
        const float height = static_cast<float>(kVideoHeight);
        const float scroll = time * 0.1f;
        const float discX = width * (0.5f + 0.3f * std::cos(time * 1.3f));
        const float discY = height * (0.5f + 0.3f * std::sin(time * 1.3f));
        const float discRadius = height * 0.18f;

        for (uint32_t y = 0; y < kVideoHeight; ++y) {
            for (uint32_t x = 0; x < kVideoWidth; ++x) {
                const float u = static_cast<float>(x) / width + scroll;
                const float wrapped = u - std::floor(u);
                const auto bar = std::min(static_cast<size_t>(wrapped * 7.0f), kBars.size() - 1);
                const auto& rgb = kBars[bar];

                const float ddx = static_cast<float>(x) - discX;
                const float ddy = static_cast<float>(y) - discY;
                const bool inDisc = ddx * ddx + ddy * ddy < discRadius * discRadius;

                const size_t i = (static_cast<size_t>(y) * kVideoWidth + x) * 4;
                _videoPixels[i] = inDisc ? 255 : rgb[0];
                _videoPixels[i + 1] = inDisc ? 255 : rgb[1];
                _videoPixels[i + 2] = inDisc ? 255 : rgb[2];
                _videoPixels[i + 3] = 255;
            }
        }
        _videoCookie->setLevelData(0, _videoPixels.data(), _videoPixels.size());
        _videoCookie->upload();
    }

    // Shows the cookie atlas in the bottom-left corner, following the window size.
    void updateAtlasPreview()
    {
        LightTextureAtlas* atlas = engine()->renderer()->lightTextureAtlas();
        Texture* atlasTexture = atlas ? atlas->cookieAtlasTexture() : nullptr;
        if (!atlasTexture) {
            return;
        }
        if (!_previewPass) {
            _previewPass = std::make_shared<RenderPassDownsample>(device(), atlasTexture);
            _previewPass->init(nullptr);
            _previewPass->setRequiresCubemaps(false);
            engine()->renderer()->addAppendPass(_previewPass);
        } else {
            _previewPass->setSourceTexture(atlasTexture);
        }

        const auto [deviceWidth, deviceHeight] = device()->size();
        const auto screenW = static_cast<float>(std::max(1, deviceWidth));
        const auto screenH = static_cast<float>(std::max(1, deviceHeight));
        const Vector4 viewport(kPreviewX * screenW, kPreviewY * screenH,
            kPreviewW * screenW, kPreviewH * screenH);
        _previewPass->setViewport(viewport);
        _previewPass->setScissor(viewport);
    }

    std::unique_ptr<Asset> _boardAsset;
    std::shared_ptr<Texture> _proceduralCookie;
    std::shared_ptr<Texture> _videoCookie;
    std::vector<uint8_t> _proceduralPixels;
    std::vector<uint8_t> _videoPixels;
    std::shared_ptr<RenderPassDownsample> _previewPass;

    int _frame = 0;
    float _phase = 0.0f;
    float _videoTime = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ClusteredLightCookiesExample)
