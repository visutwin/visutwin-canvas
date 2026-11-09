// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// The engine's own property types: the entity node, the light, camera, render and element
// components, and the standard material. A property is named after the getter it reads,
// so a frontend's name and the C++ one agree.
//
#include "propertyTable.h"

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/entity.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

namespace visutwin::canvas
{
    namespace
    {
        template <class C>
        std::function<C*(Entity&)> componentOf()
        {
            return [](Entity& entity) { return entity.findComponent<C>(); };
        }

        template <class C>
        void enabledProperty(TypeBuilder<C>& type)
        {
            type.property("enabled", [](const C& c) { return c.enabled(); },
                [](C& c, const bool value) { c.setEnabled(value); });
        }

        void registerNode(PropertyRegistry& registry)
        {
            auto type = registry.registerType<Entity>("node", "The entity: its name, state and local transform.",
                [](Entity& entity) { return &entity; });
            type.property("id", [](const Entity& e) { return e.id(); })
                    .describe("Unique in the process, never reused.")
                .property("name", [](const Entity& e) { return e.name(); },
                    [](Entity& e, const std::string& value) { e.setName(value); })
                .property("enabled", [](const Entity& e) { return e.enabledLocal(); },
                    [](Entity& e, const bool value) { e.setEnabled(value); })
                    .describe("The entity's own flag; it is drawn only when every parent is enabled too.")
                .property("parent", [](const Entity& e) {
                        const auto* parent = dynamic_cast<const Entity*>(e.parent());
                        return parent ? parent->id() : EntityId{};
                    })
                .property("position", [](const Entity& e) { return e.localPosition(); },
                    [](Entity& e, const Vector3& value) { e.setLocalPosition(value); })
                    .describe("Relative to the parent.")
                .property("rotation", [](const Entity& e) { return e.localRotation().getEulerAngles(); },
                    [](Entity& e, const Vector3& value) {
                        e.setLocalEulerAngles(value.getX(), value.getY(), value.getZ());
                    })
                    .unit("deg").describe("Euler angles relative to the parent.")
                .property("scale", [](const Entity& e) { return e.localScale(); },
                    [](Entity& e, const Vector3& value) { e.setLocalScale(value); })
                .property("worldPosition", [](const Entity& e) { return const_cast<Entity&>(e).position(); });
        }

        void registerLight(PropertyRegistry& registry)
        {
            auto type = registry.registerType<LightComponent>("light", "A light component.",
                componentOf<LightComponent>());
            enabledProperty(type);
            type.property("type", &LightComponent::type, &LightComponent::setType)
                    .options({{static_cast<int>(LightType::LIGHTTYPE_DIRECTIONAL), "directional"},
                        {static_cast<int>(LightType::LIGHTTYPE_OMNI), "omni"},
                        {static_cast<int>(LightType::LIGHTTYPE_POINT), "point"},
                        {static_cast<int>(LightType::LIGHTTYPE_SPOT), "spot"}})
                .property("color", &LightComponent::color, &LightComponent::setColor)
                .property("intensity", &LightComponent::intensity, &LightComponent::setIntensity).range(0, 10, 0.05f)
                .property("luminance", &LightComponent::luminance, &LightComponent::setLuminance)
                    .unit("lm").describe("Under Scene::setPhysicalUnits, in place of the intensity.")
                .property("range", &LightComponent::range, &LightComponent::setRange).range(0, 1000, 0.1f)
                .property("innerConeAngle", &LightComponent::innerConeAngle, &LightComponent::setInnerConeAngle)
                    .range(0, 90, 0.5f).unit("deg").describe("Half-angle of the full-strength cone.")
                .property("outerConeAngle", &LightComponent::outerConeAngle, &LightComponent::setOuterConeAngle)
                    .range(0, 90, 0.5f).unit("deg").describe("Half-angle at which a spot's light ends.")
                .property("falloffMode", &LightComponent::falloffMode, &LightComponent::setFalloffMode)
                    .options({{static_cast<int>(LightFalloff::LIGHTFALLOFF_LINEAR), "linear"},
                        {static_cast<int>(LightFalloff::LIGHTFALLOFF_INVERSESQUARED), "inverseSquared"}})
                .property("shape", &LightComponent::shape, &LightComponent::setShape)
                    .options({{static_cast<int>(LightShape::LIGHTSHAPE_PUNCTUAL), "punctual"},
                        {static_cast<int>(LightShape::LIGHTSHAPE_RECT), "rect"},
                        {static_cast<int>(LightShape::LIGHTSHAPE_DISK), "disk"},
                        {static_cast<int>(LightShape::LIGHTSHAPE_SPHERE), "sphere"}})
                .property("castShadows", &LightComponent::castShadows, &LightComponent::setCastShadows)
                .property("shadowType", &LightComponent::shadowType, &LightComponent::setShadowType)
                    .options({{SHADOW_PCF3_32F, "pcf3"}, {SHADOW_VSM_16F, "vsm"}, {SHADOW_PCF5_32F, "pcf5"},
                        {SHADOW_PCF1_32F, "pcf1"}, {SHADOW_PCSS_32F, "pcss"}})
                .property("shadowUpdateMode", &LightComponent::shadowUpdateMode, &LightComponent::setShadowUpdateMode)
                    .options({{static_cast<int>(ShadowUpdateType::SHADOWUPDATE_NONE), "none"},
                        {static_cast<int>(ShadowUpdateType::SHADOWUPDATE_THISFRAME), "thisFrame"},
                        {static_cast<int>(ShadowUpdateType::SHADOWUPDATE_REALTIME), "realtime"}})
                .property("shadowResolution", &LightComponent::shadowResolution, &LightComponent::setShadowResolution)
                    .range(16, 8192)
                .property("shadowDistance", &LightComponent::shadowDistance, &LightComponent::setShadowDistance)
                    .range(0, 1000, 0.5f)
                .property("shadowBias", &LightComponent::shadowBias, &LightComponent::setShadowBias).range(0, 1, 0.001f)
                .property("shadowNormalBias", &LightComponent::shadowNormalBias,
                    &LightComponent::setShadowNormalBias).range(0, 1, 0.001f).describe("In world units.")
                .property("shadowIntensity", &LightComponent::shadowIntensity, &LightComponent::setShadowIntensity)
                    .range(0, 1, 0.01f)
                .property("numCascades", &LightComponent::numCascades, &LightComponent::setNumCascades).range(1, 4, 1)
                .property("cascadeDistribution", &LightComponent::cascadeDistribution,
                    &LightComponent::setCascadeDistribution).range(0, 1, 0.01f)
                .property("cascadeBlend", &LightComponent::cascadeBlend, &LightComponent::setCascadeBlend)
                    .range(0, 1, 0.01f).describe("A fraction of each cascade's end distance.")
                .property("shadowSamples", &LightComponent::shadowSamples, &LightComponent::setShadowSamples)
                    .range(1, 128, 1)
                .property("shadowBlockerSamples", &LightComponent::shadowBlockerSamples,
                    &LightComponent::setShadowBlockerSamples).range(1, 128, 1)
                .property("penumbraSize", &LightComponent::penumbraSize, &LightComponent::setPenumbraSize)
                    .range(0, 10, 0.05f)
                .property("penumbraFalloff", &LightComponent::penumbraFalloff, &LightComponent::setPenumbraFalloff)
                    .range(0, 10, 0.05f)
                .property("vsmBias", &LightComponent::vsmBias, &LightComponent::setVsmBias).range(0, 1, 0.0005f)
                .property("vsmBlurSize", &LightComponent::vsmBlurSize, &LightComponent::setVsmBlurSize).range(1, 25, 1)
                .property("cookieIntensity", &LightComponent::cookieIntensity, &LightComponent::setCookieIntensity)
                    .range(0, 1, 0.01f)
                .property("cookieAngle", &LightComponent::cookieAngle, &LightComponent::setCookieAngle).unit("deg")
                .property("cookieFalloff", &LightComponent::cookieFalloff, &LightComponent::setCookieFalloff)
                .property("volumetricScattering", &LightComponent::volumetricScattering,
                    &LightComponent::setVolumetricScattering).range(0, 10, 0.05f);
        }

        void registerCamera(PropertyRegistry& registry)
        {
            auto type = registry.registerType<CameraComponent>("camera", "A camera component.",
                componentOf<CameraComponent>());
            enabledProperty(type);
            type.property("priority", &CameraComponent::priority, &CameraComponent::setPriority)
                    .describe("Cameras render in increasing priority.")
                .property("projection", [](const CameraComponent& c) { return c.camera()->projection(); },
                    [](CameraComponent& c, const ProjectionType value) { c.camera()->setProjection(value); })
                    .options({{static_cast<int>(ProjectionType::Orthographic), "orthographic"},
                        {static_cast<int>(ProjectionType::Perspective), "perspective"}})
                .property("fov", [](const CameraComponent& c) { return c.camera()->fov(); },
                    [](CameraComponent& c, const float value) { c.camera()->setFov(value); })
                    .range(1, 179, 0.5f).unit("deg").describe("Vertical, unless horizontalFov.")
                .property("horizontalFov", [](const CameraComponent& c) { return c.camera()->horizontalFov(); },
                    [](CameraComponent& c, const bool value) { c.camera()->setHorizontalFov(value); })
                .property("orthoHeight", [](const CameraComponent& c) { return c.camera()->orthoHeight(); },
                    [](CameraComponent& c, const float value) { c.camera()->setOrthoHeight(value); })
                    .range(0, 1000, 0.1f).describe("Half the height of an orthographic view.")
                .property("nearClip", [](const CameraComponent& c) { return c.camera()->nearClip(); },
                    [](CameraComponent& c, const float value) { c.camera()->setNearClip(value); })
                    .range(0, 1000, 0.01f)
                .property("farClip", [](const CameraComponent& c) { return c.camera()->farClip(); },
                    [](CameraComponent& c, const float value) { c.camera()->setFarClip(value); })
                    .range(0, 100000, 1)
                .property("aspectRatio", [](const CameraComponent& c) { return c.camera()->aspectRatio(); })
                .property("clearColor", [](const CameraComponent& c) { return c.camera()->clearColor(); },
                    [](CameraComponent& c, const Color& value) { c.camera()->setClearColor(value); })
                .property("clearColorBuffer",
                    [](const CameraComponent& c) { return c.camera()->clearColorBufferEnabled(); },
                    [](CameraComponent& c, const bool value) { c.camera()->setClearColorBuffer(value); })
                .property("clearDepthBuffer",
                    [](const CameraComponent& c) { return c.camera()->clearDepthBufferEnabled(); },
                    [](CameraComponent& c, const bool value) { c.camera()->setClearDepthBuffer(value); })
                .property("toneMapping", [](const CameraComponent& c) { return static_cast<Tonemap>(c.toneMapping()); },
                    [](CameraComponent& c, const Tonemap value) { c.setToneMapping(value); })
                    .options({{TONEMAP_INHERIT, "inherit"}, {TONEMAP_LINEAR, "linear"}, {TONEMAP_FILMIC, "filmic"},
                        {TONEMAP_HEJL, "hejl"}, {TONEMAP_ACES, "aces"}, {TONEMAP_ACES2, "aces2"},
                        {TONEMAP_NEUTRAL, "neutral"}, {TONEMAP_NONE, "none"}, {TONEMAP_AGX, "agx"},
                        {TONEMAP_AGX_PUNCHY, "agxPunchy"}})
                    .describe("Inherit takes the scene's.")
                .property("aperture", [](const CameraComponent& c) { return c.camera()->aperture(); },
                    [](CameraComponent& c, const float value) { c.camera()->setAperture(value); })
                    .unit("f-stop").describe("Under physical units.")
                .property("shutter", [](const CameraComponent& c) { return c.camera()->shutter(); },
                    [](CameraComponent& c, const float value) { c.camera()->setShutter(value); })
                    .unit("s").describe("Under physical units.")
                .property("sensitivity", [](const CameraComponent& c) { return c.camera()->sensitivity(); },
                    [](CameraComponent& c, const float value) { c.camera()->setSensitivity(value); })
                    .unit("ISO").describe("Under physical units.")
                .property("taaEnabled", [](const CameraComponent& c) { return c.taa().enabled; },
                    [](CameraComponent& c, const bool value) { c.setTaaEnabled(value); })
                .property("ssaoEnabled", [](const CameraComponent& c) { return c.ssao().enabled; },
                    [](CameraComponent& c, const bool value) { c.setSsaoEnabled(value); })
                .property("dofEnabled", [](const CameraComponent& c) { return c.dof().enabled; },
                    [](CameraComponent& c, const bool value) { c.setDofEnabled(value); });
        }

        void registerRender(PropertyRegistry& registry)
        {
            auto type = registry.registerType<RenderComponent>("render", "A render component.",
                componentOf<RenderComponent>());
            enabledProperty(type);
            type.property("type", &RenderComponent::type, &RenderComponent::setType)
                    .describe("A primitive (box, sphere, cylinder, cone, capsule, plane) or asset.")
                .property("castShadows", &RenderComponent::castShadows, &RenderComponent::setCastShadows)
                .property("receiveShadows", &RenderComponent::receiveShadows, &RenderComponent::setReceiveShadows)
                .property("batchGroupId", &RenderComponent::batchGroupId, &RenderComponent::setBatchGroupId)
                    .describe("-1 for none.")
                .property("meshInstanceCount",
                    [](const RenderComponent& c) { return static_cast<int>(c.meshInstances().size()); });
        }

        void registerElement(PropertyRegistry& registry)
        {
            auto type = registry.registerType<ElementComponent>("element", "A UI element component.",
                componentOf<ElementComponent>());
            enabledProperty(type);
            type.property("type", &ElementComponent::type, &ElementComponent::setType)
                    .options({{static_cast<int>(ElementType::Group), "group"},
                        {static_cast<int>(ElementType::Image), "image"}, {static_cast<int>(ElementType::Text), "text"}})
                .property("anchor", &ElementComponent::anchor, &ElementComponent::setAnchor)
                    .describe("Left, bottom, right, top, as fractions of the parent.")
                .property("pivot", &ElementComponent::pivot, &ElementComponent::setPivot)
                .property("margin", &ElementComponent::margin, &ElementComponent::setMargin)
                    .describe("Left, bottom, right, top.")
                .property("width", &ElementComponent::width, &ElementComponent::setWidth)
                    .describe("Ignored on an axis whose anchors differ.")
                .property("height", &ElementComponent::height, &ElementComponent::setHeight)
                .property("calculatedWidth", &ElementComponent::calculatedWidth)
                .property("calculatedHeight", &ElementComponent::calculatedHeight)
                .property("color", &ElementComponent::color, &ElementComponent::setColor)
                .property("opacity", &ElementComponent::opacity, &ElementComponent::setOpacity).range(0, 1, 0.01f)
                .property("text", &ElementComponent::text, &ElementComponent::setText)
                .property("fontSize", &ElementComponent::fontSize, &ElementComponent::setFontSize).range(1, 512, 1)
                .property("lineHeight", &ElementComponent::lineHeight, &ElementComponent::setLineHeight)
                .property("spacing", &ElementComponent::spacing, &ElementComponent::setSpacing)
                .property("letterSpacing", &ElementComponent::letterSpacing, &ElementComponent::setLetterSpacing)
                .property("wrapLines", &ElementComponent::wrapLines, &ElementComponent::setWrapLines)
                .property("maxLines", &ElementComponent::maxLines, &ElementComponent::setMaxLines)
                    .describe("-1 for no limit.")
                .property("autoWidth", &ElementComponent::autoWidth, &ElementComponent::setAutoWidth)
                .property("autoHeight", &ElementComponent::autoHeight, &ElementComponent::setAutoHeight)
                .property("horizontalAlign", &ElementComponent::horizontalAlign, &ElementComponent::setHorizontalAlign)
                    .options({{static_cast<int>(ElementHorizontalAlign::Left), "left"},
                        {static_cast<int>(ElementHorizontalAlign::Center), "center"},
                        {static_cast<int>(ElementHorizontalAlign::Right), "right"}})
                .property("verticalAlign", &ElementComponent::verticalAlign, &ElementComponent::setVerticalAlign)
                    .range(0, 1, 0.01f)
                .property("enableMarkup", &ElementComponent::enableMarkup, &ElementComponent::setEnableMarkup)
                .property("outlineColor", &ElementComponent::outlineColor, &ElementComponent::setOutlineColor)
                .property("outlineThickness", &ElementComponent::outlineThickness,
                    &ElementComponent::setOutlineThickness).range(0, 1, 0.01f)
                .property("shadowColor", &ElementComponent::shadowColor, &ElementComponent::setShadowColor)
                .property("fitMode", &ElementComponent::fitMode, &ElementComponent::setFitMode)
                    .options({{static_cast<int>(ElementFitMode::Stretch), "stretch"},
                        {static_cast<int>(ElementFitMode::Contain), "contain"},
                        {static_cast<int>(ElementFitMode::Cover), "cover"}})
                .property("spriteFrame", &ElementComponent::spriteFrame, &ElementComponent::setSpriteFrame)
                .property("useInput", &ElementComponent::useInput, &ElementComponent::setUseInput)
                .property("mask", &ElementComponent::mask, &ElementComponent::setMask)
                .property("drawOrder", &ElementComponent::drawOrder, &ElementComponent::setDrawOrder)
                    .describe("Assigned by the element's screen; set to order by hand.");
        }

        void registerStandardMaterial(PropertyRegistry& registry)
        {
            auto type = registry.registerType<StandardMaterial>("standard",
                "A standard material: handed over directly, carried by no entity.");
            type.property("diffuse", &StandardMaterial::diffuse, &StandardMaterial::setDiffuse)
                    .describe("Authored in gamma space.")
                .property("opacity", &StandardMaterial::opacity, &StandardMaterial::setOpacity).range(0, 1, 0.01f)
                .property("useMetalness", &StandardMaterial::useMetalness, &StandardMaterial::setUseMetalness)
                    .describe("The metalness workflow; off, the specular colour gives F0.")
                .property("metalness", &StandardMaterial::metalness, &StandardMaterial::setMetalness)
                    .range(0, 1, 0.01f)
                .property("specular", &StandardMaterial::specular, &StandardMaterial::setSpecular)
                .property("specularityFactor", &StandardMaterial::specularityFactor,
                    &StandardMaterial::setSpecularityFactor).range(0, 1, 0.01f)
                .property("gloss", &StandardMaterial::gloss, &StandardMaterial::setGloss).range(0, 1, 0.01f)
                .property("glossInvert", &StandardMaterial::glossInvert, &StandardMaterial::setGlossInvert)
                    .describe("Read the gloss as roughness.")
                .property("emissive", &StandardMaterial::emissive, &StandardMaterial::setEmissive)
                .property("emissiveIntensity", &StandardMaterial::emissiveIntensity,
                    &StandardMaterial::setEmissiveIntensity).range(0, 100, 0.1f)
                .property("ambient", &StandardMaterial::ambient, &StandardMaterial::setAmbient)
                    .describe("Tints the ambient diffuse.")
                .property("bumpiness", &StandardMaterial::bumpiness, &StandardMaterial::setBumpiness)
                    .range(0, 2, 0.01f)
                .property("reflectivity", &StandardMaterial::reflectivity, &StandardMaterial::setReflectivity)
                    .range(0, 1, 0.01f)
                .property("anisotropy", &StandardMaterial::anisotropy, &StandardMaterial::setAnisotropy)
                    .range(0, 1, 0.01f)
                .property("anisotropyRotation", &StandardMaterial::anisotropyRotation,
                    &StandardMaterial::setAnisotropyRotation).unit("deg")
                .property("clearCoat", &StandardMaterial::clearCoat, &StandardMaterial::setClearCoat)
                    .range(0, 1, 0.01f)
                .property("clearCoatGloss", &StandardMaterial::clearCoatGloss, &StandardMaterial::setClearCoatGloss)
                    .range(0, 1, 0.01f)
                .property("transmissionFactor", &StandardMaterial::transmissionFactor,
                    &StandardMaterial::setTransmissionFactor).range(0, 1, 0.01f)
                .property("refractionIndex", &StandardMaterial::refractionIndex, &StandardMaterial::setRefractionIndex)
                    .range(1, 3, 0.01f).describe("An index of refraction.")
                .property("thickness", &StandardMaterial::thickness, &StandardMaterial::setThickness)
                .property("twoSidedLighting", &StandardMaterial::twoSidedLighting,
                    &StandardMaterial::setTwoSidedLighting)
                .property("useLighting", &StandardMaterial::useLighting, &StandardMaterial::setUseLighting)
                    .describe("Off: no direct lights.")
                .property("unlit", &StandardMaterial::unlit, &StandardMaterial::setUnlit)
                .property("useFog", &StandardMaterial::useFog, &StandardMaterial::setUseFog)
                .property("useTonemap", &StandardMaterial::useTonemap, &StandardMaterial::setUseTonemap)
                .property("useSkybox", &StandardMaterial::useSkybox, &StandardMaterial::setUseSkybox)
                .property("opacityFadesSpecular", &StandardMaterial::opacityFadesSpecular,
                    &StandardMaterial::setOpacityFadesSpecular);
        }
    }

    void registerBuiltinProperties(PropertyRegistry& registry)
    {
        registerNode(registry);
        registerLight(registry);
        registerCamera(registry);
        registerRender(registry);
        registerElement(registry);
        registerStandardMaterial(registry);
    }
}
