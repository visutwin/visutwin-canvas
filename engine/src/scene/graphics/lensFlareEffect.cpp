// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.10.2026
//
#include "lensFlareEffect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "colorTarget.h"
#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/componentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "renderPassShaderQuad.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    namespace
    {
        // The depth the source of a directional light is tested at, as a fraction of the far
        // clip: the sky renders at the far clip, so anything nearer covers the sun.
        constexpr float kDirectionalDepth = 0.98f;

        // The vertex layout both drawn passes use: the standard 56-byte one (position, normal,
        // uv0, tangent, uv1), whose attributes carry the per-element and per-flare values.
        constexpr int kVertexFloats = 14;
        constexpr int kVertexBytes = kVertexFloats * static_cast<int>(sizeof(float));

        struct LensFlareUniforms
        {
            float camera[4];   // near, far, orthographic, depth height / width
            float params[4];   // x: 1 when there is no previous measurement; y: sRGB shapes;
                               // z: the shape library's resolution
        };

        float smoothstep(const float edge0, const float edge1, const float x)
        {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        void pushVertex(std::vector<float>& out, const float px, const float py, const std::array<float, 3>& a,
            const float u, const float v, const std::array<float, 4>& b)
        {
            const float vertex[kVertexFloats] = {px, py, 0.0f, a[0], a[1], a[2], u, v, b[0], b[1], b[2], b[3],
                0.0f, 0.0f};
            out.insert(out.end(), vertex, vertex + kVertexFloats);
        }

        // The compose chunk: the flares added to the scene at the hdr slot.
        const char* composeChunk()
        {
            return R"(
float3 applyLensFlare(float3 color, float2 uv)
{
    return color + lensFlareTexture.SampleLevel(quadSampler, uv, 0.0).rgb * lensFlareIntensity;
}

// The 'lensflare' debug view: the flares as they are added to the scene.
float3 debugLensflare()
{
    return lensFlareTexture.SampleLevel(quadSampler, composeDebugUv, 0.0).rgb * lensFlareIntensity;
}
)";
        }
    }

    // ---- LensFlare

    LensFlare::LensFlare(std::shared_ptr<const LensFlareLook> look, const LensFlareSource& source)
        : _look(std::move(look))
    {
        setSource(source);
    }

    void LensFlare::setLook(std::shared_ptr<const LensFlareLook> look)
    {
        _look = std::move(look);
        ++_version;
    }

    void LensFlare::setSource(const LensFlareSource& source)
    {
        _source = source;
        _lightWatch.watch(_source.light ? _source.light->entity() : nullptr, [this] { _source.light = nullptr; });
        _entityWatch.watch(_source.entity, [this] { _source.entity = nullptr; });
    }

    // ---- The geometry a drawn pass takes this frame: a ring of vertex buffers, one per frame in
    // flight, each written once a frame, so a buffer is rewritten only once the frame that drew
    // it has finished (on Metal the GPU reads the CPU's copy directly).

    class LensFlareEffect::Geometry
    {
    public:
        explicit Geometry(GraphicsDevice* device)
            : _device(device), _format(std::make_shared<VertexFormat>(kVertexBytes, VertexFormat::standardElements()))
        {
        }

        std::shared_ptr<VertexBuffer> upload(const std::vector<float>& vertices)
        {
            const int count = static_cast<int>(vertices.size()) / kVertexFloats;
            if (!_device || count == 0) {
                return nullptr;
            }
            const size_t frames = static_cast<size_t>(std::max(_device->maxFramesInFlight(), 1));
            if (_ring.size() < frames) {
                _ring.resize(frames);
            }
            Slot& slot = _ring[static_cast<size_t>(_device->renderVersion()) % _ring.size()];
            const size_t bytes = vertices.size() * sizeof(float);
            if (!slot.buffer || slot.capacity < count) {
                const int capacity = std::max(count, slot.capacity * 2);
                VertexBufferOptions options;
                options.usage = BUFFER_DYNAMIC;
                options.data.assign(static_cast<size_t>(capacity) * kVertexBytes, 0);
                std::memcpy(options.data.data(), vertices.data(), bytes);
                slot.buffer = _device->createVertexBuffer(_format, capacity, options);
                slot.capacity = slot.buffer ? capacity : 0;
            } else if (!slot.buffer->writeRange(0, vertices.data(), bytes)) {
                slot.buffer.reset();
                slot.capacity = 0;
            }
            return slot.buffer;
        }

    private:
        struct Slot
        {
            std::shared_ptr<VertexBuffer> buffer;
            int capacity = 0;
        };
        GraphicsDevice* _device = nullptr;
        std::shared_ptr<VertexFormat> _format;
        std::vector<Slot> _ring;
    };

    // ---- One pass: a variant of lens-flare.slang with its two inputs, its uniform block and,
    // for the occlusion and sprite variants, the geometry of this frame.

    class LensFlareEffect::Pass final : public RenderPassShaderQuad
    {
    public:
        Pass(const std::shared_ptr<GraphicsDevice>& device, std::string variant)
            : RenderPassShaderQuad(device), _variant(std::move(variant))
        {
            _name = "RenderPassLensFlare-" + _variant;
        }

        void setInputs(Texture* input0, Texture* input1)
        {
            _inputs = {input0, input1};
        }
        void setUniforms(const LensFlareUniforms& uniforms) { _uniforms = uniforms; }
        void setVertices(std::shared_ptr<VertexBuffer> buffer, const int count)
        {
            _vertices = std::move(buffer);
            _vertexCount = _vertices ? count : 0;
        }
        /// A pass that only clears its target (a shape with no texture).
        void setClearOnly(const bool value) { _clearOnly = value; }

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangShader("lens-flare", _variant);
            }
        }

        void execute() override
        {
            prepareShaders();
            if (!shader() || _clearOnly) {
                return;
            }
            const bool ownGeometry = _variant != "shape";
            if (ownGeometry && _vertexCount == 0) {
                return;   // the target is still cleared
            }
            if (!ownGeometry && !_inputs[0]) {
                return;
            }
            setQuadTextureBinding(0, _inputs[0]);
            setQuadTextureBinding(1, _inputs[1]);
            setQuadUniforms(_uniforms);
            setGeometry(ownGeometry ? _vertices : nullptr, ownGeometry ? _vertexCount : 0);
            RenderPassShaderQuad::execute();
        }

    private:
        std::string _variant;
        std::array<Texture*, 2> _inputs{};
        LensFlareUniforms _uniforms{};
        std::shared_ptr<VertexBuffer> _vertices;
        int _vertexCount = 0;
        bool _clearOnly = false;
    };

    // ---- LensFlareEffect

    LensFlareEffect::LensFlareEffect()
        : CameraFrameEffect("lensFlare", CameraFrameEffectOptions{.slot = ComposeSlot::Hdr, .slang = composeChunk(),
              .debugViews = {"lensflare"}, .requiredResources = {FrameResource::Depth}})
    {
        // The chunk's inputs exist from the start, so it compiles before the first frame's passes.
        setTextureUniform("lensFlareTexture", nullptr);
        setUniform("lensFlareIntensity", intensity);
    }

    LensFlareEffect::~LensFlareEffect() = default;

    bool LensFlareEffect::active() const
    {
        return enabled() && intensity > 0.0f && !_flares.empty();
    }

    void LensFlareEffect::setShapes(LensFlareShapeLibrary library)
    {
        _library = std::move(library);
        _shapesDirty = true;
    }

    void LensFlareEffect::add(const std::shared_ptr<LensFlare>& flare)
    {
        if (flare && std::find(_flares.begin(), _flares.end(), flare) == _flares.end()) {
            _flares.push_back(flare);
        }
    }

    void LensFlareEffect::remove(const std::shared_ptr<LensFlare>& flare)
    {
        std::erase(_flares, flare);
    }

    int LensFlareEffect::libraryLayers() const
    {
        return std::max(static_cast<int>(_library.shapes.size()), 1);
    }

    int LensFlareEffect::libraryResolution() const
    {
        return std::clamp(_library.resolution, 16, 4096);
    }

    int LensFlareEffect::shapeLayer(const std::string& name) const
    {
        for (size_t i = 0; i < _library.shapes.size(); ++i) {
            if (_library.shapes[i].first == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    std::string LensFlareEffect::buildKey() const
    {
        // The shape array is built with the passes: its size and layer count rebuild them.
        return std::to_string(libraryResolution()) + "x" + std::to_string(libraryLayers());
    }

    void LensFlareEffect::createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes)
    {
        GraphicsDevice* device = resources.device.get();
        if (!device || !resources.depth) {
            return;
        }
        _device = resources.device;
        _depth = resources.depth;

        // The shape library: a mipmapped texture array, a layer per shape, each copied by a pass
        // of its own, the last one generating the mips of them all.
        TextureOptions shapeOptions;
        shapeOptions.name = "LensFlareShapes";
        shapeOptions.width = static_cast<uint32_t>(libraryResolution());
        shapeOptions.height = shapeOptions.width;
        shapeOptions.arrayLength = static_cast<uint32_t>(libraryLayers());
        shapeOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        shapeOptions.mipmaps = true;
        shapeOptions.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
        shapeOptions.magFilter = FilterMode::FILTER_LINEAR;
        _shapeTexture = std::make_shared<Texture>(device, shapeOptions);
        _shapeTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _shapeTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        for (int layer = 0; layer < libraryLayers(); ++layer) {
            RenderTargetOptions options;
            options.graphicsDevice = device;
            options.colorBuffer = _shapeTexture.get();
            options.depth = false;
            options.face = layer;
            options.name = "LensFlareShape" + std::to_string(layer);
            auto target = device->createRenderTarget(options);
            auto pass = std::make_shared<Pass>(resources.device, "shape");
            pass->init(target);
            const bool last = layer + 1 == libraryLayers();
            if (last) {
                pass->colorOps()->genMipmaps = true;
                // The copies are done once the last has rendered.
                pass->setOnAfter([this](RenderPass&) {
                    for (const auto& shapePass : _shapePasses) {
                        shapePass->setEnabled(false);
                    }
                });
            }
            _shapeTargets.push_back(target);
            _shapePasses.push_back(pass);
            passes.postResolve.push_back(pass);
        }
        _shapesDirty = true;

        // The visibility of each flare's source: measured into one texture from the other.
        for (size_t i = 0; i < _visibility.size(); ++i) {
            auto target = createColorTarget(device, "LensFlareVisibility" + std::to_string(i),
                PixelFormat::PIXELFORMAT_RGBA8, kMaxFlares, 1);
            target.texture->setMinFilter(FilterMode::FILTER_NEAREST);
            target.texture->setMagFilter(FilterMode::FILTER_NEAREST);
            _visibility[i] = target.texture;
            _visibilityTargets[i] = target.target;
        }
        _visibilityIndex = 0;
        _occlusionPass = std::make_shared<Pass>(resources.device, "occlusion");
        _occlusionPass->init(_visibilityTargets[0]);
        passes.postResolve.push_back(_occlusionPass);

        // The flares, added into a texture the composition reads.
        auto flare = createColorTarget(device, "LensFlareTexture", PixelFormat::PIXELFORMAT_RGBA16F,
            std::max(static_cast<int>(_depth->width() / 2), 1), std::max(static_cast<int>(_depth->height() / 2), 1));
        _flareTexture = flare.texture;
        _flareTarget = flare.target;
        _spritePass = std::make_shared<Pass>(resources.device, "sprite");
        _spritePass->init(_flareTarget);
        const Color black(0.0f, 0.0f, 0.0f, 1.0f);
        _spritePass->setClearColor(&black);
        static const auto additive = [] {
            auto blend = std::make_shared<BlendState>();
            blend->setEnabled(true);
            blend->setColorOp(BLENDEQUATION_ADD);
            blend->setColorSrcFactor(BLENDMODE_ONE);
            blend->setColorDstFactor(BLENDMODE_ONE);
            blend->setAlphaOp(BLENDEQUATION_ADD);
            blend->setAlphaSrcFactor(BLENDMODE_ONE);
            blend->setAlphaDstFactor(BLENDMODE_ONE);
            return blend;
        }();
        _spritePass->setBlendState(additive);
        passes.postResolve.push_back(_spritePass);

        _occlusionGeometry = std::make_unique<Geometry>(device);
        _spriteGeometry = std::make_unique<Geometry>(device);
        _firstMeasurement = true;
        setTextureUniform("lensFlareTexture", _flareTexture.get());
    }

    void LensFlareEffect::destroyPasses()
    {
        setTextureUniform("lensFlareTexture", nullptr);
        _shapePasses.clear();
        _shapeTargets.clear();
        _shapeTexture.reset();
        _occlusionPass.reset();
        _spritePass.reset();
        _visibility = {};
        _visibilityTargets = {};
        _flareTarget.reset();
        _flareTexture.reset();
        _occlusionGeometry.reset();
        _spriteGeometry.reset();
        _depth = nullptr;
        _shapesDirty = true;
        ++_shapesVersion;
    }

    void LensFlareEffect::update()
    {
        // Copy the shapes after the library changed (or the array was rebuilt). A library of
        // another size or layer count rebuilds the passes first (buildKey).
        if (_shapesDirty && !_shapePasses.empty() &&
            static_cast<int>(_shapePasses.size()) == libraryLayers() &&
            static_cast<int>(_shapeTexture->width()) == libraryResolution()) {
            for (size_t i = 0; i < _shapePasses.size(); ++i) {
                Texture* texture = i < _library.shapes.size() ? _library.shapes[i].second : nullptr;
                if (!texture && i < _library.shapes.size()) {
                    spdlog::warn("LensFlareEffect: shape '{}' has no texture; its layer is left empty",
                        _library.shapes[i].first);
                }
                _shapePasses[i]->setInputs(texture, nullptr);
                LensFlareUniforms shapeUniforms{};
                shapeUniforms.params[1] = _library.srgb ? 1.0f : 0.0f;
                _shapePasses[i]->setUniforms(shapeUniforms);
                // A shape with no texture clears its layer, so a layer of a previous library
                // does not keep its pixels.
                _shapePasses[i]->setClearOnly(texture == nullptr);
                const Color transparent(0.0f, 0.0f, 0.0f, 0.0f);
                _shapePasses[i]->setClearColor(texture ? nullptr : &transparent);
                _shapePasses[i]->setEnabled(true);
            }
            _shapesDirty = false;
            ++_shapesVersion;
        }

        // Rebuild the elements when the shapes, the flares or their looks changed.
        std::string key = std::to_string(_shapesVersion);
        for (const auto& flare : _flares) {
            key += "|" + std::to_string(reinterpret_cast<uintptr_t>(flare.get())) + ":" +
                std::to_string(flare->version());
        }
        if (key != _builtKey) {
            rebuildElements();
            _builtKey = key;
        }

        setUniform("lensFlareIntensity", intensity);
        if (_flareTexture) {
            setTextureUniform("lensFlareTexture", _flareTexture.get());
        }
    }

    void LensFlareEffect::rebuildElements()
    {
        _elements.clear();
        if (_library.shapes.empty()) {
            return;
        }
        static std::unordered_set<std::string> reported;
        const int flareCount = std::min(static_cast<int>(_flares.size()), kMaxFlares);
        if (static_cast<int>(_flares.size()) > kMaxFlares && reported.insert("#maxFlares").second) {
            spdlog::warn("LensFlareEffect: {} flares are registered; only the first {} are drawn", _flares.size(),
                kMaxFlares);
        }
        for (int i = 0; i < flareCount; ++i) {
            const auto& look = _flares[static_cast<size_t>(i)]->look();
            if (!look) {
                continue;
            }
            for (const LensFlareElement& element : look->elements) {
                const int layer = shapeLayer(element.shape);
                if (layer < 0) {
                    if (reported.insert(element.shape).second) {
                        spdlog::warn("LensFlareEffect: the shape library has no shape named '{}'; skipping the element",
                            element.shape);
                    }
                    continue;
                }
                const int copies = std::max(element.count, 1);
                for (int k = 0; k < copies; ++k) {
                    ElementInstance instance;
                    instance.flare = i;
                    instance.layer = layer;
                    instance.offset = copies > 1
                        ? element.offset + element.spread * (static_cast<float>(k) / static_cast<float>(copies - 1) - 0.5f)
                        : element.offset;
                    instance.size = element.size;
                    instance.aspect = element.aspect;
                    instance.rotation = element.rotation * DEG_TO_RAD;
                    instance.autoRotate = element.autoRotate;
                    instance.r = element.color.r * element.intensity;
                    instance.g = element.color.g * element.intensity;
                    instance.b = element.color.b * element.intensity;
                    _elements.push_back(instance);
                }
            }
        }
    }

    void LensFlareEffect::frameUpdate(const CameraFrameEffectContext& context)
    {
        CameraComponent* cameraComponent = camera();
        Camera* cam = cameraComponent ? cameraComponent->camera() : nullptr;
        GraphNode* node = cam ? cam->node() : nullptr;
        if (!cam || !node || !_occlusionPass || !_spritePass || !_depth) {
            return;
        }

        const Matrix4& world = node->worldTransform();
        const Matrix4 view = world.inverse();
        const Matrix4 viewProjection = cam->projectionMatrix() * view;
        const Vector3 cameraPosition = world.getTranslation();
        const float farClip = cam->farClip();
        const bool orthographic = cam->projection() == ProjectionType::Orthographic;
        const Engine* engine = cameraComponent->system() ? cameraComponent->system()->engine() : nullptr;
        const bool physicalUnits = engine && engine->scene() && engine->scene()->physicalUnits();

        // The flare texture follows the scene at `scale`.
        const int renderWidth = context.renderWidth > 0 ? context.renderWidth : static_cast<int>(_depth->width());
        const int renderHeight = context.renderHeight > 0 ? context.renderHeight : static_cast<int>(_depth->height());
        const float s = std::clamp(scale, 0.1f, 1.0f);
        const int flareWidth = std::max(static_cast<int>(std::lround(static_cast<float>(renderWidth) * s)), 1);
        const int flareHeight = std::max(static_cast<int>(std::lround(static_cast<float>(renderHeight) * s)), 1);
        if (static_cast<int>(_flareTarget->width()) != flareWidth || static_cast<int>(_flareTarget->height()) != flareHeight) {
            _flareTarget->resize(flareWidth, flareHeight);
        }

        const int flareCount = std::min(static_cast<int>(_flares.size()), kMaxFlares);
        _states.assign(static_cast<size_t>(flareCount), FlareState{});
        std::vector<float> occlusion;
        occlusion.reserve(static_cast<size_t>(kMaxFlares) * 6 * kVertexFloats);

        for (int i = 0; i < kMaxFlares; ++i) {
            // Every texel is written every frame; one with no flare keeps its visibility.
            std::array<float, 3> source{0.0f, 0.0f, 0.0f};
            std::array<float, 2> control{0.0f, 0.0f};   // radius, blend
            float onScreenFlag = 0.0f;

            if (i < flareCount) {
                const LensFlare& flare = *_flares[static_cast<size_t>(i)];
                FlareState& state = _states[static_cast<size_t>(i)];
                float fade = flare.enabled ? 1.0f : 0.0f;

                bool directional = false;
                bool hasSource = true;
                Vector3 position;
                Vector3 direction;
                const LensFlareSource& src = flare.source();
                LightComponent* light = src.light;
                if (light) {
                    Entity* entity = light->entity();
                    if (!light->active()) {
                        fade = 0.0f;
                    }
                    // A light shines along the negative y axis of its entity.
                    const Matrix4& lightWorld = entity->worldTransform();
                    const Vector4 up = lightWorld.getColumn(1);
                    const Vector3 axis = Vector3(up.getX(), up.getY(), up.getZ()).normalized();
                    if (light->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                        directional = true;
                        direction = axis;
                    } else {
                        position = lightWorld.getTranslation();
                        // A spot light flares within its cone.
                        if (light->type() == LightType::LIGHTTYPE_SPOT) {
                            const float cosOuter = std::cos(light->outerConeAngle() * DEG_TO_RAD);
                            const float cosInner = std::cos(light->innerConeAngle() * DEG_TO_RAD);
                            const Vector3 toCamera = (cameraPosition - position).normalized();
                            const float cosAngle = -toCamera.dot(axis);
                            fade *= cosInner > cosOuter ? smoothstep(cosOuter, cosInner, cosAngle)
                                                        : (cosAngle >= cosOuter ? 1.0f : 0.0f);
                        }
                    }
                } else if (src.entity) {
                    position = src.entity->worldTransform().getTranslation();
                } else if (src.position) {
                    position = *src.position;
                } else if (src.direction) {
                    directional = true;
                    direction = src.direction->normalized();
                } else {
                    hasSource = false;
                    fade = 0.0f;
                }

                // In front of the camera, tested in view space (clip w does not tell for an
                // orthographic projection).
                bool inFront = false;
                Vector4 clip;
                Vector3 viewPosition;
                if (hasSource && directional) {
                    const Vector4 viewDirection = view * Vector4(direction, 0.0f);
                    inFront = viewDirection.getZ() < 0.0f;
                    // An orthographic projection has no vanishing point for a direction: the
                    // source is placed far along it.
                    clip = orthographic
                        ? viewProjection * Vector4(cameraPosition + direction * (farClip * kDirectionalDepth), 1.0f)
                        : viewProjection * Vector4(direction, 0.0f);
                } else if (hasSource) {
                    viewPosition = view.transformPoint(position);
                    inFront = viewPosition.getZ() < 0.0f;
                    clip = viewProjection * Vector4(position, 1.0f);
                }

                float x = 0.0f;
                float y = 0.0f;
                onScreenFlag = -1.0f;
                if (inFront && std::abs(clip.getW()) > 1e-6f) {
                    x = clip.getX() / clip.getW();
                    y = clip.getY() / clip.getW();
                    // Fade out at the edge of the screen, and beyond it when allowed.
                    const float m = std::max(std::abs(x), std::abs(y));
                    const float edge = std::max(flare.edgeFade, 0.0f);
                    const float start = 1.0f - edge;
                    const float end = flare.allowOffScreen ? 1.0f + edge : 1.0f;
                    fade *= end > start ? 1.0f - smoothstep(start, end, m) : (m <= end ? 1.0f : 0.0f);
                    onScreenFlag = m <= 1.0f ? 1.0f : 0.0f;
                } else {
                    fade = 0.0f;
                }

                // The depth the visibility is tested at, and the distance fade.
                float sourceDepth = farClip * kDirectionalDepth;
                if (hasSource && !directional) {
                    sourceDepth = -viewPosition.getZ() - flare.occlusionOffset;
                    if (flare.fadeEnd > flare.fadeStart) {
                        fade *= 1.0f - smoothstep(flare.fadeStart, flare.fadeEnd, (position - cameraPosition).length());
                    }
                }

                // The colour, tinted by the light's (linear) and scaled by its intensity.
                float intensity = flare.intensity;
                if (light && flare.useLightIntensity) {
                    intensity *= light->renderIntensity(physicalUnits);
                }
                Color color(flare.color.r * intensity, flare.color.g * intensity, flare.color.b * intensity, 1.0f);
                if (light && flare.useLightColor) {
                    const Color& lightColor = light->color();
                    color.r *= gammaToLinear(lightColor.r);
                    color.g *= gammaToLinear(lightColor.g);
                    color.b *= gammaToLinear(lightColor.b);
                }

                state = {x, y, fade, inFront, onScreenFlag > 0.5f, sourceDepth, color};
                source = {x, y, sourceDepth};
                control = {flare.occlusionRadius,
                    flare.occlusionFade > 0.0f ? 1.0f - std::exp(-context.dt / flare.occlusionFade) : 1.0f};
            }

            // The flare's texel of the 32 x 1 visibility target.
            const float x0 = -1.0f + 2.0f * static_cast<float>(i) / kMaxFlares;
            const float x1 = -1.0f + 2.0f * static_cast<float>(i + 1) / kMaxFlares;
            const std::array<float, 4> b{onScreenFlag, static_cast<float>(i), 0.0f, 0.0f};
            for (const auto& [px, py] : {std::pair{x0, -1.0f}, {x1, -1.0f}, {x1, 1.0f}, {x0, -1.0f}, {x1, 1.0f},
                     {x0, 1.0f}}) {
                pushVertex(occlusion, px, py, source, control[0], control[1], b);
            }
        }

        // The elements, along the axis from their flare's source through the centre. Sizes are
        // in screen heights: x is divided by the aspect ratio so rotation and size keep their
        // shape, and multiplied back at the end.
        std::vector<float> sprites;
        sprites.reserve(_elements.size() * 6 * kVertexFloats);
        const float aspect = static_cast<float>(flareHeight) / static_cast<float>(flareWidth);
        _drawnElements = 0;
        for (const ElementInstance& element : _elements) {
            if (element.flare >= flareCount) {
                continue;
            }
            const FlareState& state = _states[static_cast<size_t>(element.flare)];
            if (state.fade <= 0.0001f) {
                continue;
            }
            const float flareScale = _flares[static_cast<size_t>(element.flare)]->scale;
            const float sourceX = state.x / aspect;
            const float sourceY = state.y;
            const float centerX = sourceX * (1.0f - element.offset);
            const float centerY = sourceY * (1.0f - element.offset);
            float rotation = element.rotation;
            if (element.autoRotate) {
                rotation += std::atan2(-sourceY, -sourceX);
            }
            const float c = std::cos(rotation);
            const float sn = std::sin(rotation);
            const std::array<float, 3> color{element.r * state.color.r * state.fade,
                element.g * state.color.g * state.fade, element.b * state.color.b * state.fade};
            const std::array<float, 4> b{static_cast<float>(element.layer), static_cast<float>(element.flare), 0.0f, 0.0f};
            for (const auto& [cx, cy] : {std::pair{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, -1.0f},
                     {1.0f, 1.0f}, {-1.0f, 1.0f}}) {
                const float px = cx * element.size * element.aspect * flareScale;
                const float py = cy * element.size * flareScale;
                const float rx = px * c - py * sn;
                const float ry = px * sn + py * c;
                // The top of the element samples the top row of its shape.
                pushVertex(sprites, (centerX + rx) * aspect, centerY + ry, color, cx * 0.5f + 0.5f, 0.5f - cy * 0.5f, b);
            }
            ++_drawnElements;
        }

        // Measure into one visibility texture from the other; the sprites read this frame's.
        _visibilityIndex = 1 - _visibilityIndex;
        Texture* measured = _visibility[static_cast<size_t>(_visibilityIndex)].get();
        Texture* previous = _visibility[static_cast<size_t>(1 - _visibilityIndex)].get();
        _occlusionPass->init(_visibilityTargets[static_cast<size_t>(_visibilityIndex)]);

        LensFlareUniforms uniforms{};
        uniforms.camera[0] = cam->nearClip();
        uniforms.camera[1] = farClip;
        uniforms.camera[2] = orthographic ? 1.0f : 0.0f;
        uniforms.camera[3] = _depth->width() > 0
            ? static_cast<float>(_depth->height()) / static_cast<float>(_depth->width()) : 1.0f;
        uniforms.params[0] = _firstMeasurement ? 1.0f : 0.0f;
        _firstMeasurement = false;
        _occlusionPass->setUniforms(uniforms);
        _occlusionPass->setInputs(_depth, previous);
        const auto occlusionBuffer = _occlusionGeometry->upload(occlusion);
        _occlusionPass->setVertices(occlusionBuffer, static_cast<int>(occlusion.size()) / kVertexFloats);

        LensFlareUniforms spriteUniforms{};
        spriteUniforms.params[2] = static_cast<float>(_shapeTexture->width());
        _spritePass->setUniforms(spriteUniforms);
        _spritePass->setInputs(_shapeTexture.get(), measured);
        const auto spriteBuffer = _spriteGeometry->upload(sprites);
        _spritePass->setVertices(spriteBuffer, static_cast<int>(sprites.size()) / kVertexFloats);
    }
}
