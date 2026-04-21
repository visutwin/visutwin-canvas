// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <array>
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
    class Compute;
    class Texture;
    class Material;
    struct RenderTargetOptions;

    enum class GpuLightType : uint32_t
    {
        Directional = 0u,
        Point = 1u,
        Spot = 2u,
        AreaRect = 3u
    };

    /** @brief Per-light GPU data uploaded to the lighting uniform buffer.
     *  @ingroup group_scene_lighting */
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

        // Area rect light: half-extents and local right axis (world space).
        float areaHalfWidth = 0.0f;
        float areaHalfHeight = 0.0f;
        Vector3 areaRight = Vector3(1.0f, 0.0f, 0.0f);
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
        } localShadows[kMaxLocalShadows];
    };

    struct ComposePassParams
    {
        Texture* sceneTexture = nullptr;
        Texture* bloomTexture = nullptr;
        Texture* cocTexture = nullptr;
        Texture* blurTexture = nullptr;
        Texture* ssaoTexture = nullptr;
        float bloomIntensity = 0.01f;
        float dofIntensity = 1.0f;
        float sharpness = 0.0f;
        int toneMapping = 0;
        float exposure = 1.0f;
        bool dofEnabled = false;
        bool taaEnabled = false;
        bool blurTextureUpscale = false;

        // Single-pass DOF (computed in compose shader from depth buffer)
        Texture* depthTexture = nullptr;  // scene depth buffer
        float dofFocusDistance = 1.0f;    // linear depth at perfect focus
        float dofFocusRange = 0.5f;       // transition zone width
        float dofBlurRadius = 3.0f;       // max blur in pixels
        float dofCameraNear = 0.01f;
        float dofCameraFar = 100.0f;

        // Vignette
        bool vignetteEnabled = false;
        float vignetteInner = 0.5f;       // inner radius — fully clear inside this
        float vignetteOuter = 1.0f;       // outer radius — darkest at this edge
        float vignetteCurvature = 0.5f;   // edge curvature (higher = more rounded)
        float vignetteIntensity = 0.3f;   // max darkness
        float vignetteColor[3] = {0.0f, 0.0f, 0.0f};  // darkening color (black)
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

    struct CoCPassParams
    {
        Texture* depthTexture = nullptr;
        float focusDistance = 100.0f;
        float focusRange = 10.0f;
        float cameraNear = 0.1f;
        float cameraFar = 1000.0f;
        bool nearBlur = false;
    };

    struct DofBlurPassParams
    {
        Texture* nearTexture = nullptr;   // half-res scene (for near blur)
        Texture* farTexture = nullptr;    // far-field pre-multiplied texture
        Texture* cocTexture = nullptr;    // CoC texture from the CoC pass
        float blurRadiusNear = 3.0f;
        float blurRadiusFar = 3.0f;
        int blurRings = 4;
        int blurRingPoints = 5;
        float invResolutionX = 0.0f;
        float invResolutionY = 0.0f;
    };

    struct DepthAwareBlurPassParams
    {
        Texture* sourceTexture = nullptr;
        Texture* depthTexture = nullptr;
        int filterSize = 4;
        float sourceInvResolutionX = 0.0f;
        float sourceInvResolutionY = 0.0f;
        float cameraNear = 0.1f;
        float cameraFar = 1000.0f;
    };

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
        Texture* sourceEquirect = nullptr;
        Texture* sourceCubemap  = nullptr;
        std::vector<EnvReprojectOp> ops;
        bool encodeRgbp = true;
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

        std::shared_ptr<RenderTarget> backBuffer() const { return _backBuffer; }

        // Submits a graphical primitive to the hardware for immediate rendering
        virtual void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer = nullptr,
            int numInstances = 1, int indirectSlot = -1, bool first = true, bool last = true) = 0;
        virtual void setTransformUniforms(const Matrix4& viewProjection, const Matrix4& model) {}
        virtual void setLightingUniforms(const Color& ambientColor, const std::vector<GpuLightData>& lights,
            const Vector3& cameraPosition, bool enableNormalMaps, float exposure,
            const FogParams& fogParams = FogParams{}, const ShadowParams& shadowParams = ShadowParams{},
            int toneMapping = 0) {}
        virtual void setEnvironmentUniforms(Texture* envAtlas, float skyboxIntensity, float skyboxMip,
            const Vector3& skyDomeCenter = Vector3(0,0,0), bool isDome = false,
            Texture* skyboxCubeMap = nullptr) {}

        /// Set atmosphere uniforms for Nishita sky scattering.
        /// data must point to an AtmosphereUniforms-compatible struct (96 bytes).
        virtual void setAtmosphereUniforms(const void* data, size_t size) { (void)data; (void)size; }

        /// Enable/disable atmosphere scattering for the current frame.
        void setAtmosphereEnabled(bool value) { _atmosphereEnabled = value; }
        bool atmosphereEnabled() const { return _atmosphereEnabled; }

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
        virtual void setDepthBias(float depthBias, float slopeScale, float clamp) {}

        /// Set the indirect draw buffer for the next draw call (GPU-driven instancing).
        /// The buffer is consumed (reset to nullptr) after one indirect draw.
        /// nativeBuffer is a backend-specific GPU buffer handle.
        virtual void setIndirectDrawBuffer(void* nativeBuffer) { (void)nativeBuffer; }

        /// Bind the dynamic batch matrix palette for the next draw call.
        /// data: float4x4 array (16 floats per bone), size: byte count.
        virtual void setDynamicBatchPalette(const void* data, size_t size)
        {
            (void)data;
            (void)size;
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
        Texture* sceneDepthMap() const { return _sceneDepthMap; }
        void setSceneDepthMap(Texture* depthMap) { _sceneDepthMap = depthMap; }

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

        virtual std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions& options) = 0;
        virtual void executeComposePass(const ComposePassParams& params) {}
        virtual void executeTAAPass(Texture* sourceTexture, Texture* historyTexture, Texture* depthTexture,
            const Matrix4& viewProjectionPrevious, const Matrix4& viewProjectionInverse,
            const std::array<float, 4>& jitters, const std::array<float, 4>& cameraParams,
            bool highQuality, bool historyValid) {}
        virtual void executeSsaoPass(const SsaoPassParams& params) {}
        virtual void executeCoCPass(const CoCPassParams& params) {}
        virtual void executeDofBlurPass(const DofBlurPassParams& params) {}
        virtual void executeDepthAwareBlurPass(const DepthAwareBlurPassParams& params, bool horizontal) {}
        virtual void generateEnvReproject(const EnvReprojectPassParams& params) { (void)params; }
        virtual bool supportsCompute() const { return false; }
        virtual void computeDispatch(const std::vector<Compute*>& computes, const std::string& label = "") {}

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
        void recordDrawCall(int count = 1) { _drawCallsPerFrame += count; }

        void clearVertexBuffer();

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

        std::shared_ptr<GpuProfiler> _gpuProfiler;

        int _samples = 0;

        float _maxPixelRatio;

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
        Texture* _reflectionMap = nullptr;
        Texture* _reflectionDepthMap = nullptr;
        Texture* _ssaoForwardTexture = nullptr;
        ReflectionBlurParams _reflectionBlurParams;

        std::unordered_map<std::string, std::shared_ptr<Shader>> _shaderCache;
    };
}
