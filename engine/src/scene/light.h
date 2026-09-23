// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.09.2025.
//
#pragma once

#include <vector>

#include <algorithm>
#include <array>
#include <memory>

#include "camera.h"
#include "constants.h"
#include "core/math/vector4.h"
#include "core/shape/boundingSphere.h"
#include "platform/graphics/graphicsDevice.h"
#include "renderer/shadowMap.h"

namespace visutwin::canvas
{
    class Light;
    class MeshInstance;

    /**
     * @brief Per-face shadow rendering data: shadow camera, viewport, and scissor.
     * @ingroup group_scene_lighting
     */
    class LightRenderData
    {
    public:
        LightRenderData(Camera* camera, int face, Light* light);

        Light* light;

        // Camera used to cull/render the shadow map
        std::unique_ptr<Camera> shadowCamera;

        Camera* camera;

        int face;

        // Viewport for the shadow rendering to the texture (x, y, width, height)
        Vector4 shadowViewport;

        // Scissor rectangle for the shadow rendering to the texture
        Vector4 shadowScissor;

        // Casters this face must draw, filled once per frame for OMNI lights by
        // cullShadowCastersOmni: one sweep of the scene classifies each caster
        // into the faces it touches, instead of six independent frustum sweeps.
        // Empty for every other light type, whose passes cull as they draw.
        std::vector<MeshInstance*> visibleCasters;
    };

    /**
     * @brief Directional, point, spot, or area light with shadow mapping and cookie projection.
     * @ingroup group_scene_lighting
     *
     * Light defines the type, color, intensity, range, and shadow parameters for a light source.
     * Shadow-casting lights own LightRenderData instances (one per face for omni shadows) that
     * hold the shadow camera, viewport, and scissor used during shadow pass rendering.
     */
    class Light
    {
    public:
        Light(GraphicsDevice* graphicsDevice, bool clusteredLighting);

        bool atlasViewportAllocated() const { return _atlasViewportAllocated; }
        void setAtlasViewportAllocated(bool value) { _atlasViewportAllocated = value; }

        // True on the frame the atlas hands this light a DIFFERENT slot, so its
        // shadow (and cookie) content has to be rendered again.
        bool atlasSlotUpdated() const { return _atlasSlotUpdated; }
        void setAtlasSlotUpdated(const bool value) { _atlasSlotUpdated = value; }

        // Clustered local-shadow atlas (LightTextureAtlas): the slot this light
        // renders into, normalized (x, y, width, height) with a top-left origin, and
        // the bookkeeping that lets a light keep its slot across frames.
        const Vector4& atlasViewport() const { return _atlasViewport; }
        void setAtlasViewport(const Vector4& value) { _atlasViewport = value; }
        int atlasSlotIndex() const { return _atlasSlotIndex; }
        void setAtlasSlotIndex(const int value) { _atlasSlotIndex = value; }
        int atlasVersion() const { return _atlasVersion; }
        void setAtlasVersion(const int value) { _atlasVersion = value; }

        // The largest fraction of any camera's viewport this light's bounds cover
        // this frame (upstream `maxScreenSize`); reset with visibility, raised by
        // Renderer::cullLights. Ranks lights for atlas slots and main-array slots.
        float maxScreenSize() const { return _maxScreenSize; }
        void setMaxScreenSize(const float value) { _maxScreenSize = value; }

        bool enabled() const { return _enabled; }
        void setEnabled(const bool value) { _enabled = value; }

        // Light cookie (upstream Light.cookie): a texture projected by the light,
        // masking its color. 2D for spot lights, cubemap for omni. Non-owning —
        // the app owns the texture (asset or manually built cubemap).
        Texture* cookie() const { return _cookie; }
        void setCookie(Texture* value) { _cookie = value; }

        // Blend between "no cookie" (0) and the full cookie sample (1).
        float cookieIntensity() const { return _cookieIntensity; }
        void setCookieIntensity(const float value) { _cookieIntensity = value; }

        CookieChannel cookieChannel() const { return _cookieChannel; }
        void setCookieChannel(const CookieChannel value) { _cookieChannel = value; }

        // Spot only. When false (upstream's non-default), the cone angle falloff is
        // skipped and the cookie's own projection clip defines the beam shape.
        bool cookieFalloff() const { return _cookieFalloff; }
        void setCookieFalloff(const bool value) { _cookieFalloff = value; }

        // World → cookie-UV projection for spot cookies. Equal to the shadow VP
        // when the light casts shadows; otherwise evaluated separately (upstream
        // LightCamera.evalSpotCookieMatrix). Omni cookies use the light's world
        // transform instead and ignore this.
        const Matrix4& cookieMatrix() const { return _cookieMatrix; }
        void setCookieMatrix(const Matrix4& value) { _cookieMatrix = value; }

        /**
         * True when some camera's frustum reached this light in the frame being
         * built. A UNION over every camera, as upstream's is, so it answers "does
         * anything need this light's per-frame work" — its shadow map and its
         * cookie — rather than "does this camera see it". A per-camera decision
         * has to be made per camera; see the local-light cull in
         * Renderer::renderForwardLayer.
         *
         * Renderer::resetLightVisibility clears it once a frame and
         * Renderer::cullLights sets it; a directional light is always visible
         * because its influence has no bounds to test.
         */
        bool visibleThisFrame() const { return _visibleThisFrame; }
        void setVisibleThisFrame(const bool value) { _visibleThisFrame = value; }

        /**
         * World-space sphere bounding this light's influence, for culling. Omni is
         * the range sphere; a SPOT is upstream's bound of its cone, which is the
         * range sphere only for a very wide cone and much smaller for a narrow one.
         * Meaningless for a directional light, which is never culled.
         */
        BoundingSphere boundingSphere() const;

        LightType type() const { return _type; }
        void setType(const LightType value) { _type = value; }

        bool castShadows() const;
        void setCastShadows(bool value);

        MaskType mask() const { return _mask; }
        void setMask(const MaskType value) { _mask = value; }

        ShadowUpdateType shadowUpdateMode() const { return _shadowUpdateMode; }
        void setShadowUpdateMode(const ShadowUpdateType mode) { _shadowUpdateMode = mode; }

        int numShadowFaces() const;

        int numCascades() const;
        void setNumCascades(int value);

        float cascadeDistribution() const { return _cascadeDistribution; }
        void setCascadeDistribution(const float value) { _cascadeDistribution = value; }

        float cascadeBlend() const { return _cascadeBlend; }
        void setCascadeBlend(const float value) { _cascadeBlend = value; }

        const std::array<Vector4, 4>& cascadeViewports() const { return _cascadeViewports; }
        const std::array<float, 64>& shadowMatrixPalette() const { return _shadowMatrixPalette; }
        float* shadowMatrixPaletteData() { return _shadowMatrixPalette.data(); }
        const std::array<float, 4>& shadowCascadeDistances() const { return _shadowCascadeDistances; }
        float* shadowCascadeDistancesData() { return _shadowCascadeDistances.data(); }

        // The light owns its shadow map: replacing it (setNumCascades,
        // setShadowType, setShadowResolution) or destroying the light frees the
        // old GPU textures instead of orphaning them in a renderer-side list.
        ShadowMap* shadowMap() const { return _shadowMap.get(); }
        void setShadowMap(std::shared_ptr<ShadowMap> value) { _shadowMap = std::move(value); }

        LightRenderData* getRenderData(Camera* camera, int face);

        // Drops cached render data keyed on this camera. Must be called when a
        // camera is destroyed — _renderData is keyed on raw Camera*, and a new
        // camera allocated at the same address would silently reuse stale state.
        void invalidateRenderData(const Camera* camera);

        /// The shadow type this light will actually render, which is not always
        /// the one that was asked for: a VSM_16F request on a device without
        /// half-float colour attachments has nowhere to write its moments and
        /// falls back to PCF3, upstream's documented fallback. `requestedShadowType`
        /// is what the caller set, kept so a per-frame replay of the same value
        /// does not re-resolve and drop the shadow map every frame.
        ShadowType shadowType() const { return _shadowType; }
        ShadowType requestedShadowType() const { return _requestedShadowType; }
        void setShadowType(ShadowType value);

        GraphNode* node() const { return _node; }
        void setNode(GraphNode* value) { _node = value; }

        float shadowDistance() const { return _shadowDistance; }
        void setShadowDistance(const float value) { _shadowDistance = value; }

        int shadowResolution() const { return _shadowResolution; }

        // Drops the shadow map on a change: it is allocated lazily and only when
        // null, so an inline store left the texture at its old size while the
        // viewport and the shader's texel size followed the new one.
        void setShadowResolution(int value);

        // VSM-only: separable gaussian blur kernel size (total taps; must be odd).
        int vsmBlurSize() const { return _vsmBlurSize; }
        void setVsmBlurSize(const int value) { _vsmBlurSize = value < 3 ? 3 : value; }

        // VSM-only: bias scale used for the minVariance floor in Chebyshev's
        // inequality (depth ambiguity at thin / silhouette edges). Default
        // mirrors upstream SHADOW_VSM_16F: 0.01 * 0.25 = 0.0025.
        float vsmBias() const { return _vsmBias; }
        void setVsmBias(const float value) { _vsmBias = std::max(value, 0.0f); }

        // PCSS (SHADOW_PCSS_32F): world-space light area size driving the penumbra width.
        float penumbraSize() const { return _penumbraSize; }
        void setPenumbraSize(const float value) { _penumbraSize = std::max(value, 0.0f); }

        // PCSS: penumbra growth curve shape (>= 1; 1 = linear with blocker distance).
        float penumbraFalloff() const { return _penumbraFalloff; }
        void setPenumbraFalloff(const float value) { _penumbraFalloff = std::max(value, 1.0f); }

        //shadowBias.
        float shadowBias() const { return _shadowBias; }
        void setShadowBias(const float value) { _shadowBias = value; }

        //_normalOffsetBias.
        float normalBias() const { return _normalBias; }
        void setNormalBias(const float value) { _normalBias = value; }

        //shadowIntensity (1 = full shadow, 0 = no shadow effect).
        float shadowIntensity() const { return _shadowIntensity; }
        void setShadowIntensity(const float value) { _shadowIntensity = value; }

        // Per-light shadow VP matrix for local lights. Set during shadow camera positioning.
        const Matrix4& shadowViewProjection() const { return _shadowViewProjection; }
        void setShadowViewProjection(const Matrix4& value) { _shadowViewProjection = value; }

        // Range and cone angle — synced from LightComponent for shadow camera setup.
        float range() const { return _range; }
        void setRange(const float value) { _range = value; }

        float outerConeAngle() const { return _outerConeAngle; }
        void setOuterConeAngle(const float value) { _outerConeAngle = value; }

        GraphicsDevice* device() const { return _device; }

    private:
        // Drops the shadow map so the next frame reallocates it, and re-arms a
        // light whose shadow was already considered rendered — a map nothing
        // renders into is worse than the stale one it replaced. Upstream's
        // `_destroyShadowMap`.
        void destroyShadowMap();

        // Maps a requested shadow type onto one this device can actually render.
        ShadowType resolveShadowType(ShadowType requested) const;

        GraphicsDevice* _device;

        bool _clusteredLighting;

        bool _atlasViewportAllocated = false;

        bool _atlasSlotUpdated = false;

        Vector4 _atlasViewport = Vector4(0.0f, 0.0f, 1.0f, 1.0f);
        int _atlasSlotIndex = -1;
        int _atlasVersion = -1;
        float _maxScreenSize = 0.0f;

        bool _enabled = false;

        Texture* _cookie = nullptr;

        float _cookieIntensity = 1.0f;

        CookieChannel _cookieChannel = CookieChannel::COOKIE_CHANNEL_RGB;

        bool _cookieFalloff = true;

        Matrix4 _cookieMatrix = Matrix4::identity();

        bool _visibleThisFrame = false;

        LightType _type = LightType::LIGHTTYPE_DIRECTIONAL;

        bool _castShadows = false;

        // DEVIATION: upstream's Light defaults this to MASK_AFFECT_DYNAMIC. Because
        // castShadows() folds the mask in, MASK_NONE makes that getter false on any
        // Light not driven by a LightComponent, whatever setCastShadows said. A
        // component pushes its own mask every frame, so nothing in the examples shows
        // it — but aligning the default is NOT free: it changes depth-of-field, whose
        // only light is the environment atlas, so something reads castShadows() before
        // the first sync and keeps the answer. Left alone deliberately; it belongs
        // with the defaults alignment, not here.
        MaskType _mask = MaskType::MASK_NONE;

        ShadowUpdateType _shadowUpdateMode = ShadowUpdateType::SHADOWUPDATE_NONE;

        // ONE cascade by default, as upstream (light.js `numCascades = 1`). It used to
        // be 4, and a one-shot directional shadow then broke the moment the camera
        // moved: the cascade a fragment samples is picked by its VIEW depth, so
        // zooming in carried the whole scene into cascades 0-1, whose maps had been
        // fitted once to the empty near slices of the original view.
        int _numCascades = 1;
        float _cascadeDistribution = 0.5f;   // 0=linear splits, 1=logarithmic, 0.5=practical blend
        float _cascadeBlend = 0.0f;          // fraction: 0 = off, else dither + far fade (upstream)

        // Viewport rects per cascade (normalized 0..1 within shadow texture).
        // Layout matches upstream directionalCascades:
        //   1 cascade: full texture
        //   2 cascades: 2×1 vertical strip
        //   4 cascades: 2×2 grid
        std::array<Vector4, 4> _cascadeViewports = {{ Vector4(0,0,0.5f,0.5f), Vector4(0,0.5f,0.5f,0.5f), Vector4(0.5f,0,0.5f,0.5f), Vector4(0.5f,0.5f,0.5f,0.5f) }};

        // Per-cascade VP matrices (viewport-scaled). 4 matrices × 16 floats.
        //_shadowMatrixPalette.
        std::array<float, 64> _shadowMatrixPalette = {};

        // Per-cascade split distances. distances[i] = far distance of cascade i.
        //_shadowCascadeDistances.
        std::array<float, 4> _shadowCascadeDistances = {};

        std::shared_ptr<ShadowMap> _shadowMap;

        std::vector<std::unique_ptr<LightRenderData>> _renderData;

        ShadowType _shadowType = SHADOW_PCF3_32F;
        ShadowType _requestedShadowType = SHADOW_PCF3_32F;

        GraphNode* _node = nullptr;

        float _shadowDistance = 40.0f;

        int _shadowResolution = 1024; // upstream's default
        int _vsmBlurSize = 11;
        float _vsmBias = 0.0025f;
        float _penumbraSize = 1.0f;
        float _penumbraFalloff = 1.0f;

        //_shadowBias (-0.0005 default in the upstream engine).
        float _shadowBias = -0.0005f;

        //_normalOffsetBias.
        float _normalBias = 0.0f;

        //shadowIntensity.
        float _shadowIntensity = 1.0f;

        // Computed shadow VP matrix for local lights (set during shadow camera positioning).
        Matrix4 _shadowViewProjection = Matrix4::identity();

        // Range and cone angle — synced from LightComponent for shadow camera setup.
        float _range = 10.0f;
        float _outerConeAngle = 45.0f;
    };
}
