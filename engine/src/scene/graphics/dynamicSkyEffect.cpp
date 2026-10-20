// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.10.2026
//
#include "dynamicSkyEffect.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/math/matrix4.h"
#include "core/math/quaternion.h"
#include "core/math/vector4.h"
#include "dynamicSkyNoise.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/componentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "renderPassShaderQuad.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/envLighting.h"
#include "scene/graphics/quadRender.h"
#include "scene/layer.h"
#include "scene/materials/material.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "scene/shader-lib/slangShaders.h"
#include "scene/skybox/skyMesh.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;

        // The atmosphere constants of the three.js `Sky` shader, for the scattering terms that
        // are the same for the whole sky.
        constexpr double kTotalRayleigh[3] = {5.804542996261093e-6, 1.3562911419845635e-5, 3.0265902468824876e-5};
        constexpr double kMieConst[3] = {1.8399918514433978e14, 2.7798023919660528e14, 4.0790479543861094e14};
        constexpr double kSunEe = 1000.0;
        constexpr double kSunCutoff = 1.6110731556870734;
        constexpr double kSunSteepness = 1.5;

        // The scale balancing the sky's radiance against the sun's light. The sky model's ambient
        // light, its radiance averaged over the upper hemisphere by the cosine, is about
        // 2 sin(elevation) at a scale of 1: divided by the sine, the sky lights the scene with
        // about a tenth of the sun's light at any elevation, a clear sky's share of daylight.
        constexpr float kSkyBalance = 0.05f;

        // Repeats of the volumetric clouds' detail per repeat of their shapes (the shader's).
        constexpr float kCloudDetailScale = 10.0f;
        // The loop limits of the shader: the steps through the layer and toward the light.
        constexpr int kCloudMaxSteps = 128;
        constexpr int kCloudMaxLightSteps = 6;
        // The samples at the cloud quality 0, 0.5 and 1.
        constexpr std::array<float, 3> kCloudQualitySteps = {24.0f, 48.0f, 64.0f};
        constexpr std::array<float, 3> kCloudQualityLightSteps = {1.0f, 2.0f, 4.0f};
        constexpr std::array<float, 3> kCloudQualityFineStep = {0.45f, 0.4f, 0.25f};

        // The samples prefiltering the reflections and the ambient light of the lighting.
        constexpr int kReflectionSamples = 256;
        constexpr int kAmbientSamples = 512;
        // The fraction a setting changes by before the lighting is generated again.
        constexpr float kSettingsTolerance = 0.01f;
        // Interpolated, a change this large is a jump, which applies at once.
        constexpr float kJumpThresholds = 4.0f;
        constexpr float kJumpTolerance = 0.1f;
        // The longest blend from the previous lighting to the latest, in seconds.
        constexpr float kBlendMaxTime = 1.0f;

        float saturate(const float v) { return std::clamp(v, 0.0f, 1.0f); }

        float smoothstep(const float a, const float b, const float x)
        {
            const float t = saturate((x - a) / (b - a));
            return t * t * (3.0f - 2.0f * t);
        }

        float cloudQualityValue(const std::array<float, 3>& values, const float quality)
        {
            const float t = saturate(quality) * 2.0f;
            const int i = std::min(static_cast<int>(std::floor(t)), 1);
            return values[static_cast<size_t>(i)] +
                (values[static_cast<size_t>(i) + 1] - values[static_cast<size_t>(i)]) * (t - static_cast<float>(i));
        }

        float wrap(const float value, const float period) { return value - std::floor(value / period) * period; }

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

        // M e_i, the columns of a 3x3 product given as column functions.
        void storeColumns(float (&out)[3][4], const Vector3& c0, const Vector3& c1, const Vector3& c2)
        {
            store3(out[0], c0);
            store3(out[1], c1);
            store3(out[2], c2);
        }

        Vector3 column3(const Matrix4& m, const int i)
        {
            const Vector4 c = m.getColumn(i);
            return Vector3(c.getX(), c.getY(), c.getZ());
        }

        // A render pass for one quad of the lighting; the device outlives it (one call).
        struct LightingQuadPass final : RenderPass
        {
            explicit LightingQuadPass(GraphicsDevice* device)
                : RenderPass(std::shared_ptr<GraphicsDevice>(device, [](GraphicsDevice*) {}))
            {
                _name = "DynamicSkyLighting";
            }
        };
    }

    // ---- The sky's material: the dynamic-sky program on the effect's own sky mesh, the effect's
    // block, and the cloud textures on the material rows the program reads them at. Whether the
    // target takes linear HDR is known only at draw time.
    class DynamicSkyEffect::SkyMaterial final : public Material
    {
    public:
        explicit SkyMaterial(const std::shared_ptr<GraphicsDevice>& device) : _device(device)
        {
            setName("DynamicSkyMaterial");
            // Drawn from inside the sky mesh, behind everything, writing no depth. Not a skybox
            // material: the camera hides those while its frame renders the sky itself.
            setCullMode(CullMode::CULLFACE_FRONT);
            auto depthState = std::make_shared<DepthState>();
            depthState->setDepthWrite(false);
            setDepthState(depthState);
        }

        void setVariant(const std::string& variant)
        {
            if (variant != _variant || !shaderOverride()) {
                _variant = variant;
                setShaderOverride(getOrCreateSlangShader(_device.get(), "dynamic-sky", variant));
            }
        }

        void setBlock(const Block& block)
        {
            _source = block;
            markUniformsDirty();
        }

        void setTextures(const std::array<Texture*, 5>& textures)
        {
            if (textures != _textures) {
                _textures = textures;
                markUniformsDirty();
            }
        }

        const void* customUniformData(size_t& outSize) const override
        {
            _drawn = _source;
            _drawn.output[2] = _device && _device->hdrPass() ? 1.0f : 0.0f;
            outSize = sizeof(_drawn);
            return &_drawn;
        }

        void getTextureSlots(std::vector<TextureSlot>& slots) const override
        {
            // The cloud map, shapes, detail, blue noise and cloud buffer, at the Metal slots of
            // the material rows dynamic-sky.slang declares them on (VT_DECLARE_<row>_AS).
            constexpr std::array<int, 5> kSlots = {0, 1, 3, 4, 5};
            for (size_t i = 0; i < _textures.size(); ++i) {
                if (_textures[i]) {
                    slots.push_back({kSlots[i], _textures[i]});
                }
            }
        }

    private:
        std::shared_ptr<GraphicsDevice> _device;
        std::string _variant;
        Block _source{};
        mutable Block _drawn{};
        std::array<Texture*, 5> _textures{};
    };

    // ---- The volumetric clouds into their texture: the effect's block, the noise through the
    // textures' own samplers (repeating).
    class DynamicSkyEffect::CloudPass final : public RenderPassShaderQuad
    {
    public:
        CloudPass(const std::shared_ptr<GraphicsDevice>& device, DynamicSkyEffect* effect, const bool encoded)
            : RenderPassShaderQuad(device), _effect(effect), _encoded(encoded)
        {
            _name = "RenderPassDynamicSkyClouds";
        }

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangShader("dynamic-sky", _encoded ? "cloud-buffer-encoded" : "cloud-buffer");
            }
        }

        void execute() override
        {
            prepareShaders();
            DynamicSkyNoise* noise = _effect->_noise.get();
            if (!shader() || !noise) {
                return;
            }
            const std::array<Texture*, 4> inputs = {noise->cloudMap(), noise->shapeMap(), noise->detailMap(),
                noise->blueNoise()};
            for (size_t i = 0; i < inputs.size(); ++i) {
                setQuadTextureBinding(i, inputs[i]);
                setQuadTextureOwnSampler(i, true);
            }
            setQuadUniforms(_effect->_block);
            RenderPassShaderQuad::execute();
        }

    private:
        DynamicSkyEffect* _effect = nullptr;
        bool _encoded = false;
    };

    // ---- The lighting, generated before the scene renders on the frames it is due: a pass with
    // no target, whose execute() runs with no device pass open.
    class DynamicSkyEffect::LightingPass final : public RenderPass
    {
    public:
        LightingPass(const std::shared_ptr<GraphicsDevice>& device, DynamicSkyEffect* effect)
            : RenderPass(device), _effect(effect)
        {
            _name = "DynamicSkyLighting";
        }

        void execute() override { _effect->updateLighting(); }

    private:
        DynamicSkyEffect* _effect = nullptr;
    };

    DynamicSkyEffect::DynamicSkyEffect() : CameraFrameEffect("dynamicSky") {}

    DynamicSkyEffect::~DynamicSkyEffect()
    {
        destroyPasses();
        // The scene goes on rendering after the effect; it no longer lights it.
        if (_scene && _envAtlas && _scene->envAtlas() == _envAtlas.get()) {
            _scene->setEnvAtlas(nullptr);
        }
        destroyLightingTargets();
    }

    void DynamicSkyEffect::setLight(LightComponent* light)
    {
        _light = light;
        _lightWatch.watch(light ? light->entity() : nullptr, [this] { _light = nullptr; });
    }

    bool DynamicSkyEffect::cloudsBuffered() const
    {
        const CameraComponent* cameraComponent = camera();
        const Camera* cam = cameraComponent ? cameraComponent->camera() : nullptr;
        return cloudType == CloudType::Volumetric && cam &&
            cam->projection() == ProjectionType::Perspective;
    }

    std::string DynamicSkyEffect::buildKey() const
    {
        return cloudsBuffered() ? "buffered" : "";
    }

    void DynamicSkyEffect::createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes)
    {
        GraphicsDevice* device = resources.device.get();
        CameraComponent* cameraComponent = camera();
        const Engine* engine = cameraComponent && cameraComponent->system() ? cameraComponent->system()->engine() : nullptr;
        if (!device || !engine || !engine->scene()) {
            return;
        }
        _device = resources.device;
        _scene = engine->scene().get();
        _noise = DynamicSkyNoise::forDevice(device);

        // The sky, drawn by the effect's camera where the scene's sky is drawn.
        if (!_material) {
            _material = std::make_shared<SkyMaterial>(_device);
            _mesh = SkyMesh::createInfiniteSkyMesh(_device);
            _node = std::make_unique<GraphNode>("DynamicSkyNode");
            _meshInstance = std::make_unique<MeshInstance>(_mesh.get(), _material.get(), _node.get());
            _meshInstance->setCull(false);
            // Shown only while its camera leaves the sky to the effect: from the frame the effect
            // stops (removed, disabled, or the camera out of its frame) the scene's sky is back.
            _meshInstance->setReplacesSky(true);
            _meshInstance->setCastShadow(false);
            _meshInstance->setReceiveShadow(false);
        }
        _meshInstance->setOnlyCamera(cameraComponent->camera());
        if (const auto& layers = _scene->layers()) {
            if (const auto layer = layers->getLayerById(LAYERID_SKYBOX)) {
                layer->addMeshInstances({_meshInstance.get()});
                _layer = layer.get();
            }
        }
        if (!_layer) {
            spdlog::warn("DynamicSkyEffect: the scene has no Skybox layer; the sky is not drawn");
        }

        _lightingPass = std::make_shared<LightingPass>(resources.device, this);
        passes.preScene.push_back(_lightingPass);

        // The volumetric clouds into a texture of their own.
        if (cloudsBuffered()) {
            const bool halfFloat = device->textureHalfFloatRenderable();
            _cloudEncoded = !halfFloat;
            TextureOptions options;
            options.name = "DynamicSkyClouds";
            options.width = 4;
            options.height = 4;
            options.format = halfFloat ? PixelFormat::PIXELFORMAT_RGBA16F : PixelFormat::PIXELFORMAT_RGBA8;
            options.mipmaps = false;
            options.minFilter = FilterMode::FILTER_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;
            _cloudTexture = std::make_shared<Texture>(device, options);
            _cloudTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            _cloudTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            RenderTargetOptions targetOptions;
            targetOptions.graphicsDevice = device;
            targetOptions.colorBuffer = _cloudTexture.get();
            targetOptions.depth = false;
            targetOptions.name = "DynamicSkyClouds";
            _cloudTarget = device->createRenderTarget(targetOptions);
            _cloudPass = std::make_shared<CloudPass>(resources.device, this, _cloudEncoded);
            _cloudPass->init(_cloudTarget);
            passes.preScene.push_back(_cloudPass);
        }
    }

    void DynamicSkyEffect::destroyPasses()
    {
        if (_layer && _meshInstance) {
            _layer->removeMeshInstances({_meshInstance.get()});
        }
        _layer = nullptr;
        _lightingPass.reset();
        _cloudPass.reset();
        _cloudTarget.reset();
        _cloudTexture.reset();
    }

    Vector3 DynamicSkyEffect::computeSunDir() const
    {
        const float el = elevation * kDegToRad;
        const float az = (azimuth + rotation) * kDegToRad;
        const float cosEl = std::cos(el);
        return Vector3(cosEl * std::sin(az), std::sin(el), cosEl * std::cos(az)).normalized();
    }

    Vector3 DynamicSkyEffect::computeMoonDir() const
    {
        // Turned around Y the way an increasing azimuth turns the sun.
        const float a = rotation * kDegToRad;
        const float s = std::sin(a);
        const float c = std::cos(a);
        const float x = moonDirection.getX();
        const float z = moonDirection.getZ();
        return Vector3(x * c + z * s, moonDirection.getY(), z * c - x * s).normalized();
    }

    void DynamicSkyEffect::updateAtmosphere()
    {
        const double sunY = _skyDir.getY();

        // The Rayleigh scattering weakens as the sun sets, kept above zero: a negative one would
        // turn extinction into gain, and none would divide zero by zero.
        const double sunFade = 1.0 - std::clamp(1.0 - std::exp(sunY), 0.0, 1.0);
        const double rayleighCoefficient = std::max(static_cast<double>(rayleigh) - (1.0 - sunFade), 1e-4);
        const double mieC = 0.434 * (0.2 * std::max(static_cast<double>(turbidity), 0.0) * 1e-18) *
            std::max(static_cast<double>(mieCoefficient), 0.0);
        for (int i = 0; i < 3; ++i) {
            _block.betaR[i] = static_cast<float>(kTotalRayleigh[i] * rayleighCoefficient);
            _block.betaM[i] = static_cast<float>(kMieConst[i] * mieC);
        }
        const double zenithCos = std::clamp(sunY, -1.0, 1.0);
        _block.sunDir[3] = static_cast<float>(
            kSunEe * std::max(0.0, 1.0 - std::exp(-((kSunCutoff - std::acos(zenithCos)) / kSunSteepness))));
        // A fully forward scattering would divide by zero toward the sun.
        _block.betaR[3] = std::clamp(mieDirectionalG, 0.0f, 0.999f);
        // The sky balanced against the sun's light, held below about 6 degrees, where the sun's
        // light fades out and the sky darkens with it into the twilight.
        _block.betaM[3] = luminance * sunIntensity * kSkyBalance / std::max(_skyDir.getY(), 0.1f);
    }

    void DynamicSkyEffect::updateLight()
    {
        // The light crossfades from the sun to the moon once the sun dips below the horizon.
        const float nightFactor = saturate(-elevation / 6.0f);
        const Vector3 moon = computeMoonDir();
        const Vector3 source = Vector3::lerp(_sunDir, moon, nightFactor).normalized();

        // Warm to white sun by day, the cold moon by night.
        const float y = _sunDir.getY();
        Color sunColor = lerpColor(Color(1.0f, 0.6f, 0.35f, 1.0f), Color(1.0f, 1.0f, 0.98f, 1.0f), saturate(y / 0.4f));
        sunColor = lerpColor(sunColor, moonColor, nightFactor);

        // A low sun's light crosses a long path through the air, so on the clouds it fades in as
        // the sun rises, at full strength at about 17 degrees; at night the moon lights them.
        const float cloudIntensity =
            (sunIntensity * smoothstep(0.0f, 0.3f, y) + cloudNightBrightness * nightFactor) * cloudBrightness;
        _block.cloudLight[0] = std::pow(sunColor.r, 2.2f) * cloudIntensity;
        _block.cloudLight[1] = std::pow(sunColor.g, 2.2f) * cloudIntensity;
        _block.cloudLight[2] = std::pow(sunColor.b, 2.2f) * cloudIntensity;
        _block.cloudLightDir[0] = -source.getX();
        _block.cloudLightDir[1] = source.getY();
        _block.cloudLightDir[2] = source.getZ();

        // The brightness of the light on the clouds, which 8-bit clouds are divided by, so the
        // dark clouds of the night keep their precision: the sun's, the sky's around them (about a
        // tenth of the sun's), or the night sky's.
        const float* light = _block.cloudLight;
        const float skyLight = luminance * sunIntensity * kSkyBalance * 2.0f * std::max(y, 0.0f) / std::max(y, 0.1f);
        _block.cloudLight[3] = std::max({0.2126f * light[0] + 0.7152f * light[1] + 0.0722f * light[2], skyLight,
            (0.2126f * _block.night[0] + 0.7152f * _block.night[1] + 0.0722f * _block.night[2]) * 0.6f, 1e-5f});

        if (!_light) {
            return;
        }
        if (_light->type() != LightType::LIGHTTYPE_DIRECTIONAL) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("DynamicSkyEffect: the light is not a directional light; it is not kept in sync with the sun");
            }
            return;
        }
        // A directional light shines down its entity's -Y, so +Y points toward the source.
        const Vector3 ref = std::abs(source.getY()) > 0.99f ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 1.0f, 0.0f);
        const Vector3 axisX = source.cross(ref).normalized();
        const Vector3 axisZ = axisX.cross(source).normalized();
        Matrix4 basis = Matrix4::identity();
        basis.setColumn(0, Vector4(axisX.getX(), axisX.getY(), axisX.getZ(), 0.0f));
        basis.setColumn(1, Vector4(source.getX(), source.getY(), source.getZ(), 0.0f));
        basis.setColumn(2, Vector4(axisZ.getX(), axisZ.getY(), axisZ.getZ(), 0.0f));
        if (Entity* entity = _light->entity()) {
            entity->setRotation(Quaternion::fromMatrix4(basis));
        }
        _light->setColor(sunColor);

        // The sun fades out below the horizon while the moon fades in; clouds dim the light below.
        const float sunPart = sunIntensity * saturate(y / 0.1f);
        const float moonPart = moonIntensity * nightFactor;
        const float dimming = hasClouds() ? cloudCoverage * cloudSunDimming : 0.0f;
        _light->setIntensity((sunPart + moonPart) * (1.0f - dimming));
    }

    void DynamicSkyEffect::updateNight()
    {
        _block.night[3] = saturate((2.0f - elevation) / 10.0f);
        _block.night[0] = nightColor.r * nightBrightness;
        _block.night[1] = nightColor.g * nightBrightness;
        _block.night[2] = nightColor.b * nightBrightness;
        _block.nightParams[0] = starBrightness;
        _block.nightParams[1] = starDensity;
        _block.nightParams[2] = starSize;
        _block.nightParams[3] = twilightGlow;
        const Vector3 moon = computeMoonDir();
        store3(_block.moonDir, Vector3(-moon.getX(), moon.getY(), moon.getZ()));
        _block.moonParams[0] = moonColor.r * moonGlow;
        _block.moonParams[1] = moonColor.g * moonGlow;
        _block.moonParams[2] = moonColor.b * moonGlow;
        _block.moonParams[3] = std::cos(moonSize * kDegToRad);
    }

    void DynamicSkyEffect::updateClouds()
    {
        _block.cloudParams[0] = hasClouds() ? saturate(cloudCoverage) : 0.0f;
        _block.cloudParams[1] = std::max(cloudSoftness, 0.01f);
        _block.cloudParams[2] = 1000.0f / std::max(cloudSize, 1.0f);
        _block.cloudParams[3] = cloudHeight / 1000.0f;
        _block.cloudShading[0] = saturate(cloudOpacity);
        _block.cloudShading[2] = cloudQualityValue(kCloudQualityFineStep, cloudQuality);
        _block.cloudVolume[0] = cloudHeight / 1000.0f;
        _block.cloudVolume[1] = (cloudHeight + std::max(cloudThickness, 10.0f)) / 1000.0f;
        _block.cloudVolume[2] = std::min(std::round(cloudQualityValue(kCloudQualitySteps, cloudQuality)),
            static_cast<float>(kCloudMaxSteps));
        _block.cloudVolume[3] = std::min(std::round(cloudQualityValue(kCloudQualityLightSteps, cloudQuality)),
            static_cast<float>(kCloudMaxLightSteps));
    }

    void DynamicSkyEffect::fillSettings(std::array<float, 32>& out) const
    {
        out = {rayleigh, turbidity, mieCoefficient, mieDirectionalG, luminance,
            static_cast<float>(lightingResolution), static_cast<float>(atlasSize), nightColor.r, nightColor.g,
            nightColor.b, nightBrightness, twilightGlow, cloudType == CloudType::Flat ? 1.0f : 0.0f,
            cloudType == CloudType::Volumetric ? 1.0f : 0.0f, cloudCoverage, cloudSoftness, cloudOpacity,
            cloudBrightness, cloudNightBrightness, cloudSize, cloudHeight, cloudThickness, cloudQuality, sunIntensity,
            moonColor.r, moonColor.g, moonColor.b};
    }

    bool DynamicSkyEffect::settingsChanged(const float tolerance) const
    {
        for (size_t i = 0; i < _settings.size(); ++i) {
            const float value = _settings[i];
            const float baked = _bakedSettings[i];
            if (std::abs(value - baked) > tolerance * std::max(std::abs(value), std::abs(baked))) {
                return true;
            }
        }
        return false;
    }

    void DynamicSkyEffect::update()
    {
        // The sun in world space, and in sky space, which flips X.
        _sunDir = computeSunDir();
        _skyDir = Vector3(-_sunDir.getX(), _sunDir.getY(), _sunDir.getZ());
        store3(_block.sunDir, _skyDir);

        updateAtmosphere();
        updateNight();
        updateLight();
        updateClouds();

        // The sky's variant and its textures.
        if (_material && _noise) {
            const bool buffered = _cloudPass != nullptr;
            std::string variant = "sky";
            std::array<Texture*, 5> textures{};
            if (cloudType == CloudType::Flat) {
                variant = "sky-flat";
                textures[0] = _noise->cloudMap();
            } else if (cloudType == CloudType::Volumetric && buffered) {
                variant = _cloudEncoded ? "sky-buffer-encoded" : "sky-buffer";
                textures[4] = _cloudTexture.get();
            } else if (cloudType == CloudType::Volumetric) {
                variant = "sky-volumetric";
                textures = {_noise->cloudMap(), _noise->shapeMap(), _noise->detailMap(), _noise->blueNoise(), nullptr};
            }
            _skyVariant = variant;
            _material->setVariant(variant);
            _material->setTextures(textures);
        }

        // The lighting is due when the sun or the moon moved enough, or a setting changed by more
        // than a small fraction since it was generated: settings animated every frame generate it
        // as their change adds up.
        fillSettings(_settings);
        _moonWorldDir = computeMoonDir();
        const float cosine = std::min(_skyDir.dot(_bakedSkyDir), _moonWorldDir.dot(_bakedMoonDir));
        const bool moved = cosine < std::cos(lightingThreshold * kDegToRad);
        _bakeDue = moved || settingsChanged(kSettingsTolerance);
        // A change too large to come from the sky's gradual change applies at once.
        _bakeJump = cosine < std::cos(lightingThreshold * kJumpThresholds * kDegToRad) || settingsChanged(kJumpTolerance);

        // The atlas is the scene's before the frame renders, so the materials are set up for it;
        // its content is generated before the scene renders.
        if (lighting && _device) {
            createLightingTargets();
            if (_scene && _envAtlas && _scene->envAtlas() != _envAtlas.get()) {
                _scene->setEnvAtlas(_envAtlas.get());
            }
        }
    }

    void DynamicSkyEffect::frameUpdate(const CameraFrameEffectContext& context)
    {
        CameraComponent* cameraComponent = camera();
        Camera* cam = cameraComponent ? cameraComponent->camera() : nullptr;
        GraphNode* cameraNode = cam ? cam->node() : nullptr;
        if (!cam || !cameraNode) {
            return;
        }

        // The clouds drift with the wind, in pattern repeats of sky space (X flipped). The shapes
        // wrap after four repeats, the weather's; the detail drifts a little faster and in a
        // slightly different direction, so the clouds evolve as they move.
        if (hasClouds() && context.dt > 0.0f) {
            const float angle = windDirection * kDegToRad;
            const float step = (windSpeed * context.dt) / std::max(cloudSize, 1.0f);
            _block.cloudOffset[0] = wrap(_block.cloudOffset[0] + std::cos(angle) * step, 4.0f);
            _block.cloudOffset[1] = wrap(_block.cloudOffset[1] - std::sin(angle) * step, 4.0f);
            _block.cloudOffset[2] = wrap(_block.cloudOffset[2] + std::cos(angle + 0.2f) * step * kCloudDetailScale * 1.3f, 1.0f);
            _block.cloudOffset[3] = wrap(_block.cloudOffset[3] - std::sin(angle + 0.2f) * step * kCloudDetailScale * 1.3f, 1.0f);
            _driftSinceBake += std::abs(step);
        }

        // Accumulated by TAA, the clouds' steps are offset differently every frame.
        ++_frameIndex;
        _block.cloudShading[3] = cam->jitter() > 0.0f ? std::fmod(static_cast<float>(_frameIndex) * 0.618034f, 1.0f) : 0.0f;

        // World to sky space: the scene's sky rotation (the one its own sky and lighting use).
        const Quaternion skyRotation = _scene ? _scene->skyboxRotation() : Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
        const Matrix4 r = Matrix4::trs(Vector3(0.0f), skyRotation, Vector3(1.0f));
        storeColumns(_block.skyRotation, column3(r, 0), column3(r, 1), column3(r, 2));

        // The volumetric clouds' texture at their resolution, and the camera they render for.
        const Matrix4& world = cameraNode->worldTransform();
        if (_cloudTarget) {
            const float scale = std::clamp(cloudResolution, 0.25f, 1.0f);
            const int width = std::max(1, static_cast<int>(std::lround(static_cast<float>(context.renderWidth) * scale)));
            const int height = std::max(1, static_cast<int>(std::lround(static_cast<float>(context.renderHeight) * scale)));
            if (static_cast<int>(_cloudTarget->width()) != width || static_cast<int>(_cloudTarget->height()) != height) {
                _cloudTarget->resize(width, height);
            }
            const Matrix4& projection = cam->projectionMatrix();
            const float p00 = projection.getElement(0, 0);
            const float p11 = projection.getElement(1, 1);
            _block.bufferProj[0] = p00;
            _block.bufferProj[1] = p11;
            _block.bufferProj[2] = 1.0f / static_cast<float>(width);
            _block.bufferProj[3] = 1.0f / static_cast<float>(height);

            // A texel's view ray (ndc / scale, -1) to sky space: R x the camera's rotation.
            const Matrix4 rayToSky = r * world;
            storeColumns(_block.bufferRay, column3(rayToSky, 0) * (1.0f / p00), column3(rayToSky, 1) * (1.0f / p11),
                column3(rayToSky, 2));
            // Sky space back to the camera's view space: the view's rotation x R^T.
            const Matrix4 skyToView = world.inverse() * r.inverse();
            storeColumns(_block.bufferView, column3(skyToView, 0), column3(skyToView, 1), column3(skyToView, 2));
        }

        // The sky mesh follows the camera.
        if (_node) {
            _node->setPosition(world.getTranslation());
        }

        // Without tone mapping in the frame (a gamma target) the sky exposes and tone maps itself.
        if (_scene) {
            _block.output[0] = _scene->exposureFor(cam);
            const int toneMapping = cameraComponent->toneMapping() == TONEMAP_INHERIT
                ? _scene->toneMapping() : cameraComponent->toneMapping();
            _block.output[1] = static_cast<float>(toneMapping);
        }

        // The lighting is generated before the scene renders on the frames it is due, and blends on
        // the frames after while it is interpolated.
        _timeSinceBake += context.dt;
        _frameDt = context.dt;
        const bool drift = hasClouds() && lightingCloudThreshold > 0.0f && _driftSinceBake >= lightingCloudThreshold;
        _bakePending = lighting && (_bakeRequested || _bakeDue || drift);

        if (_material) {
            _material->setBlock(_block);
        }
    }

    void DynamicSkyEffect::createLightingTargets()
    {
        GraphicsDevice* device = _device.get();
        if (!device) {
            return;
        }
        const int size = std::max(8, lightingResolution);
        if (!_lightingCube || static_cast<int>(_lightingCube->width()) != size) {
            _lightingCube.reset();
            _faceTargets = {};
            PixelFormat format = PixelFormat::PIXELFORMAT_RGBA16F;
            if (!device->textureHalfFloatRenderable()) {
                if (!device->textureFloatRenderable()) {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        spdlog::warn("DynamicSkyEffect: the device renders to no float format; no sky lighting");
                    }
                    return;
                }
                format = PixelFormat::PIXELFORMAT_RGBA32F;
            }
            TextureOptions options;
            options.name = "DynamicSkyLightingSource";
            options.width = static_cast<uint32_t>(size);
            options.height = static_cast<uint32_t>(size);
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
                targetOptions.name = "DynamicSkyLightingFace";
                _faceTargets[static_cast<size_t>(face)] = device->createRenderTarget(targetOptions);
            }
            _bakeRequested = true;
        }

        // Environment atlases in the engine's layout and encoding (EnvLighting::generateAtlas).
        const auto createAtlas = [device](const char* name, const int atlasSize) {
            TextureOptions options;
            options.name = name;
            options.width = static_cast<uint32_t>(atlasSize);
            options.height = static_cast<uint32_t>(atlasSize);
            options.format = PixelFormat::PIXELFORMAT_RGBA8;
            options.mipmaps = false;
            options.minFilter = FilterMode::FILTER_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;
            auto texture = std::make_shared<Texture>(device, options);
            texture->setEncoding(TextureEncoding::RGBP);
            texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            // Zeroed, not left uninitialised until the first generation: the atlas is the scene's
            // from the frame it is created, and a camera that renders before the effect's camera
            // in that frame (a planar reflection) samples it before it is generated.
            const std::vector<uint8_t> zeros(static_cast<size_t>(atlasSize) * static_cast<size_t>(atlasSize) * 4u, 0u);
            texture->setLevelData(0, zeros.data(), zeros.size());
            texture->upload();
            return texture;
        };
        const auto createTarget = [device](Texture* texture, const char* name) {
            RenderTargetOptions options;
            options.graphicsDevice = device;
            options.colorBuffer = texture;
            options.depth = false;
            options.name = name;
            return device->createRenderTarget(options);
        };
        const int atlasSizeNow = std::max(64, atlasSize);
        if (!_envAtlas || static_cast<int>(_envAtlas->width()) != atlasSizeNow) {
            if (_scene && _envAtlas && _scene->envAtlas() == _envAtlas.get()) {
                _scene->setEnvAtlas(nullptr);
            }
            _envAtlasTarget.reset();
            _envAtlas = createAtlas("DynamicSkyEnvAtlas", atlasSizeNow);
            _bakeRequested = true;
        }

        // Interpolated, the lighting is generated into an atlas of its own and blended from the
        // previous one into the scene's, which stays the scene's: another atlas would set up
        // every material again.
        if (lightingInterpolation) {
            if (!_atlasLatest || static_cast<int>(_atlasLatest->width()) != atlasSizeNow) {
                _atlasLatest = createAtlas("DynamicSkyEnvAtlasLatest", atlasSizeNow);
                _atlasPrevious = createAtlas("DynamicSkyEnvAtlasPrevious", atlasSizeNow);
                _atlasPreviousTarget = createTarget(_atlasPrevious.get(), "DynamicSkyEnvAtlasPrevious");
                _blendWeight = 1.0f;
                _bakeRequested = true;
            }
            if (!_envAtlasTarget) {
                _envAtlasTarget = createTarget(_envAtlas.get(), "DynamicSkyEnvAtlas");
            }
        } else if (_atlasLatest) {
            // The scene's atlas may show a blend in progress: generated again.
            _atlasPreviousTarget.reset();
            _atlasPrevious.reset();
            _atlasLatest.reset();
            _blendWeight = 1.0f;
            _bakeRequested = true;
        }
    }

    void DynamicSkyEffect::destroyLightingTargets()
    {
        _faceTargets = {};
        _lightingCube.reset();
        _envAtlasTarget.reset();
        _atlasPreviousTarget.reset();
        _atlasPrevious.reset();
        _atlasLatest.reset();
        _envAtlas.reset();
    }

    void DynamicSkyEffect::drawQuad(const std::shared_ptr<RenderTarget>& target, const std::string& variant,
        const std::array<Texture*, 4>& inputs)
    {
        GraphicsDevice* device = _device.get();
        if (!device || !target) {
            return;
        }
        QuadRender quad(getOrCreateSlangShader(device, "dynamic-sky", variant));
        for (size_t i = 0; i < inputs.size(); ++i) {
            quad.setTexture(i, inputs[i]);
        }
        quad.setUniforms(_block);
        LightingQuadPass pass(device);
        pass.init(target);
        device->beginOfflineWork();
        device->startRenderPass(&pass);
        device->setBlendState(BlendState::noBlend());
        device->setDepthState(DepthState::noDepth());
        device->setCullMode(CullMode::CULLFACE_NONE);
        device->setStencilState();
        // The noise repeats, through its own samplers on both backends.
        for (size_t i = 0; i < inputs.size(); ++i) {
            device->setQuadTextureOwnSampler(i, inputs[i] != nullptr);
        }
        quad.render();
        device->endRenderPass(&pass);
        device->endOfflineWork();
    }

    void DynamicSkyEffect::bake()
    {
        GraphicsDevice* device = _device.get();
        createLightingTargets();
        if (!device || !_lightingCube || !_envAtlas || !_noise) {
            return;
        }

        // Interpolated, a gradual change of the sky blends in over the time since the previous
        // update: the time the next one is due in, if the sky keeps changing at its rate. The blend
        // continues from the lighting shown, so a blend in progress carries on smoothly.
        const bool interpolated = lightingInterpolation && _atlasLatest;
        const bool blending = interpolated && !_bakeRequested && !_bakeJump && _timeSinceBake > 0.0f;
        if (blending) {
            drawQuad(_atlasPreviousTarget, "copy", {nullptr, _envAtlas.get(), nullptr, nullptr});
        }
        Texture* atlas = interpolated ? _atlasLatest.get() : _envAtlas.get();

        // The sky into the cube's faces, then its mips and the atlas.
        const char* variant = !hasClouds() ? "bake"
            : (cloudType == CloudType::Flat ? "bake-flat" : "bake-volumetric");
        const std::array<Texture*, 4> inputs = !hasClouds() ? std::array<Texture*, 4>{}
            : (cloudType == CloudType::Flat
                ? std::array<Texture*, 4>{_noise->cloudMap(), nullptr, nullptr, nullptr}
                : std::array<Texture*, 4>{_noise->cloudMap(), _noise->shapeMap(), _noise->detailMap(), _noise->blueNoise()});
        const float savedFace = _block.cloudLightDir[3];
        for (int face = 0; face < 6; ++face) {
            _block.cloudLightDir[3] = static_cast<float>(face);
            drawQuad(_faceTargets[static_cast<size_t>(face)], variant, inputs);
        }
        _block.cloudLightDir[3] = savedFace;
        device->beginOfflineWork();
        device->generateMipmaps(_lightingCube.get());
        device->endOfflineWork();
        EnvLighting::generateAtlas(device, _lightingCube.get(), atlas, static_cast<int>(atlas->width()),
            kReflectionSamples, kAmbientSamples);

        // The blend starts, or the latest lighting applies at once.
        if (blending) {
            _blendWeight = 0.0f;
            _blendDuration = std::min(_timeSinceBake, kBlendMaxTime);
        } else if (interpolated) {
            drawQuad(_envAtlasTarget, "copy", {nullptr, _atlasLatest.get(), nullptr, nullptr});
            _blendWeight = 1.0f;
        }

        // What the lighting was generated for, and when.
        _timeSinceBake = 0.0f;
        _bakeRequested = false;
        _bakeDue = false;
        _driftSinceBake = 0.0f;
        _bakedSkyDir = _skyDir;
        _bakedMoonDir = _moonWorldDir;
        _bakedSettings = _settings;
        ++_bakeCount;
    }

    void DynamicSkyEffect::blend()
    {
        if (!_envAtlasTarget || !_atlasPrevious || !_atlasLatest) {
            return;
        }
        _block.output[3] = _blendWeight;
        drawQuad(_envAtlasTarget, "blend", {_atlasPrevious.get(), _atlasLatest.get(), nullptr, nullptr});
    }

    void DynamicSkyEffect::updateLighting()
    {
        if (_bakePending) {
            bake();
            _bakePending = false;
        }
        // The blend advances with the frame's time; with the time paused there is no gradual
        // change to follow, so it is done at once.
        if (_blendWeight < 1.0f) {
            const float step = _frameDt > 0.0f && _blendDuration > 0.0f ? _frameDt / _blendDuration : 1.0f;
            _blendWeight = std::min(1.0f, _blendWeight + step);
            blend();
        }
    }
}
