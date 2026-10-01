// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream scripts/esm/annotations.mjs (AnnotationManager). The deviations are
// listed in the header.
//
#include "annotationManager.h"
#include "annotation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "spdlog/spdlog.h"

#include "core/math/matrix4.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/handlers/fontResource.h"
#include "framework/input/elementInput.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/input/mouse.h"
#include "platform/input/touchDevice.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/composition/layerComposition.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "scene/sprite.h"
#include "scene/textureAtlas.h"

namespace visutwin::canvas
{
    namespace
    {
        // Upstream's stylesheet (.pc-annotation and friends), in points.
        constexpr float kTooltipPadding = 8.0f;
        constexpr float kTooltipMaxContentWidth = 200.0f;
        constexpr float kTooltipOffsetX = 25.0f;
        constexpr float kTitleMarginBottom = 4.0f;
        constexpr float kTooltipAlpha = 0.8f;
        constexpr float kArrowWidth = 8.0f;
        constexpr float kArrowHeight = 16.0f;
        constexpr float kCornerRadius = 4.0f;
        constexpr int kFontSize = 14;
        // CSS `line-height: normal` for a 14 px sans-serif.
        constexpr float kLineHeight = 17.0f;
        constexpr float kFadeSeconds = 0.2f;

        // The hotspot texture: upstream's 64 px canvas, a radius-28 disc with a 6 px ring
        // and "bold 32px" text centred on (32, 33).
        constexpr int kHotspotTextureSize = 64;
        constexpr float kHotspotBorderWidth = 6.0f;
        constexpr float kHotspotLabelSize = 32.0f;
        // textBaseline 'middle' puts the middle of the EM box on the given y; for Arial
        // (ascent 0.905, descent 0.212 em) that is 0.3465 em above the alphabetic baseline.
        constexpr float kMiddleAboveBaseline = 0.3465f;

        // A free layer id: upstream allocates them, this port names them.
        int unusedLayerId(const LayerComposition& layers)
        {
            int id = 1000;
            while (layers.getLayerById(id)) {
                ++id;
            }
            return id;
        }

        /// Source-over of a straight colour `c` at coverage `a` onto a premultiplied pixel.
        struct Pixel
        {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            void over(const float c, const float alpha)
            {
                r = c * alpha + r * (1.0f - alpha);
                g = c * alpha + g * (1.0f - alpha);
                b = c * alpha + b * (1.0f - alpha);
                a = alpha + a * (1.0f - alpha);
            }
        };

        float clamp01(const float v) { return std::clamp(v, 0.0f, 1.0f); }

        /// Bilinear RGB of an RGBA8 image at a pixel coordinate (texel centres at +0.5),
        /// then the median of the three: the MSDF distance, 0.5 at the edge.
        float sampleMsdf(const uint8_t* pixels, const int width, const int height, const float x, const float y)
        {
            const float fx = x - 0.5f;
            const float fy = y - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const int y0 = static_cast<int>(std::floor(fy));
            const float tx = fx - static_cast<float>(x0);
            const float ty = fy - static_cast<float>(y0);
            float rgb[3] = {0.0f, 0.0f, 0.0f};
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const int sx = std::clamp(x0 + i, 0, width - 1);
                    const int sy = std::clamp(y0 + j, 0, height - 1);
                    const float w = (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
                    const uint8_t* p = pixels + (static_cast<size_t>(sy) * width + sx) * 4u;
                    for (int c = 0; c < 3; ++c) {
                        rgb[c] += w * static_cast<float>(p[c]) / 255.0f;
                    }
                }
            }
            return std::max(std::min(rgb[0], rgb[1]), std::min(std::max(rgb[0], rgb[1]), rgb[2]));
        }

        /// Coverage of `label` set in `font` at kHotspotLabelSize, centred as upstream's
        /// canvas centres it (textAlign 'center', textBaseline 'middle' at (32, 33)), for
        /// every pixel of the hotspot texture.
        std::vector<float> rasterizeLabel(const std::string& label, const FontResource& font)
        {
            constexpr int size = kHotspotTextureSize;
            std::vector<float> coverage(static_cast<size_t>(size) * size, 0.0f);
            const float scale = kHotspotLabelSize / 32.0f;   // the fonts' em is 32 units

            std::vector<const FontGlyph*> glyphs;
            float advance = 0.0f;
            int prev = -1;
            for (const unsigned char ch : label) {
                const auto it = font.glyphs.find(ch);
                if (it == font.glyphs.end()) {
                    continue;
                }
                advance += ((prev >= 0 ? font.kerningValue(prev, ch) : 0.0f) + it->second.xadvance) * scale;
                glyphs.push_back(&it->second);
                prev = ch;
            }

            const float baseline = std::floor(size * 0.5f) + 1.0f + kMiddleAboveBaseline * kHotspotLabelSize;
            float pen = std::floor(size * 0.5f) - advance * 0.5f;
            prev = -1;
            for (const FontGlyph* g : glyphs) {
                pen += (prev >= 0 ? font.kerningValue(prev, g->id) : 0.0f) * scale;
                prev = g->id;
                const int pageIndex = g->page >= 0 && g->page < static_cast<int>(font.pages.size()) ? g->page : 0;
                const Texture* page = font.pages.empty() ? font.texture : font.pages[static_cast<size_t>(pageIndex)];
                const auto* pixels = page ? static_cast<const uint8_t*>(page->getLevel(0)) : nullptr;
                if (!pixels || g->width <= 0.0f || g->height <= 0.0f) {
                    pen += g->xadvance * scale;
                    continue;
                }
                const int pageW = static_cast<int>(page->width());
                const int pageH = static_cast<int>(page->height());

                // The glyph's atlas cell, placed as text layout places it (y down here).
                const float quad = scale * (g->width + g->height) * 0.5f / g->scale;
                const float left = pen - g->xoffset * scale;
                const float top = baseline + g->yoffset * scale - quad;
                // Distance-field texels per output pixel.
                const float pxRange = font.pxRange * quad / g->width;
                for (int py = 0; py < size; ++py) {
                    const float v = (static_cast<float>(py) + 0.5f - top) / quad;
                    if (v < 0.0f || v > 1.0f) {
                        continue;
                    }
                    for (int px = 0; px < size; ++px) {
                        const float u = (static_cast<float>(px) + 0.5f - left) / quad;
                        if (u < 0.0f || u > 1.0f) {
                            continue;
                        }
                        const float distance = sampleMsdf(pixels, pageW, pageH, g->x + u * g->width,
                                                          g->y + v * g->height);
                        float& c = coverage[static_cast<size_t>(py) * size + px];
                        c = std::max(c, clamp01((distance - 0.5f) * pxRange + 0.5f));
                    }
                }
                pen += g->xadvance * scale;
            }
            return coverage;
        }

        std::unique_ptr<Texture> createRgbaTexture(GraphicsDevice* device, const std::string& name,
                                                   const int width, const int height,
                                                   const std::vector<uint8_t>& rgba)
        {
            TextureOptions options;
            options.name = name;
            options.width = static_cast<uint32_t>(width);
            options.height = static_cast<uint32_t>(height);
            options.format = PixelFormat::PIXELFORMAT_RGBA8;
            options.minFilter = FilterMode::FILTER_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;
            options.mipmaps = false;
            auto texture = std::make_unique<Texture>(device, options);
            texture->setLevelData(0, rgba.data(), rgba.size());
            texture->upload();
            return texture;
        }

        /// White with `coverage(x, y)` (pixel centres) as alpha, 4x4 supersampled.
        template <typename Coverage>
        std::vector<uint8_t> whiteShape(const int width, const int height, Coverage&& coverage)
        {
            std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u, 255);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    int inside = 0;
                    for (int sy = 0; sy < 4; ++sy) {
                        for (int sx = 0; sx < 4; ++sx) {
                            inside += coverage(static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 4.0f,
                                               static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 4.0f) ? 1 : 0;
                        }
                    }
                    rgba[(static_cast<size_t>(y) * width + x) * 4u + 3u] =
                        static_cast<uint8_t>(std::lround(255.0f * static_cast<float>(inside) / 16.0f));
                }
            }
            return rgba;
        }
    }

    AnnotationManager::~AnnotationManager()
    {
        teardown();
    }

    void AnnotationManager::setHotspotSize(const float value)
    {
        if (_hotspotSize == value) {
            return;
        }
        _hotspotSize = value;
        // Upstream rewrites the stylesheet that sizes the hotspot DOM elements.
        for (const auto& resources : _annotationResources) {
            if (auto* element = resources->hotspot ? resources->hotspot->findComponent<ElementComponent>() : nullptr) {
                element->setWidth(_hotspotSize + 5.0f);
                element->setHeight(_hotspotSize + 5.0f);
            }
        }
    }

    void AnnotationManager::setHotspotColor(const Color& value)
    {
        if (_hotspotColor == value) {
            return;
        }
        _hotspotColor = value;
        // Only non-hovered annotations change
        for (const auto& resources : _annotationResources) {
            if (resources->annotation != _hoverAnnotation) {
                for (const auto& material : resources->materials) {
                    material->setEmissive(_hotspotColor);
                }
            }
        }
    }

    void AnnotationManager::setHoverColor(const Color& value)
    {
        if (_hoverColor == value) {
            return;
        }
        _hoverColor = value;
        // Update the currently hovered annotation if any
        if (_hoverAnnotation) {
            setAnnotationHover(_hoverAnnotation, true);
        }
    }

    void AnnotationManager::setFonts(FontResource* regular, FontResource* bold)
    {
        _regularFont = regular;
        _boldFont = bold ? bold : regular;
        if (_title) {
            _title->setFontResource(_boldFont);
        }
        if (_text) {
            _text->setFontResource(_regularFont);
        }
    }

    AnnotationManager::Resources* AnnotationManager::resourcesFor(const Annotation* annotation)
    {
        for (const auto& resources : _annotationResources) {
            if (resources->annotation == annotation) {
                return resources.get();
            }
        }
        return nullptr;
    }

    std::unique_ptr<Texture> AnnotationManager::createHotspotTexture(const std::string& label) const
    {
        constexpr int size = kHotspotTextureSize;
        const float center = size / 2.0f;
        const float radius = size / 2.0f - 4.0f;

        std::vector<float> text;
        if (_boldFont && _boldFont->msdf) {
            text = rasterizeLabel(label, *_boldFont);
        } else if (!label.empty()) {
            spdlog::warn("AnnotationManager: no MSDF font set (setFonts), hotspot labels are not drawn");
        }

        std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4u);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - center;
                const float dy = static_cast<float>(y) + 0.5f - center;
                const float d = std::sqrt(dx * dx + dy * dy);

                // Cleared to transparent, then a black disc, its white border and the
                // white label, each anti-aliased by its distance to the edge
                Pixel p;
                p.over(0.0f, clamp01(radius - d + 0.5f));
                p.over(1.0f, clamp01(kHotspotBorderWidth * 0.5f - std::abs(d - radius) + 0.5f));
                if (!text.empty()) {
                    p.over(1.0f, text[static_cast<size_t>(y) * size + x]);
                }

                const auto alpha = static_cast<uint8_t>(std::lround(p.a * 255.0f));
                // Set the colour of semitransparent pixels to white so the blending at the
                // edges is correct (upstream's fix-up of the canvas pixels)
                const float c = alpha < 255 ? 1.0f : p.r / std::max(p.a, 1e-6f);
                const auto channel = static_cast<uint8_t>(std::lround(clamp01(c) * 255.0f));
                uint8_t* out = &rgba[(static_cast<size_t>(y) * size + x) * 4u];
                out[0] = out[1] = out[2] = channel;
                out[3] = alpha;
            }
        }
        return createRgbaTexture(entity()->engine()->graphicsDevice().get(), "annotation-hotspot-" + label,
                                 size, size, rgba);
    }

    std::shared_ptr<StandardMaterial> AnnotationManager::createHotspotMaterial(Texture* texture,
        const float opacity, const bool depth) const
    {
        auto material = std::make_shared<StandardMaterial>();

        // Alpha test 0.01. setAlphaMode resets the blend, the depth state and the
        // transparent flag, so it goes first.
        material->setAlphaMode(AlphaMode::MASK);
        material->setAlphaCutoff(0.01f);

        material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        material->setEmissive(_hotspotColor);
        material->setEmissiveMap(texture);
        // DEVIATION: the texture's alpha comes through the diffuse map, as the UI
        // elements' does, where upstream sets it as the opacity map, which is Metal-only
        // here. The diffuse is black, so the map adds no colour.
        material->setDiffuseMap(texture);
        material->setOpacity(opacity);

        BlendState blend;
        blend.setEnabled(true);
        blend.setColorOp(BLENDEQUATION_ADD);
        blend.setColorSrcFactor(BLENDMODE_SRC_ALPHA);
        blend.setColorDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
        blend.setAlphaOp(BLENDEQUATION_ADD);
        blend.setAlphaSrcFactor(BLENDMODE_ONE);
        blend.setAlphaDstFactor(BLENDMODE_ONE);
        material->setBlendState(std::make_shared<BlendState>(blend));
        material->setTransparent(true);

        auto depthState = std::make_shared<DepthState>();
        depthState->setDepthTest(depth);
        depthState->setDepthWrite(depth);
        material->setDepthState(depthState);

        material->setCullMode(CullMode::CULLFACE_NONE);
        material->setUseLighting(false);
        return material;
    }

    void AnnotationManager::setAnnotationHover(Annotation* annotation, const bool hover)
    {
        Resources* resources = resourcesFor(annotation);
        if (!resources) {
            return;
        }
        for (const auto& material : resources->materials) {
            material->setEmissive(hover ? _hoverColor : _hotspotColor);
        }
        annotation->fire("hover", hover);
    }

    void AnnotationManager::showTooltip(Annotation* annotation)
    {
        _activeAnnotation = annotation;
        _tooltipShown = true;
        if (_tooltip) {
            _tooltip->entity()->setEnabled(true);
            layoutTooltip();
        }
        annotation->fire("show", annotation);
    }

    void AnnotationManager::hideTooltip(Annotation* annotation)
    {
        _activeAnnotation = nullptr;
        _tooltipShown = false;

        // Wait for fade out before hiding
        _hidePending = annotation;
        _hideTimer = kFadeSeconds;
    }

    void AnnotationManager::hideAnnotationElements(Annotation* annotation, Resources& resources)
    {
        if (resources.hotspot) {
            resources.hotspot->setEnabled(false);
        }
        if (_activeAnnotation == annotation && _tooltipShown) {
            hideTooltip(annotation);
        }
    }

    void AnnotationManager::layoutTooltip()
    {
        if (!_tooltip || !_activeAnnotation) {
            return;
        }

        // Content as wide as its widest line, wrapping at the maximum width (CSS
        // `width: fit-content; max-width: 200px`): autoWidth measures, and a text wider
        // than the maximum is set again wrapping, which turns autoWidth off BEFORE the text
        const auto fit = [](ElementComponent* element, const std::string& text) {
            element->setWrapLines(false);
            element->setAutoWidth(true);
            element->setText(text);
            if (element->width() > kTooltipMaxContentWidth) {
                element->setAutoWidth(false);
                element->setWidth(kTooltipMaxContentWidth);
                element->setWrapLines(true);
                element->setText(text);
            }
        };
        fit(_title, _activeAnnotation->title());
        fit(_text, _activeAnnotation->text());

        const auto height = [](const ElementComponent* element, const std::string& text) {
            return text.empty() ? 0.0f : element->height();
        };
        const float titleHeight = height(_title, _activeAnnotation->title());
        const float textHeight = height(_text, _activeAnnotation->text());
        const float contentWidth = std::max(_activeAnnotation->title().empty() ? 0.0f : _title->width(),
                                            _activeAnnotation->text().empty() ? 0.0f : _text->width());

        _tooltip->setWidth(contentWidth + 2.0f * kTooltipPadding);
        _tooltip->setHeight(titleHeight + kTitleMarginBottom + textHeight + 2.0f * kTooltipPadding);
        _title->entity()->setLocalPosition(kTooltipPadding, -kTooltipPadding, 0.0f);
        _text->entity()->setLocalPosition(kTooltipPadding, -kTooltipPadding - titleHeight - kTitleMarginBottom, 0.0f);
    }

    void AnnotationManager::onLabelChange(Annotation* annotation)
    {
        Resources* resources = resourcesFor(annotation);
        if (!resources) {
            return;
        }
        auto texture = createHotspotTexture(annotation->label());
        for (const auto& material : resources->materials) {
            material->setEmissiveMap(texture.get());
            material->setDiffuseMap(texture.get());
        }
        resources->texture = std::move(texture);
    }

    void AnnotationManager::onAnnotationEnable(Annotation* annotation)
    {
        Resources* resources = resourcesFor(annotation);
        if (!resources) {
            return;
        }
        if (resources->holder) {
            resources->holder->setEnabled(true);
        }
        if (resources->hotspot) {
            resources->hotspot->setEnabled(true);
        }
    }

    void AnnotationManager::onAnnotationDisable(Annotation* annotation)
    {
        Resources* resources = resourcesFor(annotation);
        if (!resources) {
            return;
        }
        // An entity being destroyed disables its scripts first; its nodes are going too
        if (resources->holder && !resources->holder->destroying()) {
            resources->holder->setEnabled(false);
        }
        if (resources->hotspot) {
            resources->hotspot->setEnabled(false);
        }
        if (_activeAnnotation == annotation) {
            hideTooltip(annotation);
        }
        if (_hoverAnnotation == annotation) {
            _hoverAnnotation = nullptr;
            setAnnotationHover(annotation, false);
        }
    }

    void AnnotationManager::registerAnnotation(Annotation* annotation)
    {
        if (!annotation || resourcesFor(annotation) || !annotation->entity()) {
            return;
        }
        Engine* engine = entity()->engine();

        auto resources = std::make_unique<Resources>();
        resources->annotation = annotation;

        // Create texture and materials
        resources->texture = createHotspotTexture(annotation->label());
        resources->materials[0] = createHotspotMaterial(resources->texture.get(), 1.0f, true);
        resources->materials[1] = createHotspotMaterial(resources->texture.get(), _behindOpacity, false);

        // The node that faces the camera and takes the hotspot's size, holding the base
        // entity (depth tested) and the overlay entity (drawn over geometry)
        resources->holder = new Entity();
        resources->holder->setEngine(engine);
        resources->holder->setName("hotspot");
        const char* names[2] = {"base", "overlay"};
        for (int i = 0; i < 2; ++i) {
            auto* quad = new Entity();
            quad->setEngine(engine);
            quad->setName(names[i]);
            auto* render = static_cast<RenderComponent*>(quad->addComponent<RenderComponent>());
            if (render) {
                render->setLayers({_layers[i]->id()});
                render->setCastShadows(false);
                render->setMaterial(resources->materials[i].get());
                render->setType("plane");
                for (auto* meshInstance : render->meshInstances()) {
                    meshInstance->setCull(false);
                }
            }
            resources->holder->addChild(quad);
        }
        annotation->entity()->addChild(resources->holder);

        // The clickable hotspot, (size + 5) pixels square around the hotspot's centre
        if (_screen) {
            auto* hotspot = new Entity();
            hotspot->setEngine(engine);
            hotspot->setName("annotation-hotspot");
            if (auto* element = static_cast<ElementComponent*>(hotspot->addComponent<ElementComponent>())) {
                element->setup({.type = ElementType::Group, .anchor = Vector4(0.0f, 0.0f, 0.0f, 0.0f),
                                .pivot = Vector2(0.5f, 0.5f), .width = _hotspotSize + 5.0f,
                                .height = _hotspotSize + 5.0f, .useInput = true});

                // Click handler
                const auto onPointerDown = [this, annotation](ElementInputEvent* event) {
                    event->stopPropagation();
                    if (_activeAnnotation == annotation) {
                        hideTooltip(annotation);
                    } else {
                        showTooltip(annotation);
                    }
                };
                resources->eventHandles.push_back(element->on("mousedown", onPointerDown));
                resources->eventHandles.push_back(element->on("touchstart", onPointerDown));

                // Hover handlers
                resources->eventHandles.push_back(element->on("mouseenter", [this, annotation]() {
                    if (_hoverAnnotation) {
                        setAnnotationHover(_hoverAnnotation, false);
                    }
                    _hoverAnnotation = annotation;
                    setAnnotationHover(annotation, true);
                }));
                resources->eventHandles.push_back(element->on("mouseleave", [this, annotation]() {
                    if (_hoverAnnotation == annotation) {
                        _hoverAnnotation = nullptr;
                        setAnnotationHover(annotation, false);
                    }
                }));
            }
            // Hidden until the first prerender has placed it
            hotspot->setEnabled(false);
            _screen->addChild(hotspot);
            resources->hotspot = hotspot;
        }

        // Listen for annotation attribute changes
        resources->eventHandles.push_back(annotation->on("label:set", [this, annotation]() {
            onLabelChange(annotation);
        }));
        resources->eventHandles.push_back(annotation->on("title:set", [this, annotation]() {
            if (_activeAnnotation == annotation) {
                layoutTooltip();
            }
        }));
        resources->eventHandles.push_back(annotation->on("text:set", [this, annotation]() {
            if (_activeAnnotation == annotation) {
                layoutTooltip();
            }
        }));
        resources->eventHandles.push_back(annotation->on("enable", [this, annotation]() {
            onAnnotationEnable(annotation);
        }));
        resources->eventHandles.push_back(annotation->on("disable", [this, annotation]() {
            onAnnotationDisable(annotation);
        }));

        _annotationResources.push_back(std::move(resources));
    }

    void AnnotationManager::unregisterAnnotation(Annotation* annotation)
    {
        const auto it = std::find_if(_annotationResources.begin(), _annotationResources.end(),
            [annotation](const auto& resources) { return resources->annotation == annotation; });
        if (it == _annotationResources.end()) {
            return;
        }
        Resources& resources = **it;

        // Clear active/hover state
        if (_activeAnnotation == annotation) {
            _activeAnnotation = nullptr;
            _tooltipShown = false;
            _fade = 0.0f;
            if (_tooltip) {
                _tooltip->entity()->setEnabled(false);
            }
        }
        if (_hoverAnnotation == annotation) {
            _hoverAnnotation = nullptr;
        }
        if (_hidePending == annotation) {
            _hidePending = nullptr;
        }

        // Unbind event handles
        for (const auto& handle : resources.eventHandles) {
            handle->off();
        }
        resources.eventHandles.clear();

        // Destroy entities. Ones already being destroyed (the annotation's entity going
        // takes its children first) belong to their parent, which frees them.
        for (Entity* node : {resources.holder, resources.hotspot}) {
            if (node && !node->destroying()) {
                node->destroy();
                (void)node->remove();
            }
        }

        // Materials and the texture last: the mesh instances that used them are gone
        _annotationResources.erase(it);
    }

    void AnnotationManager::createTooltip()
    {
        Engine* engine = entity()->engine();
        GraphicsDevice* device = engine->graphicsDevice().get();

        _screen = new Entity();
        _screen->setEngine(engine);
        _screen->setName("annotations-screen");
        auto* screen = static_cast<ScreenComponent*>(_screen->addComponent<ScreenComponent>());
        if (!screen) {
            spdlog::warn("AnnotationManager: register the Screen and Element component systems "
                         "for clickable hotspots and tooltips");
            delete _screen;
            _screen = nullptr;
            return;
        }
        screen->setScreenSpace(true);
        engine->root()->addChild(_screen);

        // The screen's teardown takes the elements with it
        _handles.push_back(_screen->on("destroy", [this]() { clearScreen(); }));

        const auto element = [engine](Entity* parent, const std::string& name, const ElementDesc& desc) {
            auto* node = new Entity();
            node->setEngine(engine);
            node->setName(name);
            auto* component = static_cast<ElementComponent*>(node->addComponent<ElementComponent>());
            component->setup(desc);
            parent->addChild(node);
            return component;
        };

        // The panel: 4 px rounded corners, as a 9-sliced 16 px texture
        constexpr int panel = 16;
        _panelTexture = createRgbaTexture(device, "annotation-tooltip", panel, panel,
            whiteShape(panel, panel, [](const float x, const float y) {
                const float r = kCornerRadius;
                const float cx = std::clamp(x, r, panel - r);
                const float cy = std::clamp(y, r, panel - r);
                return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
            }));
        _panelAtlas = std::make_shared<TextureAtlas>();
        _panelAtlas->setTexture(_panelTexture.get());
        _panelAtlas->setFrame("panel", {.rect = Vector4(0.0f, 0.0f, panel, panel), .pivot = Vector2(0.5f, 0.5f),
                                        .border = Vector4(kCornerRadius, kCornerRadius, kCornerRadius, kCornerRadius)});
        _panelSprite = std::make_shared<Sprite>(_panelAtlas, std::vector<std::string>{"panel"}, 1.0f,
                                                SpriteRenderMode::Sliced);

        // translate(25px, -50%) from the hotspot: the left edge's middle is the pivot
        _tooltip = element(_screen, "annotation-tooltip",
            {.type = ElementType::Image, .anchor = Vector4(0.0f, 0.0f, 0.0f, 0.0f), .pivot = Vector2(0.0f, 0.5f),
             .width = 2.0f * kTooltipPadding, .height = 2.0f * kTooltipPadding});
        _tooltip->setSprite(_panelSprite);
        _tooltip->setColor(Color(0.0f, 0.0f, 0.0f, 1.0f));

        // The arrow on the left edge, pointing at the hotspot
        const int arrowW = static_cast<int>(kArrowWidth);
        const int arrowH = static_cast<int>(kArrowHeight);
        _arrowTexture = createRgbaTexture(device, "annotation-tooltip-arrow", arrowW, arrowH,
            whiteShape(arrowW, arrowH, [](const float x, const float y) {
                return x >= kArrowWidth * std::abs(y - kArrowHeight * 0.5f) / (kArrowHeight * 0.5f);
            }));
        _arrow = element(_tooltip->entity(), "arrow",
            {.type = ElementType::Image, .anchor = Vector4(0.0f, 0.5f, 0.0f, 0.5f), .pivot = Vector2(1.0f, 0.5f),
             .width = kArrowWidth, .height = kArrowHeight});
        _arrow->setTexture(_arrowTexture.get());
        _arrow->setColor(Color(0.0f, 0.0f, 0.0f, 1.0f));

        const auto text = [&](const std::string& name, FontResource* font) {
            ElementComponent* t = element(_tooltip->entity(), name,
                {.type = ElementType::Text, .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f), .pivot = Vector2(0.0f, 1.0f)});
            t->setFontResource(font);
            t->setFontSize(kFontSize);
            t->setLineHeight(kLineHeight);
            t->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            t->setHorizontalAlign(ElementHorizontalAlign::Left);
            t->setVerticalAlign(1.0f);
            return t;
        };
        _title = text("title", _boldFont);
        _text = text("text", _regularFont);

        // Visibility hidden until a hotspot is clicked
        _tooltip->entity()->setEnabled(false);
    }

    void AnnotationManager::clearScreen()
    {
        _screen = nullptr;
        _tooltip = _arrow = _title = _text = nullptr;
        for (const auto& resources : _annotationResources) {
            resources->hotspot = nullptr;
        }
    }

    void AnnotationManager::initialize()
    {
        Engine* engine = entity()->engine();
        if (!engine) {
            return;
        }

        // Create layers: the base right after World's opaque sublayer, the overlay right
        // after its transparent one
        const auto& layers = engine->scene()->layers();
        const auto worldLayer = layers->getLayerByName("World");
        const auto createLayer = [&](const std::string& name, const bool semitrans) {
            auto layer = std::make_shared<Layer>(name, unusedLayerId(*layers));
            const int index = semitrans ? layers->getTransparentIndex(worldLayer) : layers->getOpaqueIndex(worldLayer);
            layers->insert(layer, index + 1);
            return layer;
        };
        _layers[0] = createLayer("HotspotBase", false);
        _layers[1] = createLayer("HotspotOverlay", true);

        // Find the camera
        if (!_camera) {
            const auto cameras = engine->root()->findComponents<CameraComponent>();
            if (!cameras.empty()) {
                _camera = cameras.front()->entity();
            }
        }

        // Add layers to the camera
        if (_camera) {
            auto* camera = _camera->findComponent<CameraComponent>();
            std::vector<int> ids = camera->layers();
            // An empty list renders every layer; spell out upstream's default camera
            // layers before adding to it, or the camera would render only these two
            if (ids.empty()) {
                ids = {LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX, LAYERID_UI, LAYERID_IMMEDIATE};
            }
            ids.push_back(_layers[0]->id());
            ids.push_back(_layers[1]->id());
            camera->setLayers(ids);
            _handles.push_back(_camera->on("destroy", [this]() { _camera = nullptr; }));
        }

        createTooltip();

        // A press that no hotspot took dismisses the active tooltip (upstream's single
        // document-level pointerdown listener)
        const auto onDocumentPointerDown = [this]() {
            if (_activeAnnotation) {
                hideTooltip(_activeAnnotation);
            }
        };
        if (Mouse* mouse = engine->mouse()) {
            _handles.push_back(mouse->on("mousedown", onDocumentPointerDown));
        }
        if (TouchDevice* touch = engine->touch()) {
            _handles.push_back(touch->on("touchstart", onDocumentPointerDown));
        }

        // Listen for annotation add/remove events on the engine
        _handles.push_back(engine->on("annotation:add", [this](Annotation* annotation) {
            registerAnnotation(annotation);
        }));
        _handles.push_back(engine->on("annotation:remove", [this](Annotation* annotation) {
            unregisterAnnotation(annotation);
        }));

        // Prerender handler - update all annotations
        _handles.push_back(engine->on("prerender", [this]() { prerender(); }));

        // Clean up on destroy
        once("destroy", [this]() { teardown(); });
    }

    void AnnotationManager::prerender()
    {
        if (!_camera) {
            return;
        }
        auto* cameraComponent = _camera->findComponent<CameraComponent>();
        if (!cameraComponent || !cameraComponent->camera()) {
            return;
        }

        const auto [canvasWidth, canvasHeight] = entity()->engine()->canvasSize();
        const auto screenHeight = static_cast<float>(canvasHeight);
        const Matrix4 viewMatrix = _camera->worldTransform().inverse();
        const Matrix4& projMatrix = cameraComponent->camera()->projectionMatrix();
        const Quaternion cameraRotation = _camera->rotation();

        for (const auto& resources : _annotationResources) {
            Annotation* annotation = resources->annotation;
            if (!annotation->enabled()) {
                continue;
            }

            const Vector3 position = annotation->entity()->position();
            const Vector3 view = viewMatrix.transformPoint(position);
            if (view.getZ() >= 0.0f) {
                hideAnnotationElements(annotation, *resources);
                continue;
            }

            // Screen position, in canvas points from the top-left as upstream's
            // worldToScreen, then from the bottom-left where the screen's elements sit
            const Vector4 clip = projMatrix * Vector4(view.getX(), view.getY(), view.getZ(), 1.0f);
            const float screenX = (clip.getX() / clip.getW() * 0.5f + 0.5f) * static_cast<float>(canvasWidth);
            const float screenY = (1.0f - (clip.getY() / clip.getW() * 0.5f + 0.5f)) * screenHeight;
            if (resources->hotspot) {
                resources->hotspot->setEnabled(true);
                resources->hotspot->setLocalPosition(screenX, screenHeight - screenY, 0.0f);
            }
            if (_activeAnnotation == annotation && _tooltip) {
                _tooltip->entity()->setLocalPosition(screenX + kTooltipOffsetX, screenHeight - screenY, 0.0f);
            }

            // Face the camera, sized from the view-space depth (not the Euclidean distance)
            // to match the projection matrix
            resources->holder->setRotation(cameraRotation);
            resources->holder->rotateLocal(90.0f, 0.0f, 0.0f);
            const float viewDepth = -view.getZ();
            const float worldSize = _hotspotSize / std::max(screenHeight, 1.0f) *
                (2.0f * viewDepth / projMatrix.getElement(1, 1));
            resources->holder->setLocalScale(worldSize, worldSize, worldSize);

            // Update material opacity
            const float opacities[2] = {_opacity, _behindOpacity * _opacity};
            for (int i = 0; i < 2; ++i) {
                if (resources->appliedOpacity[i] != opacities[i]) {
                    resources->appliedOpacity[i] = opacities[i];
                    resources->materials[i]->setOpacity(opacities[i]);
                }
            }
        }
    }

    void AnnotationManager::update(const float dt)
    {
        // The tooltip's `transition: opacity 0.2s ease-in-out`
        const float step = dt / kFadeSeconds;
        _fade = std::clamp(_tooltipShown ? _fade + step : _fade - step, 0.0f, 1.0f);
        if (_tooltip && _fade != _appliedFade) {
            _appliedFade = _fade;
            const float eased = _fade * _fade * (3.0f - 2.0f * _fade);
            _tooltip->setOpacity(kTooltipAlpha * eased);
            _arrow->setOpacity(kTooltipAlpha * eased);
            _title->setOpacity(eased);
            _text->setOpacity(eased);
        }

        if (_hideTimer > 0.0f) {
            _hideTimer -= dt;
            if (_hideTimer <= 0.0f) {
                _hideTimer = 0.0f;
                if (_tooltip && !_activeAnnotation) {
                    _tooltip->entity()->setEnabled(false);
                }
                if (Annotation* annotation = std::exchange(_hidePending, nullptr)) {
                    annotation->fire("hide");
                }
            }
        }
    }

    void AnnotationManager::teardown()
    {
        if (_tornDown) {
            return;
        }
        _tornDown = true;

        // Unregister all annotations
        while (!_annotationResources.empty()) {
            unregisterAnnotation(_annotationResources.back()->annotation);
        }

        // Remove event listeners
        for (const auto& handle : _handles) {
            handle->off();
        }
        _handles.clear();

        // Remove the screen with the tooltip
        if (_screen && !_screen->destroying()) {
            Entity* screen = _screen;
            clearScreen();
            screen->destroy();
            (void)screen->remove();
        }
        clearScreen();

        Engine* engine = entity() ? entity()->engine() : nullptr;

        // Remove layers from the camera
        if (_camera) {
            if (auto* camera = _camera->findComponent<CameraComponent>()) {
                std::vector<int> ids = camera->layers();
                std::erase_if(ids, [this](const int id) {
                    return (_layers[0] && id == _layers[0]->id()) || (_layers[1] && id == _layers[1]->id());
                });
                camera->setLayers(ids);
            }
        }

        // Remove layers from the scene
        if (engine && engine->scene() && engine->scene()->layers()) {
            for (const auto& layer : _layers) {
                if (layer) {
                    engine->scene()->layers()->remove(layer);
                }
            }
        }
        _layers[0].reset();
        _layers[1].reset();
    }
}
