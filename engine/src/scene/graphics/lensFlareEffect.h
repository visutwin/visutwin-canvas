// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.10.2026
//
// A light-driven lens flare as a camera frame effect: each LensFlare follows a light (or an
// entity, a position or a direction) and places the shapes of its look along the axis from the
// source through the centre of the screen. The shapes are textures the effect copies into one
// texture array (the shape library), shared by all its flares. A flare fades as its source is
// covered by the scene, measured on the GPU against the scene depth and eased over
// occlusionFade, as it leaves the screen, with the distance (fadeStart / fadeEnd), and for a
// spot light as the camera leaves the cone.
//
// The flares are drawn at `scale` of the scene's resolution into a texture the composition adds
// to the scene in linear HDR, before tone mapping (the hdr compose slot); the debug view
// "lensflare" (RenderingSettings::composeDebug) shows the flares alone. Opt-in:
//
//     auto lensFlare = std::make_shared<LensFlareEffect>();
//     lensFlare->setShapes({512, {{"glow", glowTexture}, {"disc", discTexture}}});
//     auto look = std::make_shared<LensFlareLook>();
//     look->elements = {{.shape = "glow", .size = 0.5f}, {.shape = "disc", .offset = 0.5f, .size = 0.08f}};
//     lensFlare->add(std::make_shared<LensFlare>(look, LensFlareSource{.light = sunLight}));
//     camera->addFrameEffect(lensFlare);
//
// The passes sit at the postResolve stage: the shape copy (only after the library changed), the
// occlusion (the visibility of each source, one texel per flare, from the previous frame's) and
// the sprites.
//
// DEVIATIONS: the occlusion fade advances by the world's time (the effect's dt), not the wall
// clock, so it pauses with the world; a flare follows an Entity rather than any graph node; the
// elements are placed on the CPU every frame (see lens-flare.slang).
//
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cameraFrameEffect.h"
#include "core/math/color.h"
#include "core/math/vector3.h"
#include "framework/destroyWatch.h"

namespace visutwin::canvas
{
    class Entity;
    class LightComponent;
    class RenderTarget;
    class VertexBuffer;

    /// One element of a lens flare look: a shape placed on the axis from the flare's source
    /// through the centre of the screen.
    struct LensFlareElement
    {
        /// The name of the shape in the effect's shape library.
        std::string shape;
        /// The position along the axis: 0 at the source, 1 at the centre of the screen, 2
        /// mirrored across the centre, negative beyond the source.
        float offset = 0.0f;
        /// The height of the element, as a fraction of the screen height.
        float size = 0.1f;
        /// The width of the element relative to its height.
        float aspect = 1.0f;
        /// The rotation of the element, in degrees.
        float rotation = 0.0f;
        /// Turns the element to align with the axis, for streaks.
        bool autoRotate = false;
        /// The colour, linear.
        Color color{1.0f, 1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        /// The number of copies spread along the axis around the offset: a train of ghosts.
        int count = 1;
        /// The distance along the axis the copies are spread over, in the units of the offset.
        float spread = 0.0f;
    };

    /// The look of a lens flare, shared by any number of flares.
    struct LensFlareLook
    {
        std::vector<LensFlareElement> elements;
    };

    /// What a flare follows. A directional light gives the direction of the source, an omni or
    /// spot light its position, and a spot light also limits the flare to its cone; without a
    /// light, the entity's position, then the position, then the direction (a source infinitely
    /// far away) are the source.
    struct LensFlareSource
    {
        LightComponent* light = nullptr;
        Entity* entity = nullptr;
        std::optional<Vector3> position;
        std::optional<Vector3> direction;
    };

    /// A lens flare: a look placed in the world, following its source. Its settings are read
    /// every frame; the look's elements are built into the effect when it changes (setLook,
    /// which also takes the same look again after its elements were edited).
    class LensFlare
    {
    public:
        explicit LensFlare(std::shared_ptr<const LensFlareLook> look = nullptr, const LensFlareSource& source = {});

        bool enabled = true;
        /// Multiplies the intensity of the elements.
        float intensity = 1.0f;
        /// Tints the elements; linear.
        Color color{1.0f, 1.0f, 1.0f, 1.0f};
        /// Multiplies the size of the elements.
        float scale = 1.0f;
        /// The light's colour tints the flare too.
        bool useLightColor = true;
        /// The light's intensity scales the flare, so it dims and brightens with its light and
        /// disappears at 0; under physical units the light's luminance, as it scales the lighting.
        bool useLightIntensity = true;
        /// The radius around the source the scene depth is tested in, as a fraction of the screen
        /// height: the flare fades with the fraction of it the scene covers.
        float occlusionRadius = 0.02f;
        /// The time the measured visibility takes to follow a change, in seconds; 0 follows at once.
        float occlusionFade = 0.15f;
        /// How far the depth test is moved from the source towards the camera, in world units, so
        /// the light's own geometry (a bulb) does not cover its flare.
        float occlusionOffset = 0.0f;
        /// The band at the edge of the screen the flare fades over as its source leaves the
        /// screen, as a fraction of half the screen.
        float edgeFade = 0.2f;
        /// The flare keeps drawing while its source is just outside the screen, within edgeFade.
        bool allowOffScreen = true;
        /// The distance from the camera a positional source starts to fade out at, and is gone
        /// at, in world units; no fade unless fadeEnd > fadeStart.
        float fadeStart = 0.0f;
        float fadeEnd = 0.0f;

        [[nodiscard]] const std::shared_ptr<const LensFlareLook>& look() const { return _look; }
        void setLook(std::shared_ptr<const LensFlareLook> look);

        [[nodiscard]] const LensFlareSource& source() const { return _source; }
        /// Follows another source; a light's or an entity's destruction clears it.
        void setSource(const LensFlareSource& source);

        /// Changes with the look, so the effect rebuilds the elements.
        [[nodiscard]] uint64_t version() const { return _version; }

    private:
        std::shared_ptr<const LensFlareLook> _look;
        LensFlareSource _source;
        DestroyWatch _lightWatch;
        DestroyWatch _entityWatch;
        uint64_t _version = 0;
    };

    /// The shapes of the flares: textures with the shape in the colour channels and its coverage
    /// in alpha (a white shape on transparent, or a grey shape on black), copied into the effect
    /// at `resolution` (16-4096) square. A null texture leaves its layer empty.
    struct LensFlareShapeLibrary
    {
        int resolution = 512;
        /// The shapes are gamma-encoded (authored sRGB, as image files are) and are decoded as
        /// they are copied; false for linear shapes.
        bool srgb = true;
        std::vector<std::pair<std::string, Texture*>> shapes;
    };

    class LensFlareEffect final : public CameraFrameEffect
    {
    public:
        static constexpr int kMaxFlares = 32;

        LensFlareEffect();
        ~LensFlareEffect() override;

        /// The brightness of all flares; 0 turns the effect off.
        float intensity = 1.0f;
        /// The resolution the flares are drawn at relative to the scene's, 0.1-1: lower is
        /// cheaper and softens the elements, 1 keeps small sharp elements crisp.
        float scale = 0.5f;

        /// The shapes the elements reference by name; nothing is drawn until it is set. Copied
        /// at the next frame: set it again after editing it, or once a texture has loaded. The
        /// textures are only read then.
        void setShapes(LensFlareShapeLibrary library);
        [[nodiscard]] const LensFlareShapeLibrary& shapes() const { return _library; }

        /// Registers a flare, drawn from the next frame; the first kMaxFlares are drawn.
        void add(const std::shared_ptr<LensFlare>& flare);
        void remove(const std::shared_ptr<LensFlare>& flare);
        [[nodiscard]] const std::vector<std::shared_ptr<LensFlare>>& flares() const { return _flares; }

        /// Enabled, with a positive intensity and a flare.
        [[nodiscard]] bool active() const override;
        [[nodiscard]] bool ownsPasses() const override { return true; }
        [[nodiscard]] std::string buildKey() const override;
        void createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes) override;
        void destroyPasses() override;
        void update() override;
        void frameUpdate(const CameraFrameEffectContext& context) override;

        /// What frameUpdate resolved for each drawn flare this frame: the source in normalized
        /// device coordinates (y up), the fade (enabled, the screen edge, the distance and the
        /// spot cone; not the occlusion), whether the source is on screen, and the depth the
        /// occlusion tests against.
        struct FlareState
        {
            float x = 0.0f;
            float y = 0.0f;
            float fade = 0.0f;
            bool inFront = false;
            bool onScreen = false;
            float sourceDepth = 0.0f;
            Color color{0.0f, 0.0f, 0.0f, 1.0f};
        };
        [[nodiscard]] const std::vector<FlareState>& flareStates() const { return _states; }
        /// The element instances built (copies included), and the ones drawn this frame.
        [[nodiscard]] int elementCount() const { return static_cast<int>(_elements.size()); }
        [[nodiscard]] int drawnElementCount() const { return _drawnElements; }
        /// The texture the flares are drawn to, the shape library and its layer for a name (-1).
        [[nodiscard]] Texture* flareTexture() const { return _flareTexture.get(); }
        [[nodiscard]] Texture* shapeTexture() const { return _shapeTexture.get(); }
        [[nodiscard]] int shapeLayer(const std::string& name) const;

    private:
        class Pass;
        class Geometry;

        struct ElementInstance
        {
            int flare = 0;
            int layer = 0;
            float offset = 0.0f;
            float size = 0.1f;
            float aspect = 1.0f;
            float rotation = 0.0f;   // radians
            bool autoRotate = false;
            float r = 1.0f, g = 1.0f, b = 1.0f;   // colour x intensity
        };

        void rebuildElements();
        [[nodiscard]] int libraryLayers() const;
        [[nodiscard]] int libraryResolution() const;

        std::vector<std::shared_ptr<LensFlare>> _flares;
        LensFlareShapeLibrary _library;
        bool _shapesDirty = true;
        uint64_t _shapesVersion = 0;

        std::vector<ElementInstance> _elements;
        std::string _builtKey;
        std::vector<FlareState> _states;
        int _drawnElements = 0;

        // The passes and what they render into.
        std::shared_ptr<Texture> _shapeTexture;
        std::vector<std::shared_ptr<RenderTarget>> _shapeTargets;
        std::vector<std::shared_ptr<Pass>> _shapePasses;
        std::array<std::shared_ptr<Texture>, 2> _visibility{};
        std::array<std::shared_ptr<RenderTarget>, 2> _visibilityTargets{};
        int _visibilityIndex = 0;
        bool _firstMeasurement = true;
        std::shared_ptr<Texture> _flareTexture;
        std::shared_ptr<RenderTarget> _flareTarget;
        std::shared_ptr<Pass> _occlusionPass;
        std::shared_ptr<Pass> _spritePass;
        std::unique_ptr<Geometry> _occlusionGeometry;
        std::unique_ptr<Geometry> _spriteGeometry;
        std::shared_ptr<GraphicsDevice> _device;
        Texture* _depth = nullptr;
    };
}
