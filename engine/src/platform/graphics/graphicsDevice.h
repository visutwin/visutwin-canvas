// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "blendState.h"
#include "depthState.h"
#include "dynamicBuffers.h"
#include "gpuProfiler.h"
#include "indexBuffer.h"
#include "renderPass.h"
#include "renderTarget.h"
#include "shader.h"
#include "stencilParameters.h"
#include "instanceCuller.h"
#include "vertexBuffer.h"
#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "core/eventHandler.h"
#include "scene/mesh.h"

namespace visutwin::canvas
{
    /**
     * Size of the per-draw uniform slot (Metal buffer 3 / Vulkan set 0 binding 0).
     *
     * Sized for the LARGEST block any shader declares there, not just
     * MaterialUniforms: quad effects ride the same slot and some carry more —
     * volumetric fog's block is 512 bytes. On Vulkan this is the dynamic
     * descriptor's range, and the allocation behind it is padded to match, so a
     * shader can never read past its own allocation.
     */
    inline constexpr size_t kPerDrawUniformCapacity = 512;

    class Compute;
    class Texture;
    class Material;
    struct RenderTargetOptions;

    /// Emit a one-time warning that the active backend does not implement a device
    /// entry point. See VT_DEVICE_FEATURE_UNSUPPORTED.
    void logUnsupportedDeviceFeature(const char* name);

    /**
     * @brief Shader source language a backend's createShader() accepts.
     *
     * Engine code that carries its own shader source (the post-processing quad
     * passes, the outline extras) keeps one string per language and selects with
     * GraphicsDevice::shaderLanguage(). Handing a backend the wrong language is a
     * hard error, not a fallback.
     */
    enum class ShaderLanguage
    {
        Msl,
        Glsl,
    };

    /**
     * @brief Marks a GraphicsDevice entry point a backend has not implemented.
     *
     * The base-class bodies of the optional device virtuals are empty, so a backend
     * that never overrides one drops the feature without any diagnostic — the scene
     * renders, just missing volumetric fog, or the atmosphere, or clustered local
     * shadows. Placing this in the base body turns each such gap into a single
     * warning naming the entry point, on the first call and never again (these sit
     * on per-frame and per-draw paths).
     *
     * The guard is a function-local static, so each call site warns independently
     * and only once process-wide, regardless of how many devices exist.
     */
#define VT_DEVICE_FEATURE_UNSUPPORTED(name)                                        \
    do {                                                                           \
        static std::atomic_flag vtFeatureWarned = ATOMIC_FLAG_INIT;                \
        if (!vtFeatureWarned.test_and_set(std::memory_order_relaxed)) {            \
            ::visutwin::canvas::logUnsupportedDeviceFeature(name);                  \
        }                                                                          \
    } while (false)

    enum class GpuLightType : uint32_t
    {
        Directional = 0u,
        Point = 1u,
        Spot = 2u,
        AreaRect = 3u
    };

    /** @brief Per-light GPU data uploaded to the lighting uniform buffer.
     *  @ingroup group_scene_lighting */
    /// GPU particle: mirrors the MSL Particle struct (48 bytes, 12 floats).
    /// pos.xyz, age | vel.xyz, lifetime | rotation, rotSpeed, seed, size.
    /// age < 0 counts down to birth; age > lifetime on a non-looping emitter = dead.
    struct GpuParticle
    {
        float posAge[4];
        float velLifetime[4];
        float rotSeedSize[4];
    };
    static_assert(sizeof(GpuParticle) == 48);

    /// Mirrors the MSL ParticleSimParams struct (compute kernel, buffer 1).
    struct GpuParticleSimParams
    {
        Matrix4 emitterTransform;   // world transform for spawn (identity in local space)
        float gravityDamping[4];    // xyz = gravity, w = damping (fraction/s)
        float shapeParams[4];       // xyz = box half-extents or x=radius, w = shape type
        float velocityBase[4];      // xyz = base velocity, w = localSpace flag
        float velocitySpread[4];    // xyz = ± spread, w = loop flag
        float timeParams[4];        // dt, time, birth interval (s), particle count
        float lifeRot[4];           // lifetime min/max, rotSpeed min/max (radians/s)
        float angleParams[4];       // startAngle min/max (radians), seed, playing flag
    };
    static_assert(sizeof(GpuParticleSimParams) == 176);

    /// Mirrors the MSL ParticleRenderParams struct (vertex slot 11).
    struct GpuParticleRenderParams
    {
        Matrix4 modelView;
        Matrix4 projection;          // GL-style clip; z remapped in-shader
        float animParams[4];         // tilesX, tilesY, numFrames, animSpeed
        float miscParams[4];         // intensity, particle count, hasColorMap, pad
        float colorLut[16][4];       // rgb + alpha over normalized life
        float scaleLut[16][4];       // x = world size, yzw = pad
    };
    static_assert(sizeof(GpuParticleRenderParams) == 672);

    struct GpuLightData
    {
        GpuLightType type = GpuLightType::Directional;
        Vector3 position = Vector3(0.0f);
        Vector3 direction = Vector3(0.0f, -1.0f, 0.0f);
        Color color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        float intensity = 0.0f;
        float range = 0.0f;
        float innerConeCos = 1.0f;
        float outerConeCos = 1.0f;
        bool falloffModeLinear = true;
        bool castShadows = false;

        // Local light shadow data (spot/point).
        int shadowMapIndex = -1;    // -1 = no shadow, 0 = slot 11, 1 = slot 12

        // Light cookie (upstream Light.cookie): a projected texture masking the
        // light's color. 2D for spot lights, cubemap for omni; the two kinds have
        // separate slot pools, so cookieIndex is an index within the pool the
        // light's type selects. -1 = this light has no cookie.
        Texture* cookie = nullptr;
        int cookieIndex = -1;
        float cookieIntensity = 1.0f;
        uint32_t cookieChannel = 0u;   // CookieChannel: 0=rgb, 1=r, 2=g, 3=b, 4=a
        bool cookieFalloff = true;     // spot only — keep the cone falloff alongside the cookie
        // Spot: world → cookie UV projection. Omni: the light's world transform,
        // whose rotation takes the light→fragment vector into cookie cube space.
        Matrix4 cookieMatrix = Matrix4::identity();

        // Area light: half-extents, local right axis (world space) and shape
        // (0=rect, 1=disk, 2=sphere — mirrors AreaLightShape).
        float areaHalfWidth = 0.0f;
        float areaHalfHeight = 0.0f;
        Vector3 areaRight = Vector3(1.0f, 0.0f, 0.0f);
        uint32_t areaShape = 0u;
    };

    struct FogParams
    {
        bool enabled = false;
        Color color = Color(0.0f, 0.0f, 0.0f, 1.0f);
        float start = 10.0f;
        float end = 100.0f;
        float density = 0.01f;
    };

    struct ShadowParams
    {
        bool enabled = false;
        // True when the directional light uses SHADOW_VSM_16F: the shadow map
        // holds EVSM moments (RGBA16F) and `bias` is the vsmBias (minVariance
        // floor) instead of a depth offset. Metal selects the VSM sampling via
        // a shader variant; the Vulkan backend branches on this at runtime.
        bool vsm = false;
        // True when the directional light uses SHADOW_PCSS_32F (contact-hardening
        // soft shadows). The shadow map stays the standard depth texture; the
        // shader samples it raw (non-comparison) with a Vogel-disk blocker search.
        bool pcss = false;
        float penumbraSize = 1.0f;
        float penumbraFalloff = 1.0f;
        int pcssSamples = 16;
        int pcssBlockerSamples = 16;
        // Per-cascade ortho half-extent (world units) and caster depth range
        // (far - near) of the directional shadow cameras — the world-space PCSS
        // penumbra math needs both.
        float pcssCascadeRadii[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float pcssCascadeDepthRanges[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float bias = 0.001f;
        float normalBias = 0.0f;
        float strength = 1.0f;
        Texture* shadowMap = nullptr;

        // Single VP matrix for backward compat (cascade 0).
        Matrix4 viewProjection = Matrix4::identity();

        // CSM data (_shadowMatrixPalette, _shadowCascadeDistances).
        int numCascades = 1;
        float cascadeBlend = 0.0f;
        float shadowMatrixPalette[64] = {};      // 4 cascade VP matrices (viewport-scaled)
        float shadowCascadeDistances[4] = {};    // per-cascade split distances

        // Local light shadows (up to 2 simultaneous shadow-casting local lights).
        static constexpr int kMaxLocalShadows = 2;
        int localShadowCount = 0;
        struct LocalShadow {
            Texture* shadowMap = nullptr;
            Matrix4 viewProjection = Matrix4::identity();
            float bias = 0.0001f;
            float normalBias = 0.0f;
            float intensity = 1.0f;
            bool isOmni = false;    // true = cubemap shadow (omni), false = 2D shadow (spot)
            // PCSS (SHADOW_PCSS_32F on the light): blocker-search area in
            // shadow-map UV units (0 = PCSS off) + shadow camera clip range.
            float pcssSearchArea = 0.0f;
            float nearClip = 0.01f;
            float farClip = 100.0f;
        } localShadows[kMaxLocalShadows];
    };

    struct SsaoPassParams
    {
        Texture* depthTexture = nullptr;
        float aspect = 1.0f;
        float invResolutionX = 0.0f;
        float invResolutionY = 0.0f;
        int sampleCount = 12;
        float spiralTurns = 10.0f;
        float angleIncCos = 0.0f;
        float angleIncSin = 0.0f;
        float invRadiusSquared = 0.0f;
        float minHorizonAngleSineSquared = 0.0f;
        float bias = 0.001f;
        float peak2 = 0.0f;
        float intensity = 0.0f;
        float power = 6.0f;
        float projectionScaleRadius = 0.0f;
        float randomize = 0.0f;
        float cameraNear = 0.1f;
        float cameraFar = 1000.0f;
    };

    // VSM separable gaussian blur (upstream blurVSM equivalent).
    // Operates on the RGB channels of a 2D RGBA16F moments texture.
    // Run twice per shadow update — once horizontal, once vertical.
    struct EnvReprojectOp
    {
        int rectX = 0;
        int rectY = 0;
        int rectW = 0;
        int rectH = 0;
        int seamPixels = 1;
    };

    struct EnvReprojectPassParams
    {
        Texture* target = nullptr;
        Texture* sourceEquirect = nullptr;   // bound when sourceProjection != CUBE
        Texture* sourceCubemap  = nullptr;   // bound when sourceProjection == CUBE
        std::vector<EnvReprojectOp> ops;
        // How to read the source and how to lay out the target. CUBE is only valid as a
        // source; a cube TARGET is produced face-by-face by the caller instead.
        TextureProjection sourceProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
        TextureProjection targetProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
        bool encodeRgbp = true;
        bool decodeSrgb = false;
    };

    // Importance-sampled convolution. `samples` is an array of numSamples
    // float4s: (tangentX, tangentY, tangentZ, mipLevel). For Lambert the
    // tangent vector is the hemisphere sample; for GGX it is the reflected
    // direction. A sample with tangentZ <= 0 is treated as invalid and
    // skipped by the shader.
    struct EnvConvolveOp
    {
        int rectX = 0;
        int rectY = 0;
        int rectW = 0;
        int rectH = 0;
        int seamPixels = 1;
        const float* samples = nullptr;
        int numSamples = 0;
        bool weightByNoL = false;
    };

    struct EnvConvolvePassParams
    {
        Texture* target = nullptr;
        Texture* sourceEquirect = nullptr;
        Texture* sourceCubemap  = nullptr;
        std::vector<EnvConvolveOp> ops;
        bool encodeRgbp = true;
        bool decodeSrgb = false;
    };

    // Combined bake — reproject ops run first, then convolve ops, all inside
    // a single render pass. Required on Apple-Silicon tile-based GPUs where
    // splitting the atlas bake across multiple render passes loses content
    // outside the last pass's scissor.
    //
    // reprojectSource is the source for the reproject ops (straight resample);
    // convolveSource is the source for the convolve ops (usually a mipmapped
    // HDR cubemap). They may differ.
    struct EnvAtlasBakeParams
    {
        Texture* target = nullptr;
        Texture* reprojectSourceEquirect = nullptr;
        Texture* reprojectSourceCubemap  = nullptr;
        TextureProjection reprojectSourceProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
        TextureProjection reprojectTargetProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
        std::vector<EnvReprojectOp> reprojectOps;
        Texture* convolveSourceEquirect = nullptr;
        Texture* convolveSourceCubemap  = nullptr;
        std::vector<EnvConvolveOp> convolveOps;
        bool encodeRgbp = true;
        bool decodeSrgb = false;
    };

    // Builds a 6-face cubemap from an equirect source and generates its mip
    // chain. `target` must already be created as a cubemap texture with
    // mipmaps enabled.
    struct EquirectToCubeParams
    {
        Texture* source = nullptr;
        Texture* target = nullptr;
        bool decodeSrgb = false;
    };

    // DEVIATION: blurred planar reflection parameters.
    // Upstream implements these as per-material parameters on the BlurredPlanarReflection script;
    // we promote them to device-level so the forward pass can read them from LightingData.
    struct ReflectionBlurParams
    {
        float intensity = 1.0f;       // Reflection intensity (0..1). 0 = no reflection visible.
        float blurAmount = 0.0f;      // Blur radius multiplier (0..2). 0 = sharp, 1+ = blurry.
        float fadeStrength = 1.0f;    // Distance-based fade strength (0..5). Higher = quicker fade.
        float angleFade = 0.5f;       // Fresnel fade exponent (0..1). Higher = more fade when looking straight down.
        Color fadeColor = Color(0.5f, 0.5f, 0.5f, 1.0f); // Color to fade reflections into.
        float planeDistance = 0.0f;   // World Y of reflection plane (negated: d = -dot(normal, pointOnPlane)).
        float heightRange = 10.0f;    // Height normalization range for depth pass distance output.
    };

    struct DeviceVRAM
    {
        int texShadow = 0;
        int texAsset = 0;
        int texLightmap = 0;

        int tex = 0;
        int vb = 0;
        int ib = 0;
        int ub = 0;
        int sb = 0;
    };

    /*
     * The graphics device manages the underlying graphics context
     */
    /**
     * @brief Abstract GPU interface for resource creation, state management, and draw submission.
     * @ingroup group_platform_graphics
     *
     * GraphicsDevice is the platform-independent abstraction over the GPU. The concrete
     * implementation (MetalGraphicsDevice) manages triple-buffered ring buffers for uniforms,
     * pipeline state caching, and per-pass texture/uniform binding deduplication.
     * Uses reverse-Z depth.
     */
    class GraphicsDevice : public EventHandler
    {
    public:
        virtual ~GraphicsDevice();

        // Function which executes at the start of the frame
        void frameStart();

        // Function which executes at the end of the frame
        void frameEnd();

        /**
         * Capture the backbuffer to a PNG at the end of the CURRENT frame.
         *
         * Backends implement captureBackbuffer(); this just records the request
         * so the capture happens at the one point in the frame where the
         * rendered image is complete and still readable (before present).
         *
         * VISUTWIN_SCREENSHOT=<path> requests one automatically, so every
         * example can be captured without touching its source;
         * VISUTWIN_SCREENSHOT_FRAME=<n> picks the frame (default 60, which
         * gives async asset loads time to land).
         */
        void requestScreenshot(const std::string& path) { _pendingScreenshotPath = path; }
        bool screenshotPending() const { return !_pendingScreenshotPath.empty(); }

        std::shared_ptr<RenderTarget> backBuffer() const { return _backBuffer; }

        // Submits a graphical primitive to the hardware for immediate rendering
        virtual void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer = nullptr,
            int numInstances = 1, int indirectSlot = -1, bool first = true, bool last = true) = 0;
        virtual void setTransformUniforms(const Matrix4& viewProjection, const Matrix4& model)
        {
            (void)viewProjection; (void)model;
            VT_DEVICE_FEATURE_UNSUPPORTED("setTransformUniforms");
        }
        virtual void setLightingUniforms(const Color& ambientColor, const std::vector<GpuLightData>& lights,
            const Vector3& cameraPosition, bool enableNormalMaps, float exposure,
            const FogParams& fogParams = FogParams{}, const ShadowParams& shadowParams = ShadowParams{},
            int toneMapping = 0, const Vector3* ambientSH = nullptr,
            const Matrix4* viewProjection = nullptr)
        {
            (void)ambientColor; (void)lights; (void)cameraPosition; (void)enableNormalMaps;
            (void)exposure; (void)fogParams; (void)shadowParams; (void)toneMapping;
            (void)ambientSH; (void)viewProjection;
            VT_DEVICE_FEATURE_UNSUPPORTED("setLightingUniforms");
        }
        virtual void setEnvironmentUniforms(Texture* envAtlas, float skyboxIntensity, float skyboxMip,
            const Vector3& skyDomeCenter = Vector3(0,0,0), bool isDome = false,
            Texture* skyboxCubeMap = nullptr)
        {
            (void)envAtlas; (void)skyboxIntensity; (void)skyboxMip;
            (void)skyDomeCenter; (void)isDome; (void)skyboxCubeMap;
            VT_DEVICE_FEATURE_UNSUPPORTED("setEnvironmentUniforms");
        }

        virtual void setReflectionProbeUniforms(Texture* cubemap, const Vector3& boxMin,
            const Vector3& boxMax, bool boxProjection, float intensity, float maxLod)
        {
            (void)cubemap; (void)boxMin; (void)boxMax; (void)boxProjection; (void)intensity; (void)maxLod;
            VT_DEVICE_FEATURE_UNSUPPORTED("setReflectionProbeUniforms");
        }

        /// Camera clip planes for SSR depth linearization.
        virtual void setCameraClipPlanes(float nearClip, float farClip)
        {
            (void)nearClip; (void)farClip;
            VT_DEVICE_FEATURE_UNSUPPORTED("setCameraClipPlanes");
        }

        /// Which debug surface quantity the forward shader should output, as a DebugShaderPass
        /// value. Only read by shaders compiled with VT_FEATURE_DEBUG_PASS.
        virtual void setDebugShaderPass(uint32_t mode)
        {
            // The renderer calls this for every camera every frame, so warn only when
            // a debug pass is actually selected — otherwise every scene reports a gap
            // it is not using, and the diagnostic drowns out the real ones.
            if (mode != 0) {
                VT_DEVICE_FEATURE_UNSUPPORTED("setDebugShaderPass");
            }
        }

        /// Set the LTC lookup textures for area lights (bound at fragment slots 20/21
        /// when VT_FEATURE_AREA_LIGHTS is active). Pass nullptrs to unbind.
        virtual void setAreaLightLuts(Texture* lut1, Texture* lut2)
        {
            (void)lut1; (void)lut2;
            VT_DEVICE_FEATURE_UNSUPPORTED("setAreaLightLuts");
        }

        /// Bind the clustered spot-shadow atlas (depth texture2d_array) for the frame.
        virtual void setClusterShadowAtlas(Texture* atlas)
        {
            (void)atlas;
            VT_DEVICE_FEATURE_UNSUPPORTED("setClusterShadowAtlas");
        }

        /// Set atmosphere uniforms for Nishita sky scattering.
        /// data must point to an AtmosphereUniforms-compatible struct (96 bytes).
        virtual void setAtmosphereUniforms(const void* data, size_t size)
        {
            (void)data; (void)size;
            VT_DEVICE_FEATURE_UNSUPPORTED("setAtmosphereUniforms");
        }

        /// Enable/disable atmosphere scattering for the current frame.
        void setAtmosphereEnabled(bool value) { _atmosphereEnabled = value; }
        bool atmosphereEnabled() const { return _atmosphereEnabled; }

        /// Source language this device's createShader() accepts for engine-supplied
        /// shader source. Metal is the default because MSL was the only language
        /// before the Vulkan backend existed.
        [[nodiscard]] virtual ShaderLanguage shaderLanguage() const
        {
            return ShaderLanguage::Msl;
        }

        /// Create a backend-specific shader from a definition and optional source code.
        /// Backends override this to return their own Shader subclass (e.g., MetalShader).
        virtual std::shared_ptr<Shader> createShader(const ShaderDefinition& definition,
            const std::string& sourceCode = "");

        void setShader(const std::shared_ptr<Shader>& shader) { _shader = shader; }

        void setVertexBuffer(const std::shared_ptr<VertexBuffer>& vertexBuffer, const size_t slot = 0)
        {
            if (_vertexBuffers.size() <= slot) {
                _vertexBuffers.resize(slot + 1);
            }
            _vertexBuffers[slot] = vertexBuffer;
        }

        void setBlendState(const std::shared_ptr<BlendState>& blendState) { _blendState = blendState; }

        void setDepthState(const std::shared_ptr<DepthState>& depthState) { _depthState = depthState; }

        // hardware polygon-offset depth bias for shadow rendering.
        // Applied via setDepthState().
        // depthBias/slopeScale are in hardware depth-buffer units; clamp limits the maximum.
        virtual void setDepthBias(float depthBias, float slopeScale, float clamp)
        {
            (void)depthBias; (void)slopeScale; (void)clamp;
            VT_DEVICE_FEATURE_UNSUPPORTED("setDepthBias");
        }

        /// Set the indirect draw buffer for the next draw call (GPU-driven instancing).
        /// The buffer is consumed (reset to nullptr) after one indirect draw.
        /// nativeBuffer is a backend-specific GPU buffer handle.
        virtual void setIndirectDrawBuffer(void* nativeBuffer)
        {
            (void)nativeBuffer;
            VT_DEVICE_FEATURE_UNSUPPORTED("setIndirectDrawBuffer");
        }

        /// Bind the dynamic batch matrix palette for the next draw call.
        /// data: float4x4 array (16 floats per bone), size: byte count.
        /// Also used by GPU skinning (SkinInstance palette) — same slot-6 contract.
        virtual void setDynamicBatchPalette(const void* data, size_t size)
        {
            (void)data;
            (void)size;
            VT_DEVICE_FEATURE_UNSUPPORTED("setDynamicBatchPalette");
        }

        /// Bind morph target state for the next draw call (consumed by one draw).
        /// deltaBuffer: packed per-target float4 position/normal deltas (vertex slot 9).
        /// params: MorphInstance::GpuMorphParams (vertex slot 10), paramsSize: byte count.
        virtual void setMorphState(const std::shared_ptr<VertexBuffer>& deltaBuffer,
            const void* params, size_t paramsSize)
        {
            (void)deltaBuffer;
            (void)params;
            (void)paramsSize;
            VT_DEVICE_FEATURE_UNSUPPORTED("setMorphState");
        }

        /// GPU pass profiler (nullptr when the backend/device doesn't support one).
        /// Disabled by default — call gpuProfiler()->setEnabled(true) to start sampling.
        const std::shared_ptr<GpuProfiler>& gpuProfiler() const { return _gpuProfiler; }

        /// Advance a GPU particle emitter one simulation step (compute dispatch on
        /// its own command buffer, ordered before this frame's render encoding).
        virtual void simulateParticles(const std::shared_ptr<VertexBuffer>& particles,
            const GpuParticleSimParams& params)
        {
            (void)particles; (void)params;
            VT_DEVICE_FEATURE_UNSUPPORTED("simulateParticles");
        }

        /// Bind particle emitter state for the next draw call (consumed by one draw).
        /// particles: GpuParticle pool (vertex slot 7); params: GpuParticleRenderParams
        /// (vertex slot 11). Slots are shared with gsplat — a draw is one or the other.
        virtual void setParticleState(const std::shared_ptr<VertexBuffer>& particles,
            const void* params, size_t paramsSize) { (void)particles; (void)params; (void)paramsSize; }

        /// Bind an app-owned storage buffer + parameter block for the next draw call
        /// (consumed by one draw). This is the generic form of the particle/gsplat
        /// bindings and shares their slots — buffer at vertex slot 7, params at vertex
        /// slot 11 (Vulkan: descriptor set 6, bindings 0 and 3) — so a draw is a
        /// storage draw, a particle draw, or a splat draw, never two at once.
        /// Used by MeshInstance storage draws, where a custom shader expands one
        /// instance per record in the buffer.
        void setStorageDrawState(const std::shared_ptr<VertexBuffer>& buffer,
            const void* params, const size_t paramsSize)
        {
            setParticleState(buffer, params, paramsSize);
        }

        /// Bind Gaussian splat state for the next draw call (consumed by one draw).
        /// splats: GpuSplat storage (vertex slot 7); order: uint32 draw order, farthest
        /// first (vertex slot 8); sh: per-splat SH coefficients (vertex slot 12);
        /// params: GpuGSplatParams (vertex slot 11).
        virtual void setGSplatState(const std::shared_ptr<VertexBuffer>& splats,
            const std::shared_ptr<VertexBuffer>& order, const std::shared_ptr<VertexBuffer>& sh,
            const void* params, size_t paramsSize)
        {
            (void)splats;
            (void)order;
            (void)sh;
            (void)params;
            (void)paramsSize;
        }

        /// Bind clustered lighting data for the current frame.
        /// lightData: packed GpuClusteredLight array, lightSize: byte count.
        /// cellData: uint8 cell→light index mapping, cellSize: byte count.
        virtual void setClusterBuffers(const void* lightData, size_t lightSize,
            const void* cellData, size_t cellSize)
        {
            (void)lightData;
            (void)lightSize;
            (void)cellData;
            (void)cellSize;
        }

        /// Set clustered lighting grid parameters into LightingUniforms.
        /// Called by the renderer after WorldClusters::update().
        virtual void setClusterGridParams(const float* boundsMin, const float* boundsRange,
            const float* cellsCountByBoundsSize,
            int cellsX, int cellsY, int cellsZ, int maxLightsPerCell,
            int numClusteredLights)
        {
            (void)boundsMin;
            (void)boundsRange;
            (void)cellsCountByBoundsSize;
            (void)cellsX;
            (void)cellsY;
            (void)cellsZ;
            (void)maxLightsPerCell;
            (void)numClusteredLights;
        }

        void setCullMode(const CullMode cullMode) { _cullMode = cullMode; }
        CullMode cullMode() const { return _cullMode; }
        void setStencilState(const std::shared_ptr<StencilParameters>& stencilFront = nullptr,
            const std::shared_ptr<StencilParameters>& stencilBack = nullptr)
        {
            _stencilFront = stencilFront;
            _stencilBack = stencilBack;
            _stencilEnabled = (_stencilFront != nullptr || _stencilBack != nullptr);
        }

        virtual void setViewport(float x, float y, float w, float h)
        {
            _vx = x;
            _vy = y;
            _vw = w;
            _vh = h;
        }

        virtual void setScissor(int x, int y, int w, int h)
        {
            _sx = x;
            _sy = y;
            _sw = w;
            _sh = h;
        }

        float vx() const { return _vx; }
        float vy() const { return _vy; }
        float vw() const { return _vw; }
        float vh() const { return _vh; }
        int sx() const { return _sx; }
        int sy() const { return _sy; }
        int sw() const { return _sw; }
        int sh() const { return _sh; }
        std::shared_ptr<VertexBuffer> quadVertexBuffer();
        void setQuadTextureBinding(const size_t slot, Texture* texture)
        {
            if (slot < _quadTextureBindings.size()) {
                _quadTextureBindings[slot] = texture;
            }
        }
        void clearQuadTextureBindings()
        {
            _quadTextureBindings.fill(nullptr);
        }
        void setQuadRenderActive(const bool active) { _quadRenderActive = active; }
        bool quadRenderActive() const { return _quadRenderActive; }
        Texture* quadTextureBinding(const size_t slot) const
        {
            return slot < _quadTextureBindings.size() ? _quadTextureBindings[slot] : nullptr;
        }
        const std::array<Texture*, 8>& quadTextureBindings() const { return _quadTextureBindings; }

        /**
         * Uniform block for the next quad draw.
         *
         * A quad pass carries no Material, so the block rides the per-draw
         * uniform slot that would otherwise hold a default-constructed
         * MaterialUniforms (Metal buffer 3 / Vulkan set 0 binding 0). The quad
         * shader declares whatever struct it wants there. This is what lets a
         * fullscreen effect live above GraphicsDevice instead of as a backend
         * pass class: shader + textures + this block is the whole contract.
         *
         * Blocks up to kPerDrawUniformCapacity bytes are accepted; larger ones
         * are rejected rather than silently truncated.
         *
         * The bytes are copied, so the caller's struct need not outlive the draw.
         */
        void setQuadUniformData(const void* data, const size_t size)
        {
            if (size > kPerDrawUniformCapacity) {
                VT_DEVICE_FEATURE_UNSUPPORTED("quad uniform block exceeds kPerDrawUniformCapacity");
                _quadUniformData.clear();
                return;
            }
            _quadUniformData.assign(static_cast<const uint8_t*>(data),
                                    static_cast<const uint8_t*>(data) + size);
        }
        void clearQuadUniformData() { _quadUniformData.clear(); }
        const std::vector<uint8_t>& quadUniformData() const { return _quadUniformData; }

        void setMaterial(const Material* material) { _material = material; }
        const Material* material() const { return _material; }

        // when true, forward shaders output linear HDR
        // (tonemapping + gamma are deferred to the compose pass).  Set by
        // RenderPassForward when running inside a CameraFrame pipeline.
        void setHdrPass(bool hdr) { _hdrPass = hdr; }
        bool hdrPass() const { return _hdrPass; }

        virtual void startRenderPass(RenderPass* renderPass) = 0;

        virtual void endRenderPass(RenderPass* renderPass) = 0;

        virtual std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture* texture) = 0;

        virtual std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            int numVertices, const VertexBufferOptions& options = VertexBufferOptions{}) = 0;

        virtual std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat format, int numIndices,
            const std::vector<uint8_t>& data = {}) = 0;

        int samples() const { return _samples; }

        void resizeCanvas(int width, int height);
        virtual void setResolution(int width, int height) = 0;

        virtual std::pair<int, int> size() const = 0;

        int drawCallsPerFrame() const { return _drawCallsPerFrame; }
        void resetDrawCallsPerFrame() { _drawCallsPerFrame = 0; }

        bool contextLost() const { return _contextLost; }

        virtual void update();

        void updateClientRect();

        // The maximum supported number of hardware antialiasing samples
        int maxSamples() const { return _maxSamples; }

        void removeTarget(RenderTarget* target);

        std::shared_ptr<RenderTarget> renderTarget() const { return _renderTarget; }
        void setRenderTarget(const std::shared_ptr<RenderTarget>& target) { _renderTarget = target; }
        bool insideRenderPass() const { return _insideRenderPass; }

        /**
         * False when the device cannot record any work for the current frame,
         * so no render pass can accomplish anything — a Vulkan frame skipped at
         * swapchain acquire, for instance. Known from frameStart onwards, which
         * lets RenderPass::render() skip a doomed pass whole rather than run
         * before()/after() around an execute() that cannot happen.
         */
        virtual bool frameRenderable() const { return true; }
        Texture* sceneDepthMap() const { return _sceneDepthMap; }
        void setSceneDepthMap(Texture* depthMap) { _sceneDepthMap = depthMap; }

        /// Scene color grab (dynamic refraction): copy the current scene color into a
        /// mipmapped texture bound at fragment slot 22. Backend-implemented; the base
        /// class only stores the published texture.
        virtual void grabSceneColor(RenderTarget* source)
        {
            (void)source;
            VT_DEVICE_FEATURE_UNSUPPORTED("grabSceneColor");
        }
        Texture* sceneColorMap() const { return _sceneColorMap; }
        void setSceneColorMap(Texture* colorMap) { _sceneColorMap = colorMap; }

        /// Copy the post-opaque scene depth into a sampleable texture (for SSR),
        /// avoiding the feedback of sampling the still-attached depth buffer.
        virtual void grabSceneDepth(RenderTarget* source)
        {
            (void)source;
            VT_DEVICE_FEATURE_UNSUPPORTED("grabSceneDepth");
        }
        Texture* sceneDepthGrabMap() const { return _sceneDepthGrabMap; }
        void setSceneDepthGrabMap(Texture* depthMap) { _sceneDepthGrabMap = depthMap; }

        /// Regenerate the full mip chain of a (render-target) cubemap via a blit
        /// pass on its own command buffer. Used after reflection-probe scene
        /// capture, where the forward pass fills only level 0 of each cube face
        /// and the probe shader samples coarser mips for higher roughness.
        virtual void generateCubemapMips(Texture* cubemap)
        {
            (void)cubemap;
            VT_DEVICE_FEATURE_UNSUPPORTED("generateCubemapMips");
        }

        // DEVIATION: planar reflection texture, set by application-level code.
        // Upstream handles this in the planarRenderer script; we promote it to
        // a device-level binding so the forward pass can sample it at slot 9.
        Texture* reflectionMap() const { return _reflectionMap; }
        void setReflectionMap(Texture* tex) { _reflectionMap = tex; }

        // DEVIATION: blurred planar reflection parameters.
        const ReflectionBlurParams& reflectionBlurParams() const { return _reflectionBlurParams; }
        void setReflectionBlurParams(const ReflectionBlurParams& params) { _reflectionBlurParams = params; }

        // DEVIATION: planar reflection depth texture (distance-from-plane), bound at slot 10.
        // depth camera pass for per-pixel blur radius.
        Texture* reflectionDepthMap() const { return _reflectionDepthMap; }
        void setReflectionDepthMap(Texture* tex) { _reflectionDepthMap = tex; }

        // SSAO texture for per-material forward-pass compositing (VT_FEATURE_SSAO).
        // When non-null, fragment shaders modulate ambient occlusion by sampling this
        // texture at screen-space UV. Bound at fragment texture slot 18.
        Texture* ssaoForwardTexture() const { return _ssaoForwardTexture; }
        void setSsaoForwardTexture(Texture* tex) { _ssaoForwardTexture = tex; }

        /// Create a VertexBuffer that adopts a pre-existing GPU buffer (zero-copy).
        /// The nativeBuffer pointer is backend-specific (MTL::Buffer*, VkBuffer, etc.).
        /// Used for GPU compute output paths where the buffer is already filled.
        virtual std::shared_ptr<VertexBuffer> createVertexBufferFromNativeBuffer(
            const std::shared_ptr<VertexFormat>& format,
            int numVertices, void* nativeBuffer) { (void)nativeBuffer; return nullptr; }

        /// True when the backend supports dual-source blending — the BLENDMODE_SRC1_* factors,
        /// which read a second color output written by the fragment shader. Check this before
        /// building a blend state that uses them; on a backend without support the pipeline
        /// would be invalid.
        virtual bool supportsDualSourceBlending() const { return false; }

        /// True when this backend can create an InstanceCuller via createInstanceCuller().
        /// Used by MeshInstance::enableGpuInstanceCulling() to decide whether to allocate
        /// the per-instance culler resources or fall back to CPU-only path.
        virtual bool supportsGpuInstanceCulling() const { return false; }

        /// Create a GPU instance culler for hardware-instanced meshes.
        /// Each MeshInstance that opts into GPU frustum culling owns a dedicated
        /// culler (pipelines are cached by the backend shader compiler, so
        /// duplication across instances is cheap). Returns nullptr on backends
        /// that do not support GPU culling — the caller must handle that case.
        virtual std::unique_ptr<InstanceCuller> createInstanceCuller() { return nullptr; }

        /// GPU instance-cull batching. All InstanceCuller::cull() calls between
        /// begin/end share one backend command buffer. Backends may submit it
        /// asynchronously when later rendering is ordered on the same queue;
        /// standalone cull() preserves immediate CPU-readback semantics.
        virtual void beginGpuCullBatch() {}
        virtual void endGpuCullBatch() {}

        /**
         * Group consecutive environment operations (reprojection, convolution,
         * atlas bakes, cubemap mip generation) into ONE command buffer instead of
         * one each. Every such call otherwise creates, encodes and commits its own
         * buffer, so a scene that reprojects several targets per frame pays that
         * submission cost repeatedly. Nesting is reference-counted; the batch must
         * be closed before the rendering that samples its results.
         */
        virtual void beginEnvBatch() {}
        virtual void endEnvBatch() {}

        virtual std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions& options) = 0;
        virtual void executeTAAPass(Texture* sourceTexture, Texture* historyTexture, Texture* depthTexture,
            const Matrix4& viewProjectionPrevious, const Matrix4& viewProjectionInverse,
            const std::array<float, 4>& jitters, const std::array<float, 4>& cameraParams,
            bool highQuality, bool historyValid)
        {
            (void)sourceTexture; (void)historyTexture; (void)depthTexture;
            (void)viewProjectionPrevious; (void)viewProjectionInverse;
            (void)jitters; (void)cameraParams; (void)highQuality; (void)historyValid;
            VT_DEVICE_FEATURE_UNSUPPORTED("executeTAAPass");
        }
        virtual void executeSsaoPass(const SsaoPassParams& params)
        {
            (void)params;
            VT_DEVICE_FEATURE_UNSUPPORTED("executeSsaoPass");
        }
        virtual void generateEnvReproject(const EnvReprojectPassParams& params)
        {
            (void)params;
            VT_DEVICE_FEATURE_UNSUPPORTED("generateEnvReproject");
        }
        virtual void generateEnvConvolve(const EnvConvolvePassParams& params)
        {
            (void)params;
            VT_DEVICE_FEATURE_UNSUPPORTED("generateEnvConvolve");
        }
        virtual void generateEnvAtlas(const EnvAtlasBakeParams& params)
        {
            (void)params;
            VT_DEVICE_FEATURE_UNSUPPORTED("generateEnvAtlas");
        }
        virtual void generateEquirectToCubemap(const EquirectToCubeParams& params)
        {
            (void)params;
            VT_DEVICE_FEATURE_UNSUPPORTED("generateEquirectToCubemap");
        }
        virtual bool supportsCompute() const { return false; }
        virtual void computeDispatch(const std::vector<Compute*>& computes, const std::string& label = "")
        {
            (void)computes; (void)label;
            VT_DEVICE_FEATURE_UNSUPPORTED("computeDispatch");
        }

        void addTexture(const std::shared_ptr<Texture>& texture)
        {
            _textures.push_back(texture);
        }

        int renderVersion() const { return _renderVersion; }

        // Device-level shader cache for utility shaders (downsample, upsample, etc.)
        // that are compiled from fixed source and should only be created once.
        std::shared_ptr<Shader> getCachedShader(const std::string& name) const
        {
            const auto it = _shaderCache.find(name);
            return it != _shaderCache.end() ? it->second : nullptr;
        }
        void setCachedShader(const std::string& name, const std::shared_ptr<Shader>& shader)
        {
            _shaderCache[name] = shader;
        }

    protected:
        virtual void onFrameStart() {}
        virtual void onFrameEnd() {}
        void setBackBuffer(const std::shared_ptr<RenderTarget>& target) { _backBuffer = target; }
        /** Highest MSAA sample count the backend supports; RenderTarget clamps to it. */
        void setMaxSamples(const int value) { _maxSamples = value > 1 ? value : 1; }
        void recordDrawCall(int count = 1) { _drawCallsPerFrame += count; }

        void clearVertexBuffer();
        // Backends that destroy their native device in the derived destructor
        // must release base-owned GPU objects first.
        void releaseGpuReferences();

        std::shared_ptr<Shader> _shader;

        std::vector<std::shared_ptr<VertexBuffer>> _vertexBuffers;

        std::shared_ptr<RenderTarget> _renderTarget;

        std::shared_ptr<BlendState> _blendState;
        std::shared_ptr<DepthState> _depthState;

        CullMode _cullMode = CullMode::CULLFACE_BACK;
        bool _insideRenderPass = false;

        bool _stencilEnabled = false;

        std::shared_ptr<StencilParameters> _stencilFront;
        std::shared_ptr<StencilParameters> _stencilBack;
        const Material* _material = nullptr;

        // GPU pass profiler — assigned by backends that support one.
        std::shared_ptr<GpuProfiler> _gpuProfiler;

        // Backbuffer capture (see requestScreenshot). Empty when idle.
        std::string _pendingScreenshotPath;
        // Env-var driven auto-capture, resolved once on the first frame.
        bool _screenshotEnvChecked = false;
        std::string _screenshotEnvPath;
        uint64_t _screenshotEnvFrame = 60;
        uint64_t _frameCounter = 0;

    private:
        friend class Engine;
        friend class RenderPass;
        friend class VertexBuffer;
        friend class Texture;

        // Index of the currently active render pass
        int _renderPassIndex;

        // A version number that is incremented every frame. This is used to detect if some object were invalidated.
        int _renderVersion;

        // The render target representing the main back-buffer
        std::shared_ptr<RenderTarget> _backBuffer;

        std::shared_ptr<VertexBuffer> _quadVertexBuffer;
        std::array<Texture*, 8> _quadTextureBindings{};
        std::vector<uint8_t> _quadUniformData;
        bool _quadRenderActive = false;
        bool _hdrPass = false;
        bool _atmosphereEnabled = false;
        float _vx = 0.0f;
        float _vy = 0.0f;
        float _vw = 0.0f;
        float _vh = 0.0f;
        int _sx = 0;
        int _sy = 0;
        int _sw = 0;
        int _sh = 0;

        std::shared_ptr<DynamicBuffers> _dynamicBuffers;

        int _samples = 0;

        // Never assigned elsewhere; resizeCanvas clamps it with min(_, 1.0f), so
        // an uninitialized read could scale the backbuffer to 0×0 and (on Vulkan)
        // wedge swapchain recreation permanently.
        float _maxPixelRatio = 1.0f;

        int _shaderSwitchesPerFrame = 0;
        int _drawCallsPerFrame = 0;

        int _renderTargetCreationTime = 0;

        bool _contextLost = false;

        std::pair<int, int> _clientRect;

        std::unordered_set<std::map<void*, void*>*> _mapsToClear;

        std::vector<VertexBuffer*> _buffers;

        std::vector<int> _primsPerFrame;

        int _maxSamples = 1;

        std::unordered_set<RenderTarget*> _targets;

        DeviceVRAM _vram;

        std::vector<std::shared_ptr<Texture>> _textures;

        Texture* _sceneDepthMap = nullptr;
        Texture* _sceneColorMap = nullptr;
        Texture* _sceneDepthGrabMap = nullptr;
        Texture* _reflectionMap = nullptr;
        Texture* _reflectionDepthMap = nullptr;
        Texture* _ssaoForwardTexture = nullptr;
        ReflectionBlurParams _reflectionBlurParams;

        std::unordered_map<std::string, std::shared_ptr<Shader>> _shaderCache;
    };
}
