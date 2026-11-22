// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// Renderer: the forward layer's lights: the main light array and its shadow and cookie
// slots, the clustered lights and the layer's environment.
#include "rendererInternal.h"

namespace visutwin::canvas
{
    using namespace renderer_internal;

    namespace
    {
        // Light cookie slot pools: two 2D (spot) and two cubemap (omni) slots,
        // matching the local-shadow slot count.
        constexpr int kMaxCookies = 2;

        float toRadians(const float degrees)
        {
            return degrees * (std::numbers::pi_v<float> / 180.0f);
        }

        float toDegrees(const float radians)
        {
            return radians * (180.0f / std::numbers::pi_v<float>);
        }

        GpuLightData makeGpuLight(const LightComponent& lightComponent, const bool physicalUnits)
        {
            GpuLightData lightData{};
            switch (lightComponent.type()) {
                case LightType::LIGHTTYPE_DIRECTIONAL:
                    lightData.type = GpuLightType::Directional;
                    break;
                case LightType::LIGHTTYPE_SPOT:
                    lightData.type = GpuLightType::Spot;
                    break;
                case LightType::LIGHTTYPE_OMNI:
                case LightType::LIGHTTYPE_POINT:
                default:
                    lightData.type = GpuLightType::Point;
                    break;
            }

            lightData.position = lightComponent.position();
            lightData.direction = lightComponent.direction();
            if (lightData.direction.lengthSquared() > 1e-8f) {
                lightData.direction = lightData.direction.normalized();
            } else {
                lightData.direction = Vector3(0.0f, -1.0f, 0.0f);
            }
            lightData.color = lightComponent.color();
            lightData.intensity = std::max(lightComponent.renderIntensity(physicalUnits), 0.0f);
            lightData.range = std::max(lightComponent.range(), 1e-4f);
            // inner/outerConeAngle are HALF-angles in degrees (the spot shadow/cookie
            // cameras use `fov = outerConeAngle * 2`). Halving them here made every spot
            // cone half as wide as the shadow and cookie frustum fitted to the same
            // light — visible as a beam covering only the middle of its cookie.
            lightData.innerConeCos = std::cos(toRadians(std::max(lightComponent.innerConeAngle(), 0.0f)));
            lightData.outerConeCos = std::cos(toRadians(std::max(lightComponent.outerConeAngle(), 0.0f)));
            if (lightData.innerConeCos < lightData.outerConeCos) {
                lightData.innerConeCos = lightData.outerConeCos;
            }
            lightData.falloffModeLinear = lightComponent.falloffMode() == LightFalloff::LIGHTFALLOFF_LINEAR;
            lightData.castShadows = lightComponent.castShadows();

            // An area source: the world matrix's X and Z
            // axes, halved — its COLUMNS (row 0 would be the X component of all three
            // axes, a vector outside the light's own plane once it is rotated).
            lightData.shape = static_cast<uint32_t>(lightComponent.shape());
            if (lightData.shape != 0u && lightComponent.entity()) {
                const auto& wt = lightComponent.entity()->worldTransform();
                lightData.areaHalfWidth = Vector3(wt.getColumn(0)) * -0.5f;
                lightData.areaHalfHeight = Vector3(wt.getColumn(2)) * 0.5f;
            }
            return lightData;
        }

        // DEVIATION: up to kMaxDirectionalShadows directional shadows per layer,
        // where upstream samples every directional caster's map. Each shadowed
        // light takes a slot (its uniform block and texture) and carries the slot
        // index as its shadowMapIndex; the shaders gate the cascade lookup on the
        // light's own flag and index, so a light without a slot must be told it
        // casts nothing or it would be darkened by ANOTHER light's shadow — a
        // shadowless fill light carrying the key light's shadows. The filter (PCF, PCSS, VSM) is chosen per
        // shader variant, so every slot must share the first slot's shadow type.
        //
        // `camera` is the camera drawing; the cascades it samples are its own fit, or the
        // light's latest one (Light::directionalFitCamera).
        void assignDirectionalShadowSlot(const LightComponent& lightComponent, Camera* camera,
            GpuLightData& lightData, ShadowParams& shadowParams)
        {
            Light* sceneLight = lightComponent.light();
            Camera* fitCamera = sceneLight ? sceneLight->directionalFitCamera(camera) : camera;
            const bool vsm = sceneLight && sceneLight->shadowType() == SHADOW_VSM_16F;
            const bool pcss = sceneLight && sceneLight->shadowType() == SHADOW_PCSS_32F;
            const int slot = shadowParams.directionalCount;
            const char* refusal = nullptr;
            if (!sceneLight || !sceneLight->shadowMap()) {
                refusal = "";   // no map yet (first frame): unshadowed, silently
            } else if (slot >= ShadowParams::kMaxDirectionalShadows) {
                refusal = "more directional lights cast shadows on one layer than there are "
                          "directional shadow slots; the extra ones are unshadowed";
            } else if (slot > 0 && sceneLight->shadowType() != shadowParams.directionalType) {
                refusal = "a directional light's shadow type differs from the first shadowed "
                          "directional light's on the same layer; it is unshadowed";
            }
            if (refusal) {
                lightData.castShadows = false;
                static bool warned = false;
                if (*refusal && !warned) {
                    warned = true;
                    spdlog::warn("Renderer: {}", refusal);
                }
                return;
            }

            lightData.shadowMapIndex = slot;
            shadowParams.directionalCount = slot + 1;
            shadowParams.enabled = true;
            shadowParams.vsm = vsm;
            shadowParams.pcss = pcss;
            shadowParams.directionalType = sceneLight->shadowType();
            auto& dir = shadowParams.directional[slot];
            dir.shadowMap = sceneLight->shadowMap()->shadowTexture();
            dir.normalBias = lightComponent.shadowNormalBias();
            dir.strength = lightComponent.shadowIntensity();
            if (slot == 0) {
                // The tap counts are the variant's, one pair for both slots.
                shadowParams.pcssSamples = sceneLight->shadowSamples();
                shadowParams.pcssBlockerSamples = sceneLight->shadowBlockerSamples();
            }

            // CSM: copy the full matrix palette and cascade distances.
            // A shadow with shadow bounds is one map with no cascades, blend or distance.
            dir.numCascades = sceneLight->shadowCascadeCount();
            dir.cascadeBlend = sceneLight->usesCascadeBlend() ? sceneLight->cascadeBlend() : 0.0f;
            std::memcpy(dir.shadowMatrixPalette, sceneLight->shadowMatrixPalette(fitCamera).data(),
                        sizeof(dir.shadowMatrixPalette));
            std::memcpy(dir.shadowCascadeDistances, sceneLight->shadowCascadeDistances(fitCamera).data(),
                        sizeof(dir.shadowCascadeDistances));

            // For PCF: a fixed small shader-side depth bias; the real bias work
            // is hardware polygon offset (depthBias) in the shadow pass, which is
            // slope-aware. For VSM_16F the slot carries the vsmBias instead: it
            // sets the minVariance floor in chebyshevUpperBound, which decides how
            // aggressively low-variance (noisy) samples are clamped to lit. Too
            // small flickers; too large detaches contact shadows. The
            // default is 0.0025.
            //
            // The stored depth is normalised over the shadow camera's depth range,
            // which the fit sets to the casters' extent along the light every frame,
            // so a bias authored in world-ish units is divided by that range (over 7),
            // as a VSM spot's is by its range. The FIRST cascade's fitted far clip
            // stands for every cascade.
            LightRenderData* rd = sceneLight->getRenderData(fitCamera, 0);
            if (vsm) {
                const float farClip = rd && rd->shadowCamera ? rd->shadowCamera->farClip() : 0.0f;
                dir.bias = sceneLight->vsmBias() / (std::max(farClip, 1e-4f) / 7.0f);
            } else {
                dir.bias = 0.0001f;
            }
            dir.penumbraSize = sceneLight->penumbraSize();
            dir.penumbraFalloff = sceneLight->penumbraFalloff();

            if (rd && rd->shadowCamera && rd->shadowCamera->node()) {
                dir.viewProjection = rd->shadowCamera->projectionMatrix()
                    * rd->shadowCamera->node()->worldTransform().inverse();
            }
            if (pcss) {
                for (int cascade = 0; cascade < dir.numCascades && cascade < 4; ++cascade) {
                    LightRenderData* cascadeData = sceneLight->getRenderData(fitCamera, cascade);
                    if (cascadeData && cascadeData->shadowCamera) {
                        // Under shadow bounds the map is wider than the camera's view by its
                        // border: the world half size of the whole map.
                        const float radius = cascadeData->projectionCompensation > 0.0f
                            ? cascadeData->projectionCompensation : cascadeData->shadowCamera->orthoHeight();
                        dir.pcssCascadeRadii[cascade] = std::max(radius, 1e-4f);
                        dir.pcssCascadeDepthRanges[cascade] = std::max(
                            cascadeData->shadowCamera->farClip() - cascadeData->shadowCamera->nearClip(), 1e-4f);
                    }
                }
            }
        }

        // Wire local light shadow data (spot/point) for the NON-clustered path.
        // Omni lights use cubemap depth textures; spot lights use 2D textures.
        // Under clustered lighting every local light takes its shadow from the
        // LightTextureAtlas through the cluster grid, so none may consume one of
        // the two main-array slots nor be stripped of castShadows when they run
        // out — leaving spots in this branch once capped the whole feature at
        // kMaxLocalShadows, and leaving omnis in it capped them at two while
        // the clustered-lighting default put every scene on this path.
        void assignLocalShadowSlot(const LightComponent& lightComponent, GpuLightData& lightData,
            ShadowParams& shadowParams)
        {
            Light* sceneLight = lightComponent.light();
            if (!sceneLight || !sceneLight->shadowMap() ||
                shadowParams.localShadowCount >= ShadowParams::kMaxLocalShadows) {
                // No shadow slot available — clear castShadows so the shader
                // doesn't attempt to sample a non-existent shadow map.
                lightData.castShadows = false;
                return;
            }

            const int shadowIdx = shadowParams.localShadowCount;
            lightData.shadowMapIndex = shadowIdx;

            const bool isOmni = (sceneLight->type() == LightType::LIGHTTYPE_OMNI);
            auto& ls = shadowParams.localShadows[shadowIdx];
            ls.shadowMap = sceneLight->shadowMap()->shadowTexture();
            ls.isOmni = isOmni;

            // An omni light's cube faces take their near and far from ls.nearClip /
            // ls.farClip below; only a spot projects through a matrix.
            if (!isOmni) {
                ls.viewProjection = sceneLight->shadowViewProjection();
            }
            // Our local-shadow shader subtracts this from the receiver depth,
            // so it needs a POSITIVE value while Light::shadowBias() is a
            // negative internal one — hence the negation. The per-type scaling
            // is spot x20, omni raw. A PCF spot takes NONE: its bias is the shadow
            // pass's polygon offset (drawLocalShadowFace), and its perspective depth
            // is crushed against 1 (near is range / 1000), where the x20 value is
            // larger than the depth span of the whole scene and lit every receiver.
            const bool pcfSpot = !isOmni && sceneLight->shadowType() != SHADOW_PCSS_32F;
            ls.bias = isOmni ? -sceneLight->shadowBias()
                             : (pcfSpot ? 0.0f : -sceneLight->shadowBias() * 20.0f);
            ls.normalBias = sceneLight->normalBias();
            // A VSM spot: a fixed 0.0002 off the
            // distance ratio and the variance bias in place of the normal offset.
            ls.vsm = !isOmni && sceneLight->shadowType() == SHADOW_VSM_16F;
            if (ls.vsm) {
                ls.bias = 0.00001f * 20.0f;
                ls.normalBias = sceneLight->vsmBias() / (std::max(sceneLight->range(), 0.1f) / 7.0f);
            }
            ls.intensity = sceneLight->shadowIntensity();
            ls.nearClip = sceneLight->localShadowNearClip();
            ls.farClip = std::max(sceneLight->range(), 0.1f);

            // PCSS local shadows: blocker-search
            // area in shadow-map UV. Spot scales by the shadow camera's
            // FOV ratio; omni uses the raw penumbra/resolution ratio.
            if (sceneLight->shadowType() == SHADOW_PCSS_32F) {
                const float res = std::max(static_cast<float>(sceneLight->shadowResolution()), 1.0f);
                if (isOmni) {
                    ls.pcssSearchArea = sceneLight->penumbraSize() / res;
                } else {
                    const float fovRad = toRadians(std::min(sceneLight->outerConeAngle() * 2.0f, 179.0f));
                    const float fovRatio = 1.0f / std::max(std::tan(fovRad * 0.5f), 1e-4f);
                    ls.pcssSearchArea = sceneLight->penumbraSize() / res * fovRatio;
                }
            }

            shadowParams.localShadowCount++;
        }

        struct CookieSlots
        {
            int used2D = 0;
            int usedCube = 0;
        };

        // Light cookies: the projected texture masking
        // the light's color. Spot cookies need a world → cookie-UV matrix —
        // identical to the spot shadow VP, so shadow casters reuse theirs and
        // cookie-only lights evaluate it here. Omni cookies are sampled by
        // direction and carry the light's world transform instead.
        // DEVIATION: two slots per kind (like local shadows), not one per light.
        void assignCookieSlot(const LightComponent& lightComponent, GpuLightData& lightData, CookieSlots& slots)
        {
            Light* sceneLight = lightComponent.light();
            if (!sceneLight || !sceneLight->cookie() ||
                lightData.type == GpuLightType::Directional) {
                return;
            }

            const bool isOmniCookie = (sceneLight->type() == LightType::LIGHTTYPE_OMNI ||
                                       sceneLight->type() == LightType::LIGHTTYPE_POINT);
            const bool shapeMatches = (isOmniCookie == sceneLight->cookie()->isCubemap());
            int& poolCount = isOmniCookie ? slots.usedCube : slots.used2D;
            if (!shapeMatches || poolCount >= kMaxCookies) {
                return;
            }

            lightData.cookie = sceneLight->cookie();
            lightData.cookieIndex = poolCount++;
            lightData.cookieIntensity = sceneLight->cookieIntensity();
            lightData.cookieChannel = static_cast<uint32_t>(sceneLight->cookieChannel());
            lightData.cookieFalloff = sceneLight->cookieFalloff();
            lightData.cookieMatrix = isOmniCookie
                ? lightComponent.entity()->worldTransform()
                : LightCamera::evalSpotCookieTextureMatrix(*sceneLight);
            if (!isOmniCookie) {
                // The cookie offset is added to the projected xy before the
                // clip test; a translation by offset * w in front of the projection is
                // exactly that, so the offset needs no uniform of its own.
                const Vector2& offset = sceneLight->cookieOffset();
                if (offset.x != 0.0f || offset.y != 0.0f) {
                    lightData.cookieMatrix = Matrix4::translation(offset.x, offset.y, 0.0f) * lightData.cookieMatrix;
                }
                sceneLight->cookieTransform().store(lightData.cookieTransform);
            }
        }
    }

    namespace renderer_internal
    {
        // Whether a layer may have clustered lights: an active spot or omni light rendering it.
        // The shader preparation's guess at what renderForwardLayer decides from the layer's
        // gathered cluster list, so it compiles the variants the layer will draw with.
        bool layerMayHaveClusteredLights(const Layer& layer, const Scene* scene)
        {
            for (const auto* lightComponent : instancesOf<LightComponent>(scene ? scene->componentRegistry() : nullptr)) {
                if (lightComponent && lightComponent->active() &&
                    lightComponent->type() != LightType::LIGHTTYPE_DIRECTIONAL &&
                    lightComponent->rendersLayer(layer.id())) {
                    return true;
                }
            }
            return false;
        }

        void gatherForwardLights(const ForwardView& view, const Layer& layer, const Scene* scene,
            const bool clusteredEnabled, Camera* camera, ForwardLights& out)
        {
            const bool physicalUnits = scene && scene->physicalUnits();
            CookieSlots cookieSlots;

            for (const auto* lightComponent : instancesOf<LightComponent>(scene ? scene->componentRegistry() : nullptr)) {
                if (!lightComponent || !lightComponent->active()) {
                    continue;
                }
                if (!lightComponent->rendersLayer(layer.id())) {
                    continue;
                }
                if (view.cameraComponent && !view.cameraComponent->rendersLayer(layer.id())) {
                    continue;
                }

                GpuLightData lightData = makeGpuLight(*lightComponent, physicalUnits);
                // A shaped DIRECTIONAL light's LTC source sits at this camera's far clip,
                // back along its direction, with its extents scaled by the same distance:
                // it subtends the angle its entity's size gives, wherever the entity is.
                // Taken at the entity, a 0.2-unit disk held 5000 units out (lights-and-shadows/area-lights'
                // sky disk) gives a reflection a hundred times too faint.
                if (lightData.type == GpuLightType::Directional && lightData.shape != 0u && camera) {
                    const float far = camera->farClip();
                    lightData.position = view.position - lightData.direction * far;
                    lightData.areaHalfWidth = lightData.areaHalfWidth * far;
                    lightData.areaHalfHeight = lightData.areaHalfHeight * far;
                }
                // A light of zero intensity lights nothing, but one that casts a shadow
                // stays: its shadow still reaches a shadow catcher, the documented way to
                // catch a shadow without lighting the scene. The cluster grid drops it.
                if (lightData.intensity <= 0.0f && !(lightData.castShadows &&
                        (lightData.type == GpuLightType::Directional || !clusteredEnabled))) {
                    continue;
                }

                if (lightData.castShadows) {
                    if (lightData.type == GpuLightType::Directional) {
                        assignDirectionalShadowSlot(*lightComponent, camera, lightData, out.shadowParams);
                    } else if (!clusteredEnabled) {
                        assignLocalShadowSlot(*lightComponent, lightData, out.shadowParams);
                    }
                }
                assignCookieSlot(*lightComponent, lightData, cookieSlots);

                LightDispatchEntry dispatchEntry{};
                dispatchEntry.light = lightData;
                dispatchEntry.mask = lightComponent->mask();
                dispatchEntry.sceneLight = lightComponent->light();

                if (dispatchEntry.light.type == GpuLightType::Directional) {
                    out.directional.push_back(dispatchEntry);
                    continue;
                }

                // Cull the light against THIS camera. Light::visibleThisFrame is a
                // union over cameras and answers a different question — whether the
                // light's shadow map and cookie are worth rendering at all — so it
                // cannot stand in for this test: a light visible only to a reflection
                // camera would otherwise light the main view from off screen.
                //
                // Both the main array and the cluster grid are fed from this list, so
                // one test covers the two of them.
                if (view.hasFrustum && dispatchEntry.sceneLight) {
                    const BoundingSphere bounds = dispatchEntry.sceneLight->boundingSphere();
                    if (!view.frustum.checkSphere(bounds.center(), bounds.radius())) {
                        continue;
                    }
                    dispatchEntry.screenSize = view.camera->screenSize(bounds);
                }
                out.local.push_back(dispatchEntry);
            }

            // The main light array holds eight. Rank by apparent size so that when a
            // scene has more visible local lights than slots, the ones covering most of
            // the picture get them — authoring order decides nothing about that.
            // Stable, so an exact tie keeps authoring order and the choice stays
            // reproducible frame to frame.
            std::stable_sort(out.local.begin(), out.local.end(),
                [](const LightDispatchEntry& a, const LightDispatchEntry& b) {
                    return a.screenSize > b.screenSize;
                });
        }

        // Environment uniforms are constant across the entire layer (depend only on
        // the scene, not on per-draw state), so they are set once per layer rather
        // than per draw.
        void bindLayerEnvironment(GraphicsDevice& device, const Scene* scene, const ForwardView& view)
        {
            const bool isDome = skyIsDome(scene);
            const Vector3 skyDomeCenter = isDome ? scene->sky()->centerWorldPos() : Vector3(0, 0, 0);
            device.setEnvironmentUniforms(scene ? scene->envAtlas() : nullptr,
                scene ? scene->skyboxIntensity() : 1.0f,
                static_cast<float>(scene ? scene->skyboxMip() : 0),
                skyDomeCenter, isDome,
                scene ? scene->skybox() : nullptr);
            device.setSkyboxRotation(scene ? scene->skyboxRotation() : Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
            device.setUserShaderData(scene ? scene->userShaderData() : UserShaderData{});

            // A dynamic probe captures the scene into the very cubemap the scene
            // samples for reflections, so during its own face passes the probe
            // texture is simultaneously the render target and a bound sampler —
            // feedback. Skip the binding for those passes: the cube's other faces
            // hold last frame's capture, which is not worth a read-write hazard.
            // Vulkan reports this as a layout conflict (the face is in
            // COLOR_ATTACHMENT_OPTIMAL while the descriptor wants SHADER_READ_ONLY);
            // Metal reads it silently, which is undefined rather than correct.
            if (scene && scene->reflectionProbe()) {
                Texture* probe = scene->reflectionProbe();
                const bool probeIsRenderTarget =
                    view.activeTarget && view.activeTarget->colorBuffer() == probe;
                // Unbind rather than skip: the binding is device state that would
                // otherwise persist from the previous pass, which is exactly the
                // texture we must not sample here. Zero intensity also neutralizes
                // the shader term, since VT_FEATURE_REFLECTION_PROBE stays enabled
                // for the frame.
                if (probeIsRenderTarget) {
                    device.setReflectionProbeUniforms(nullptr,
                        scene->reflectionProbeBoxMin(), scene->reflectionProbeBoxMax(),
                        scene->reflectionProbeBoxProjection(), 0.0f, 0.0f);
                } else {
                    // maxLod = highest mip index (roughness → LOD in the shader).
                    const int levels = std::max(1, static_cast<int>(probe->getNumLevels()));
                    device.setReflectionProbeUniforms(probe,
                        scene->reflectionProbeBoxMin(), scene->reflectionProbeBoxMax(),
                        scene->reflectionProbeBoxProjection(),
                        scene->reflectionProbeIntensity(),
                        static_cast<float>(levels - 1));
                }
            }

            // Camera clip planes for SSR depth-grab linearization.
            device.setCameraClipPlanes(view.camera->nearClip(), view.camera->farClip());
            device.setDebugShaderPass(static_cast<uint32_t>(view.camera->debugShaderPass()));
        }

        /// The local lights in WorldClusters' input format, plus the identity of the
        /// set, order-independent: the dispatch list is sorted by apparent size, which
        /// differs per camera, and two layers seeing the same lights must still hash alike.
        void buildClusterLightInput(const std::vector<LightDispatchEntry>& localLights,
            const bool clusteredShadowsEnabled, const bool clusteredCookiesEnabled, const uint32_t clusteredAreaShapes,
            std::vector<ClusterLightData>& lights, std::vector<const void*>& members)
        {
            lights.clear();
            lights.reserve(localLights.size());
            members.clear();

            for (const auto& dispatchEntry : localLights) {
                const auto& ld = dispatchEntry.light;
                // Every spot and omni light lit at runtime is in the grid, shadowed from
                // the atlas when it holds a slot for it and unshadowed otherwise. A
                // bake-only light (neither MASK_AFFECT_DYNAMIC nor _LIGHTMAPPED) lights
                // nothing at runtime, so it stays out of the grid and out of the set's key.
                if (!clusterAdmitsLight(dispatchEntry.mask)) {
                    continue;
                }
                ClusterLightData lcd;
                // The shader accepts the light per draw by these bits against the draw's
                // mesh-instance mask (dynamic or lightmapped).
                lcd.lightMask = dispatchEntry.mask;
                // A shaped light is an area light in the grid only with clustered area
                // lights enabled and its shape among the enabled ones (LightingParams::
                // areaLightShapes); otherwise it shades as punctual. The forward variants
                // leave the code of a disabled shape out of the cluster loop, so a light of
                // that shape must never reach it as one.
                if (ld.shape != 0u && ld.shape < 32u && (clusteredAreaShapes & (1u << ld.shape)) != 0u) {
                    lcd.shape = ld.shape;
                    lcd.areaHalfWidth = ld.areaHalfWidth;
                    lcd.areaHalfHeight = ld.areaHalfHeight;
                }
                lcd.position = ld.position;
                lcd.direction = ld.direction;
                lcd.color = ld.color;
                lcd.intensity = ld.intensity;
                lcd.range = ld.range;
                // Back to the half-angle in degrees that WorldClusters re-cosines.
                lcd.innerConeAngle = toDegrees(std::acos(std::clamp(ld.innerConeCos, -1.0f, 1.0f)));
                lcd.outerConeAngle = toDegrees(std::acos(std::clamp(ld.outerConeCos, -1.0f, 1.0f)));
                lcd.isSpot = (ld.type == GpuLightType::Spot);
                lcd.falloffModeLinear = ld.falloffModeLinear;

                // Clustered shadow: the atlas rect assigned by LightTextureAtlas::update
                // this frame, and for a spot the VP into it computed by cullLocalLights.
                // Shadows switched off for the scene stop the atlas being rendered, so the
                // lights must stop sampling it too, or they keep the last shadows drawn.
                if (clusteredShadowsEnabled && ld.castShadows && dispatchEntry.sceneLight &&
                    dispatchEntry.sceneLight->atlasViewportAllocated()) {
                    Light* atlasLight = dispatchEntry.sceneLight;
                    lcd.castShadows = true;
                    lcd.atlasViewport = atlasLight->atlasViewport();
                    lcd.shadowMatrix = atlasLight->shadowViewProjection();
                    // An omni face stores perspective depth over the light's range; the
                    // receiver is pulled toward the light by the bias, a fraction of the
                    // range (getShadowOmniClusteredPCF3).
                    lcd.shadowNear = atlasLight->localShadowNearClip();
                    lcd.shadowFar = std::max(atlasLight->range(), 0.1f);
                    lcd.shadowRangeBias = -atlasLight->shadowBias();
                    // NOT the non-clustered path's depth bias. A clustered spot's
                    // perspective depth is crushed against 1.0 (near a thousandth of
                    // the range), so the whole scene spans a few thousandths of depth
                    // while that bias is 0.08 — it lit every fragment and erased the
                    // feature. These are biased on render instead (hardware polygon
                    // offset, which the atlas pass already applies), and the receiver
                    // is offset along its normal in the shader.
                    lcd.shadowNormalBias = atlasLight->normalBias();
                    lcd.shadowIntensity = atlasLight->shadowIntensity();
                }
                // Clustered cookie: the same rect of the cookie atlas. A spot that casts
                // no shadow has no shadow camera, so its projection is evaluated here.
                if (Light* cookieLight = dispatchEntry.sceneLight;
                    cookieLight && cookieLight->cookie() && cookieLight->atlasViewportAllocated() &&
                    cookieLight->cookie()->isCubemap() == !lcd.isSpot && clusteredCookiesEnabled) {
                    lcd.hasCookie = true;
                    lcd.cookieIntensity = cookieLight->cookieIntensity();
                    lcd.cookieChannel = static_cast<uint32_t>(cookieLight->cookieChannel());
                    lcd.atlasViewport = cookieLight->atlasViewport();
                    if (lcd.isSpot && !lcd.castShadows) {
                        const LightRenderData* rd = cookieLight->getRenderData(nullptr, 0);
                        lcd.shadowMatrix = LightCamera::evalSpotCookieMatrix(*cookieLight,
                            rd ? rd->shadowViewport : cookieLight->atlasViewport());
                    }
                }
                lights.push_back(lcd);
                members.push_back(dispatchEntry.sceneLight);
            }
        }

        // The main light array for one light mask: directional lights first, then —
        // outside clustered lighting only — the local lights, up to kMaxMainLights in
        // all. Under clustered lighting every spot and omni light, shaped or not, is in
        // the cluster grid, shadowed from the atlas, so none of them enters the main
        // array; it holds only the directional lights then.
        void filterLightsForMask(const ForwardLights& lights, const uint32_t mask, const bool clusteredEnabled,
            std::vector<GpuLightData>& out)
        {
            out.clear();
            for (const auto& dispatchEntry : lights.directional) {
                if ((dispatchEntry.mask & mask) == 0u) continue;
                out.push_back(dispatchEntry.light);
                if (out.size() >= kMaxMainLights) break;
            }
            if (clusteredEnabled) {
                return;
            }
            for (const auto& dispatchEntry : lights.local) {
                if (out.size() >= kMaxMainLights) break;
                if ((dispatchEntry.mask & mask) == 0u) continue;
                out.push_back(dispatchEntry.light);
            }
        }
    }
}
