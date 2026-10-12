// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
#include "extras/script/proceduralSky.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <spdlog/spdlog.h>

#include "core/math/matrix4.h"
#include "core/math/quaternion.h"
#include "framework/components/light/lightComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/envLighting.h"
#include "scene/graphics/quadRender.h"
#include "scene/materials/material.h"
#include "scene/scene.h"
#include "scene/shader-lib/slangShaders.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;

        // The atmosphere constants of the Preetham model, for the view-independent terms
        // computed on the CPU (updateAtmosphere).
        constexpr double kTotalRayleigh[3] = {5.804542996261093e-6, 1.3562911419845635e-5, 3.0265902468824876e-5};
        constexpr double kMieConst[3] = {1.8399918514433978e14, 2.7798023919660528e14, 4.0790479543861094e14};
        constexpr double kSunEe = 1000.0;
        constexpr double kSunCutoff = 1.6110731556870734;
        constexpr double kSunSteepness = 1.5;

        // The ambient and reflection sample counts of the lighting bake.
        constexpr int kReflectionSamples = 256;
        constexpr int kAmbientSamples = 512;

        // A render pass for the bake; the device outlives it (one function call).
        struct SkyBakePass final : RenderPass
        {
            explicit SkyBakePass(GraphicsDevice* device)
                : RenderPass(std::shared_ptr<GraphicsDevice>(device, [](GraphicsDevice*) {}))
            {
                _name = "ProceduralSkyCubeFace";
            }
        };

        float saturate(const float value) { return std::clamp(value, 0.0f, 1.0f); }

        Color lerpColor(const Color& a, const Color& b, const float t)
        {
            return Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f);
        }

        void store3(float (&out)[4], const Vector3& v)
        {
            out[0] = v.getX();
            out[1] = v.getY();
            out[2] = v.getZ();
        }
    }

    // The visible sky: the procedural-sky program on the scene's sky mesh, its uniforms
    // the script's block. Whether the target takes linear HDR is known only at draw time.
    class ProceduralSky::SkyMaterial final : public Material
    {
    public:
        explicit SkyMaterial(const std::shared_ptr<GraphicsDevice>& device)
            : _device(device)
        {
            setName("ProceduralSkyMaterial");
            setIsSkybox(true);
            // Drawn from inside the sky mesh, behind everything, writing no depth.
            setCullMode(CullMode::CULLFACE_FRONT);
            auto depthState = std::make_shared<DepthState>();
            depthState->setDepthWrite(false);
            setDepthState(depthState);
            setShaderOverride(getOrCreateSlangShader(device.get(), "procedural-sky"));
        }

        void setBlock(const ProceduralSkyBlock& block)
        {
            _source = block;
            markUniformsDirty();
        }

        const void* customUniformData(size_t& outSize) const override
        {
            _drawn = _source;
            const bool linear = _device && _device->hdrPass();
            _drawn.output[3] = linear ? 1.0f : 0.0f;
            outSize = sizeof(_drawn);
            return &_drawn;
        }

    private:
        std::shared_ptr<GraphicsDevice> _device;
        ProceduralSkyBlock _source;
        mutable ProceduralSkyBlock _drawn;
    };

    ProceduralSky::ProceduralSky() = default;

    ProceduralSky::~ProceduralSky()
    {
        releaseScene();
    }

    void ProceduralSky::releaseScene()
    {
        if (auto scene = _scene.lock()) {
            if (_material && scene->skyMaterial() == _material) {
                scene->setSkyMaterial(nullptr);
            }
            if (_envAtlas && scene->envAtlas() == _envAtlas.get()) {
                scene->setEnvAtlas(nullptr);
            }
        }
        _scene.reset();
    }

    void ProceduralSky::setSunLight(Entity* light)
    {
        _sunLight = light;
        _baseSunIntensity = -1.0f;
        _sunLightWatch.watch(light, [this]() { _sunLight = nullptr; });
    }

    void ProceduralSky::initialize()
    {
        Engine* engine = entity() ? entity()->engine() : nullptr;
        if (!engine || !engine->scene() || !engine->graphicsDevice()) {
            spdlog::warn("ProceduralSky: no engine, scene or device to draw with");
            return;
        }
        _device = engine->graphicsDevice();
        _scene = engine->scene();
        _material = std::make_shared<SkyMaterial>(_device);
        engine->scene()->setSkyMaterial(_material);
        _baked = false;
    }

    Vector3 ProceduralSky::computeSunDir() const
    {
        const float el = _elevation * kDegToRad;
        const float az = (_azimuth + _rotation) * kDegToRad;
        const float cosEl = std::cos(el);
        return Vector3(cosEl * std::sin(az), std::sin(el), cosEl * std::cos(az)).normalized();
    }

    Vector3 ProceduralSky::computeMoonDir() const
    {
        // Turned around Y the way an increasing azimuth turns the sun.
        const float a = _rotation * kDegToRad;
        const float s = std::sin(a);
        const float c = std::cos(a);
        const float x = _moonDirection.getX();
        const float z = _moonDirection.getZ();
        return Vector3(x * c + z * s, _moonDirection.getY(), z * c - x * s).normalized();
    }

    void ProceduralSky::updateAtmosphere()
    {
        // The terms constant across the sky, once a frame rather than per pixel.
        const double sunY = _skyDir.getY();
        const double sunFade = 1.0 - std::clamp(1.0 - std::exp(sunY), 0.0, 1.0);
        const double rayleighCoefficient = _rayleigh - (1.0 - sunFade);
        const double mieC = 0.434 * (0.2 * _turbidity * 1e-18) * _mieCoefficient;
        for (int i = 0; i < 3; ++i) {
            _block.betaR[i] = static_cast<float>(kTotalRayleigh[i] * rayleighCoefficient);
            _block.betaM[i] = static_cast<float>(kMieConst[i] * mieC);
        }
        const double zenithCos = std::clamp(sunY, -1.0, 1.0);
        _block.sunDir[3] = static_cast<float>(
            kSunEe * std::max(0.0, 1.0 - std::exp(-((kSunCutoff - std::acos(zenithCos)) / kSunSteepness))));
    }

    void ProceduralSky::updateSunLight()
    {
        auto* light = _sunLight ? _sunLight->findComponent<LightComponent>() : nullptr;
        if (!light) {
            return;
        }
        // The light's own intensity, captured before it is first overwritten, is the
        // daytime peak.
        if (_baseSunIntensity < 0.0f) {
            _baseSunIntensity = light->intensity();
        }

        // The key light crossfades from the sun to the moon only once the sun is below the
        // horizon, so while the sun shows, the light comes from exactly where it is.
        const float nightFactor = saturate(-_elevation / 6.0f);
        const Vector3 source = Vector3::lerp(_sunDir, computeMoonDir(), nightFactor).normalized();

        // A directional light shines along its entity's -Y, so the entity's +Y points at the
        // source (lookAt would aim -Z instead).
        const Vector3 ref = std::abs(source.getY()) > 0.99f ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 1.0f, 0.0f);
        const Vector3 axisX = source.cross(ref).normalized();
        const Vector3 axisZ = axisX.cross(source).normalized();
        Matrix4 basis = Matrix4::identity();
        basis.setColumn(0, Vector4(axisX.getX(), axisX.getY(), axisX.getZ(), 0.0f));
        basis.setColumn(1, Vector4(source.getX(), source.getY(), source.getZ(), 0.0f));
        basis.setColumn(2, Vector4(axisZ.getX(), axisZ.getY(), axisZ.getZ(), 0.0f));
        _sunLight->setRotation(Quaternion::fromMatrix4(basis));

        // Warm near the horizon, white towards the zenith, the cold moon at night.
        const float y = _sunDir.getY();
        Color color = lerpColor(Color(1.0f, 0.6f, 0.35f, 1.0f), Color(1.0f, 1.0f, 0.98f, 1.0f), saturate(y / 0.4f));
        color = lerpColor(color, _moonColor, nightFactor);
        light->setColor(color);

        // The sun fades out below the horizon while the moon fades in.
        light->setIntensity(_baseSunIntensity * saturate(y / 0.1f) + _moonIntensity * nightFactor);
    }

    void ProceduralSky::bakeLighting()
    {
        GraphicsDevice* device = _device.get();
        auto scene = _scene.lock();
        if (!device || !scene) {
            return;
        }

        const int faceSize = std::max(8, _lightingResolution);
        if (!_lightingCube || static_cast<int>(_lightingCube->width()) != faceSize) {
            PixelFormat format = PixelFormat::PIXELFORMAT_RGBA16F;
            if (!device->textureHalfFloatRenderable()) {
                if (!device->textureFloatRenderable()) {
                    spdlog::warn("ProceduralSky: the device renders to no float format; no sky lighting");
                    return;
                }
                format = PixelFormat::PIXELFORMAT_RGBA32F;
            }
            TextureOptions options;
            options.name = "procedural-sky-lighting";
            options.width = static_cast<uint32_t>(faceSize);
            options.height = static_cast<uint32_t>(faceSize);
            options.format = format;
            options.cubemap = true;
            options.mipmaps = true;
            options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;
            _lightingCube = std::make_shared<Texture>(device, options);
            _lightingCube->upload();
            for (int face = 0; face < 6; ++face) {
                RenderTargetOptions targetOptions;
                targetOptions.graphicsDevice = device;
                targetOptions.colorBuffer = _lightingCube.get();
                targetOptions.face = face;
                targetOptions.depth = false;
                targetOptions.samples = 1;
                targetOptions.name = "procedural-sky-lighting-face";
                _faceTargets[static_cast<size_t>(face)] = device->createRenderTarget(targetOptions);
            }
        }

        // The day sky into the six faces, then the mips the convolution samples.
        QuadRender quad(getOrCreateSlangShader(device, "procedural-sky-cube"));
        ProceduralSkyBlock block = _block;
        device->beginOfflineWork();
        for (int face = 0; face < 6; ++face) {
            const auto& target = _faceTargets[static_cast<size_t>(face)];
            if (!target) {
                continue;
            }
            SkyBakePass pass(device);
            pass.init(target);
            block.extra[1] = static_cast<float>(face);
            quad.setUniforms(block);
            device->startRenderPass(&pass);
            device->setBlendState(BlendState::noBlend());
            device->setDepthState(DepthState::noDepth());
            device->setCullMode(CullMode::CULLFACE_NONE);
            device->setStencilState();
            quad.render();
            device->endRenderPass(&pass);
        }
        device->generateMipmaps(_lightingCube.get());
        device->endOfflineWork();

        // Into the atlas; a later bake refills the same texture, so materials need nothing.
        Texture* atlas = EnvLighting::generateAtlas(device, _lightingCube.get(), _envAtlas.get(),
            std::max(64, _atlasSize), kReflectionSamples, kAmbientSamples);
        if (atlas && atlas != _envAtlas.get()) {
            if (_envAtlas && scene->envAtlas() == _envAtlas.get()) {
                scene->setEnvAtlas(nullptr);
            }
            _envAtlas.reset(atlas);
            scene->setEnvAtlas(atlas);
        }
    }

    void ProceduralSky::update(float /*dt*/)
    {
        auto scene = _scene.lock();
        if (!_material || !scene) {
            return;
        }

        // The world direction to the sun, and the same in sky space (X negated), where the
        // environment is looked up.
        _sunDir = computeSunDir();
        _skyDir = Vector3(-_sunDir.getX(), _sunDir.getY(), _sunDir.getZ());

        store3(_block.sunDir, _skyDir);
        updateAtmosphere();
        _block.betaR[3] = _mieDirectionalG;
        _block.betaM[3] = _luminance;

        // The night sky fades in from 2 degrees above the horizon to 8 below.
        _block.night[0] = saturate((2.0f - _elevation) / 10.0f);
        _block.night[1] = _nightBrightness;
        _block.night[2] = _starBrightness;
        _block.night[3] = _starDensity;
        _block.nightColor[0] = _nightColor.r;
        _block.nightColor[1] = _nightColor.g;
        _block.nightColor[2] = _nightColor.b;
        _block.nightColor[3] = _starSize;
        const Vector3 moon = computeMoonDir();
        store3(_block.moonDir, Vector3(-moon.getX(), moon.getY(), moon.getZ()));
        _block.moonDir[3] = _moonSize * kDegToRad;
        _block.moonColor[0] = _moonColor.r;
        _block.moonColor[1] = _moonColor.g;
        _block.moonColor[2] = _moonColor.b;
        _block.moonColor[3] = _moonGlow;
        _block.output[0] = scene->skyboxIntensity();
        _block.output[1] = scene->exposure();
        _block.output[2] = static_cast<float>(scene->toneMapping());
        _block.extra[0] = _twilightGlow;
        _material->setBlock(_block);

        updateSunLight();

        // Bake the lighting again only when the sun has moved far enough or the sky changed.
        const std::array<float, 7> params = {_rayleigh, _turbidity, _mieCoefficient, _mieDirectionalG,
            _luminance, static_cast<float>(_atlasSize), static_cast<float>(_lightingResolution)};
        const float cosThreshold = std::cos(_lightingThreshold * kDegToRad);
        const bool moved = _skyDir.dot(_bakedDir) < cosThreshold;
        if (!_baked || moved || params != _bakedParams) {
            bakeLighting();
            _bakedDir = _skyDir;
            _bakedParams = params;
            _baked = true;
        }
    }
}
