// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream camera-frame/custom-compose-shader.
//
// A custom post effect added to the camera frame with a CameraFrameEffect: a pixelation, written
// as a compose chunk whose entry function the single compose pass calls at the LDR slot (after
// tone mapping, the colour LUT and the vignette, before gamma), so no full-screen pass of its own
// is needed. Its parameters are uniforms set in update(), every frame.
//
// The apartment (scaled 30x) under the helipad env atlas (exposure 1.2), with the neon "love"
// sign lit yellow at emissive intensity 200 so it blooms; the camera frame renders 4x MSAA,
// neutral tone mapping, bloom 0.03 over 7 levels and a vignette (0.5, 1, 0.5, 0.8).
//
// Keys stand in for upstream's controls panel, over the same parameters:
//   [ / ]  tile size -/+ 2 pixels (2..32)
//   - / =  pixelation intensity -/+ 0.1 (0 leaves the effect out of the shader)
//   T      tone mapping, cycled through linear, filmic, ACES, ACES2, neutral, none
//
// DEVIATIONS:
// - upstream's orbitCamera script is the examples' CameraControls in orbit mode from the authored
//   position; no inertia. Its focus entity (at (-80, 80, -20)) has no meshes and is not in the
//   scene, so the script's bounds are empty and it orbits the ORIGIN, as this port does.
// - the chunk is Slang, reads its uniforms by name without declaring them, and measures the
//   tiles from the bottom-left corner of the frame, where upstream's uv starts.
//
#include <algorithm>
#include <array>
#include <memory>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"
#include "scene/graphics/cameraFrameEffect.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    // The pixelation, a compose chunk at the LDR slot: dots on a grid of tiles, each the colour
    // under its centre, blended with the image by the intensity.
    const char* const kPixelationSlang = R"slang(
float3 applyPixelation(float3 color, float2 uv)
{
    const float2 p = float2(uv.x, 1.0 - uv.y);
    const float2 tileUV = float2(pixelationTilePixels, pixelationTilePixels) * sceneTextureSize.zw;
    const float2 centerUv = (floor(p / tileUV) + 0.5) * tileUV;
    const float2 local = (p - centerUv) / tileUV;
    const float dist = length(local);
    const float radius = 0.35;
    const float edge = fwidth(dist) * 1.5;
    const float mask = 1.0 - smoothstep(radius, radius + edge, dist);
    const float3 dotResult = lerp(float3(0.0, 0.0, 0.0), color, mask);
    return lerp(color, dotResult, pixelationIntensity);
}
)slang";

    class PixelationEffect final : public CameraFrameEffect
    {
    public:
        PixelationEffect()
            : CameraFrameEffect("pixelation", CameraFrameEffectOptions{.slot = ComposeSlot::Ldr, .slang = kPixelationSlang})
        {
        }

        // Size of one pixelation tile in screen pixels
        float tilePixels = 8.0f;
        // Blend between the original image (0) and the pixelated result (1)
        float intensity = 0.5f;

        // At zero intensity the effect would leave the image untouched, so it is left out of the shader
        bool active() const override { return enabled() && intensity > 0.0f; }

        void update() override
        {
            setUniform("pixelationTilePixels", tilePixels);
            setUniform("pixelationIntensity", intensity);
        }
    };

    template <typename Fn>
    void forEachMaterial(Entity* entity, Fn&& fn)
    {
        if (!entity) {
            return;
        }
        for (auto* render : entity->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                if (auto* material = meshInstance ? meshInstance->material() : nullptr) {
                    fn(material);
                }
            }
        }
    }

    constexpr std::array<int, 6> kToneMappings = {TONEMAP_LINEAR, TONEMAP_FILMIC, TONEMAP_ACES, TONEMAP_ACES2,
        TONEMAP_NEUTRAL, TONEMAP_NONE};
}

class CustomComposeShaderExample final : public ExampleApp
{
public:
    // The scene is rendered to an antialiased texture, so the back buffer is not.
    CustomComposeShaderExample() : ExampleApp({.title = "Custom Compose Shader Example", .antialias = false}) {}

protected:
    bool create() override
    {
        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _apartment = std::make_unique<Asset>("apartment", AssetType::CONTAINER, assetPath("models/apartment.glb"));
        _love = std::make_unique<Asset>("love", AssetType::CONTAINER, assetPath("models/love.glb"));

        // Setup skydome with low intensity
        if (Texture* envAtlas = _envAtlas->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }
        scene()->setExposure(1.2f);

        // The apartment
        auto* apartmentContainer = _apartment->resourceAs<ContainerResource>();
        auto* loveContainer = _love->resourceAs<ContainerResource>();
        if (!apartmentContainer || !loveContainer) {
            spdlog::error("custom-compose-shader needs models/apartment.glb and models/love.glb");
            return false;
        }
        auto* apartment = apartmentContainer->instantiateRenderEntity();
        apartment->setLocalScale(30.0f, 30.0f, 30.0f);
        root()->addChild(apartment);

        // The love sign, its neon tube emissive enough to bloom
        auto* love = loveContainer->instantiateRenderEntity();
        love->setLocalPosition(-80.0f, 30.0f, -20.0f);
        love->setLocalScale(130.0f, 130.0f, 130.0f);
        love->rotate(0.0f, -90.0f, 0.0f);
        root()->addChild(love);
        if (auto* neon = dynamic_cast<Entity*>(love->findByName("s.0009_Standard_FF00BB_0"))) {
            if (auto* render = neon->findComponent<RenderComponent>(); render && !render->meshInstances().empty()) {
                if (auto* standard = dynamic_cast<StandardMaterial*>(render->meshInstances()[0]->material())) {
                    standard->setEmissive(Color(1.0f, 1.0f, 0.0f, 1.0f));
                    standard->setEmissiveIntensity(200.0f);
                }
            }
        }
        // the sign's materials without dynamic refraction
        forEachMaterial(love, [](Material* material) {
            if (auto* standard = dynamic_cast<StandardMaterial*>(material)) {
                standard->setUseDynamicRefraction(false);
            }
        });

        // The camera
        auto* camera = createCamera(Vector3(-50.0f, 100.0f, 220.0f));
        _cameraComp = camera->findComponent<CameraComponent>();
        if (!_cameraComp || !_cameraComp->camera()) {
            return false;
        }
        _cameraComp->camera()->setFarClip(1500.0f);
        _cameraComp->camera()->setFov(80.0f);
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setZoomRange(Vector2(0.0f, 500.0f));
            controls->storeResetState();
        }

        // The camera frame
        _cameraComp->setToneMapping(TONEMAP_NEUTRAL);
        auto rendering = _cameraComp->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.03f;
        rendering.bloomBlurLevel = 7;
        rendering.vignetteEnabled = true;
        rendering.vignetteInner = 0.5f;
        rendering.vignetteOuter = 1.0f;
        rendering.vignetteCurvature = 0.5f;
        rendering.vignetteIntensity = 0.8f;
        _cameraComp->setRendering(rendering);

        // Register the effect. Effects sharing a slot run in registration order, after the
        // frame's own effects there, so the pixelation is applied after the vignette.
        _pixelation = std::make_shared<PixelationEffect>();
        _cameraComp->addFrameEffect(_pixelation);

        spdlog::info("Controls: [ ] tile size, - = intensity, T tone mapping");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_LEFTBRACKET:
            _pixelation->tilePixels = std::max(2.0f, _pixelation->tilePixels - 2.0f);
            break;
        case SDLK_RIGHTBRACKET:
            _pixelation->tilePixels = std::min(32.0f, _pixelation->tilePixels + 2.0f);
            break;
        case SDLK_MINUS:
            _pixelation->intensity = std::max(0.0f, _pixelation->intensity - 0.1f);
            break;
        case SDLK_EQUALS:
            _pixelation->intensity = std::min(1.0f, _pixelation->intensity + 0.1f);
            break;
        case SDLK_T:
            _toneMapping = (_toneMapping + 1) % kToneMappings.size();
            _cameraComp->setToneMapping(kToneMappings[_toneMapping]);
            break;
        default:
            return false;
        }
        spdlog::info("tile {:.0f} px, intensity {:.1f}, tone mapping {}", _pixelation->tilePixels,
            _pixelation->intensity, kToneMappings[_toneMapping]);
        return true;
    }

private:
    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _apartment;
    std::unique_ptr<Asset> _love;
    CameraComponent* _cameraComp = nullptr;
    std::shared_ptr<PixelationEffect> _pixelation;
    size_t _toneMapping = 4;
};

VISUTWIN_EXAMPLE_MAIN(CustomComposeShaderExample)
