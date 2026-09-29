// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#include "elementInput.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <optional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include "framework/components/button/buttonComponent.h"
#include "framework/components/componentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/textLayout.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/sprite.h"
#include "scene/textureAtlas.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    namespace
    {
        /// A triangle list in the 14-float layout every UI visual uses: position(3)
        /// normal(3) uv0(2) tangent(4) uv1(2).
        std::shared_ptr<Mesh> makeUiMesh(const std::shared_ptr<GraphicsDevice>& gd, const std::vector<float>& vertices,
                                         const std::vector<uint32_t>& indices, const BoundingBox& bounds)
        {
            if (!gd || vertices.empty() || indices.empty()) {
                return nullptr;
            }
            const int vertexCount = static_cast<int>(vertices.size() / 14u);
            std::vector<uint8_t> vbData(vertices.size() * sizeof(float));
            std::memcpy(vbData.data(), vertices.data(), vbData.size());
            VertexBufferOptions vbOpts;
            vbOpts.data = std::move(vbData);
            auto vertexFormat = std::make_shared<VertexFormat>(
                14 * static_cast<int>(sizeof(float)), VertexFormat::standardElements(), true, false);
            auto vb = gd->createVertexBuffer(vertexFormat, vertexCount, vbOpts);

            std::vector<uint8_t> ibData(indices.size() * sizeof(uint32_t));
            std::memcpy(ibData.data(), indices.data(), ibData.size());
            auto ib = gd->createIndexBuffer(INDEXFORMAT_UINT32, static_cast<int>(indices.size()), ibData);

            auto mesh = std::make_shared<Mesh>();
            mesh->setVertexBuffer(vb);
            mesh->setIndexBuffer(ib, 0);
            Primitive prim;
            prim.type = PRIMITIVE_TRIANGLES;
            prim.base = 0;
            prim.count = static_cast<int>(indices.size());
            prim.indexed = true;
            mesh->setPrimitive(prim, 0);
            mesh->setAabb(bounds);
            return mesh;
        }

        /// `#rrggbb` or `#rrggbbaa`, sRGB (upstream Color.fromString); null otherwise.
        std::optional<Color> parseHexColor(const std::string& text)
        {
            if ((text.size() != 7 && text.size() != 9) || text[0] != '#') {
                return std::nullopt;
            }
            for (size_t i = 1; i < text.size(); ++i) {
                if (!std::isxdigit(static_cast<unsigned char>(text[i]))) {
                    return std::nullopt;
                }
            }
            const auto channel = [&](const size_t at) {
                return static_cast<float>(std::stoi(text.substr(at, 2), nullptr, 16)) / 255.0f;
            };
            return Color(channel(1), channel(3), channel(5), text.size() == 9 ? channel(7) : 1.0f);
        }

        /// A tag attribute as a number (upstream Number()), null when absent or not one.
        std::optional<float> parseNumber(const MarkupTag& tag, const char* key)
        {
            const auto it = tag.attributes.find(key);
            if (it == tag.attributes.end()) {
                return std::nullopt;
            }
            char* end = nullptr;
            const float value = std::strtof(it->second.c_str(), &end);
            if (end == it->second.c_str() || *end != '\0' || !std::isfinite(value)) {
                return std::nullopt;
            }
            return value;
        }

        /// Upstream text-element's per-symbol colour, outline and shadow for one symbol's
        /// tags, falling back to the element's own values where a tag leaves one out.
        ElementInput::TextStyle resolveTextStyle(const ElementComponent* element, const MarkupTags& tags)
        {
            ElementInput::TextStyle style{element->color(), element->outlineColor(), element->outlineThickness(),
                                          element->shadowColor(), element->shadowOffset()};
            if (const auto it = tags.find("color"); it != tags.end() && it->second.value) {
                // Upstream accepts only #rrggbb here; the element's opacity stays the alpha.
                if (const auto c = parseHexColor(*it->second.value); c && it->second.value->size() == 7) {
                    style.color = Color(c->r, c->g, c->b, element->color().a);
                }
            }
            if (const auto it = tags.find("outline"); it != tags.end() &&
                (it->second.attributes.count("color") || it->second.attributes.count("thickness"))) {
                if (const auto c = it->second.attributes.count("color")
                        ? parseHexColor(it->second.attributes.at("color")) : std::nullopt) {
                    style.outlineColor = *c;
                }
                style.outlineThickness = parseNumber(it->second, "thickness").value_or(element->outlineThickness());
            }
            if (const auto it = tags.find("shadow"); it != tags.end() &&
                (it->second.attributes.count("color") || it->second.attributes.count("offset") ||
                 it->second.attributes.count("offsetX") || it->second.attributes.count("offsetY"))) {
                if (const auto c = it->second.attributes.count("color")
                        ? parseHexColor(it->second.attributes.at("color")) : std::nullopt) {
                    style.shadowColor = *c;
                }
                const auto offset = parseNumber(it->second, "offset");
                style.shadowOffset = Vector2(
                    parseNumber(it->second, "offsetX").value_or(offset.value_or(element->shadowOffset().x)),
                    parseNumber(it->second, "offsetY").value_or(offset.value_or(element->shadowOffset().y)));
            }
            return style;
        }

        std::string styleKey(const ElementInput::TextStyle& s)
        {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%g %g %g|%g %g %g %g %g|%g %g %g %g %g %g",
                s.color.r, s.color.g, s.color.b,
                s.outlineColor.r, s.outlineColor.g, s.outlineColor.b, s.outlineColor.a, s.outlineThickness,
                s.shadowColor.r, s.shadowColor.g, s.shadowColor.b, s.shadowColor.a, s.shadowOffset.x, s.shadowOffset.y);
            return buffer;
        }

        struct TextMeshes
        {
            /// (page, style) -> mesh, in key order.
            std::vector<std::pair<std::pair<int, int>, std::shared_ptr<Mesh>>> meshes;
            /// Style 0 is the element's own, read live each frame; the rest come from tags.
            std::vector<ElementInput::TextStyle> styles;
        };

        /// The text's geometry, one mesh per (atlas page, style) run: each page is its own
        /// texture, and each markup style its own material. DEVIATION: upstream draws a page
        /// in ONE mesh with the style in vertex attributes; here glyphs of different styles
        /// are separate draws, which can order two overlapping neighbours' outlines or
        /// shadows differently from upstream's glyph order.
        TextMeshes buildTextMeshes(const std::shared_ptr<GraphicsDevice>& gd, const ElementComponent* element)
        {
            TextMeshes result;
            const FontResource* font = element ? element->fontResource() : nullptr;
            if (!gd || !font || element->textSymbols().empty()) {
                return result;
            }
            const std::string& symbols = element->textSymbols();
            const auto& tags = element->markupTags();

            const TextMeasure measure = measureText(*font, symbols, static_cast<float>(element->fontSize()),
                                                    element->lineHeight(), element->textMaxLineWidth(),
                                                    element->spacing());
            const float horizontal = element->horizontalAlign() == ElementHorizontalAlign::Left ? 0.0f
                : element->horizontalAlign() == ElementHorizontalAlign::Right ? 1.0f : 0.5f;
            const std::vector<PlacedGlyph> glyphs = placeText(*font, symbols, measure, element->calculatedWidth(),
                element->calculatedHeight(), element->pivot(), horizontal, element->verticalAlign());

            // Upstream's palettes: index 0 the element's own style, one more per distinct
            // tag style.
            std::vector<int> styleOf(symbols.size(), 0);
            result.styles.push_back({element->color(), element->outlineColor(), element->outlineThickness(),
                                     element->shadowColor(), element->shadowOffset()});
            std::map<std::string, int> styleIndex{{styleKey(result.styles[0]), 0}};
            for (size_t i = 0; i < tags.size() && i < symbols.size(); ++i) {
                if (!tags[i]) {
                    continue;
                }
                ElementInput::TextStyle style = resolveTextStyle(element, *tags[i]);
                const auto [it, inserted] = styleIndex.emplace(styleKey(style), static_cast<int>(result.styles.size()));
                if (inserted) {
                    result.styles.push_back(style);
                }
                styleOf[i] = it->second;
            }

            struct Buffers
            {
                std::vector<float> vertices;
                std::vector<uint32_t> indices;
                uint32_t base = 0;
            };
            std::map<std::pair<int, int>, Buffers> runs;
            for (const PlacedGlyph& g : glyphs) {
                Buffers& b = runs[{g.page, styleOf[g.symbol]}];
                // position(3) normal(3) uv0(2) tangent(4) uv1(2)
                const std::array<float, 56> quad = {
                    g.x0, g.y1, 0.0f,   0.0f, 0.0f, 1.0f,   g.u0, g.v0,   1.0f, 0.0f, 0.0f, 1.0f,   g.u0, g.v0,
                    g.x1, g.y1, 0.0f,   0.0f, 0.0f, 1.0f,   g.u1, g.v0,   1.0f, 0.0f, 0.0f, 1.0f,   g.u1, g.v0,
                    g.x1, g.y0, 0.0f,   0.0f, 0.0f, 1.0f,   g.u1, g.v1,   1.0f, 0.0f, 0.0f, 1.0f,   g.u1, g.v1,
                    g.x0, g.y0, 0.0f,   0.0f, 0.0f, 1.0f,   g.u0, g.v1,   1.0f, 0.0f, 0.0f, 1.0f,   g.u0, g.v1
                };
                b.vertices.insert(b.vertices.end(), quad.begin(), quad.end());
                b.indices.insert(b.indices.end(), {b.base, b.base + 2u, b.base + 1u, b.base, b.base + 3u, b.base + 2u});
                b.base += 4u;
            }

            const float boxW = element->calculatedWidth();
            const float boxH = element->calculatedHeight();
            BoundingBox bounds;
            bounds.setCenter(Vector3(0.0f, 0.0f, 0.0f));
            bounds.setHalfExtents(Vector3(std::max(boxW * 0.5f, 1.0f), std::max(boxH * 0.5f, 1.0f), 1.0f));
            for (const auto& [key, b] : runs) {
                if (auto mesh = makeUiMesh(gd, b.vertices, b.indices, bounds)) {
                    result.meshes.emplace_back(key, std::move(mesh));
                }
            }
            return result;
        }

        std::shared_ptr<Mesh> buildImageMesh(const std::shared_ptr<GraphicsDevice>& gd, const ElementComponent* element,
                                             Texture*& outTexture)
        {
            outTexture = nullptr;
            const Vector2 pivot = element->pivot();
            const float w = element->calculatedWidth();
            const float h = element->calculatedHeight();

            ImageGeometry geometry;
            const Sprite* sprite = element->sprite().get();
            const TextureAtlasFrame* frame = sprite ? sprite->frame(element->spriteFrame()) : nullptr;
            Texture* atlasTexture = sprite && sprite->atlas() ? sprite->atlas()->texture() : nullptr;
            if (frame && atlasTexture && frame->rect.getZ() > 0.0f && frame->rect.getW() > 0.0f) {
                const float texW = static_cast<float>(atlasTexture->width());
                const float texH = static_cast<float>(atlasTexture->height());
                const Vector2 size = fitImageSize(w, h, frame->rect.getZ() / frame->rect.getW(), element->fitMode());
                if (sprite->nineSliced()) {
                    const float ppu = element->pixelsPerUnit().value_or(sprite->pixelsPerUnit());
                    geometry = buildSlicedImageGeometry(size, pivot, *frame, texW, texH, ppu);
                } else {
                    const Vector4 uvRect(frame->rect.getX() / texW, frame->rect.getY() / texH,
                                         frame->rect.getZ() / texW, frame->rect.getW() / texH);
                    geometry = buildSimpleImageGeometry(size, pivot, uvRect);
                }
                outTexture = atlasTexture;
            } else {
                Texture* texture = element->texture();
                const float aspect = texture && texture->height() > 0
                    ? static_cast<float>(texture->width()) / static_cast<float>(texture->height()) : -1.0f;
                const Vector2 size = fitImageSize(w, h, aspect, element->fitMode());
                geometry = buildSimpleImageGeometry(size, pivot, element->rect());
                outTexture = texture;
            }

            std::vector<float> vertices;
            vertices.reserve(geometry.vertices.size() * 14u);
            float minX = geometry.vertices.empty() ? 0.0f : geometry.vertices[0].x;
            float maxX = minX;
            float minY = geometry.vertices.empty() ? 0.0f : geometry.vertices[0].y;
            float maxY = minY;
            for (const ImageVertex& v : geometry.vertices) {
                // position(3) normal(3) uv0(2) tangent(4) uv1(2)
                vertices.insert(vertices.end(), {v.x, v.y, 0.0f, 0.0f, 0.0f, 1.0f, v.u, v.v,
                                                 1.0f, 0.0f, 0.0f, 1.0f, v.u, v.v});
                minX = std::min(minX, v.x);
                maxX = std::max(maxX, v.x);
                minY = std::min(minY, v.y);
                maxY = std::max(maxY, v.y);
            }
            BoundingBox bounds;
            bounds.setCenter(Vector3((minX + maxX) * 0.5f, (minY + maxY) * 0.5f, 0.0f));
            bounds.setHalfExtents(Vector3((maxX - minX) * 0.5f, (maxY - minY) * 0.5f, 0.001f));
            return makeUiMesh(gd, vertices, geometry.indices, bounds);
        }
    }

    void ElementInput::releaseVisual(ElementVisual& visual)
    {
        if (visual.destroyHandle) {
            visual.destroyHandle->off();   // it captures the map entry about to go
            visual.destroyHandle.reset();
        }
        if (visual.entity) {
            auto removed = visual.entity->remove();
            removed.reset();
        }
        visual.entity = nullptr;
        visual.render = nullptr;
        visual.parts.clear();
    }

    void ElementInput::detach()
    {
        for (auto& [_, visual] : _visuals) {
            releaseVisual(visual);
        }
        _visuals.clear();
        _engine.reset();
    }

    bool ElementInput::computeElementRect(const ElementComponent* element, SDL_FRect& outRect) const
    {
        if (!element || !element->entity() || !element->enabled() || !element->entity()->enabled()) {
            return false;
        }

        // Upstream ElementInput hit-tests a screen-space element by its CANVAS corners,
        // which are in window points with y down — the space mouse coordinates are in.
        // An element on no screen, or a world-space one, is not hit-testable here.
        const ScreenComponent* screen = element->screenComponent();
        if (!screen || !screen->screenSpace()) {
            return false;
        }
        const auto& corners = const_cast<ElementComponent*>(element)->canvasCorners();
        float minX = corners[0].x;
        float maxX = corners[0].x;
        float minY = corners[0].y;
        float maxY = corners[0].y;
        for (const auto& c : corners) {
            minX = std::min(minX, c.x);
            maxX = std::max(maxX, c.x);
            minY = std::min(minY, c.y);
            maxY = std::max(maxY, c.y);
        }
        outRect.x = minX;
        outRect.y = minY;
        outRect.w = maxX - minX;
        outRect.h = maxY - minY;
        return outRect.w > 0.0f && outRect.h > 0.0f;
    }

    bool ElementInput::handleMouseButtonDown(const float x, const float y)
    {
        // Front-most element wins.
        const auto& elements = ElementComponent::instances();
        for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
            auto* element = *it;
            if (!element || !element->useInput() || !element->entity()) {
                continue;
            }

            SDL_FRect rect{};
            if (!computeElementRect(element, rect)) {
                continue;
            }

            if (x < rect.x || y < rect.y || x > rect.x + rect.w || y > rect.y + rect.h) {
                continue;
            }

            // element receives click first, then button behavior.
            element->fire("click", x, y);
            if (auto* button = element->entity()->findComponent<ButtonComponent>()) {
                button->fire("click", x, y);
            }
            return true;
        }
        return false;
    }

    namespace
    {
        /// Upstream's element materials are EMISSIVE-only: black diffuse, the element
        /// colour as the emissive (times an image's texture), alpha from the texture or,
        /// for MSDF text, from the distance field. DEVIATION: a bitmap font's or an
        /// image's alpha comes through the diffuse map, where upstream reads an opacity
        /// map, which is Metal-only here; setting the same texture as the opacity map as
        /// well would multiply it in twice on Metal.
        std::shared_ptr<StandardMaterial> makeElementMaterial(const bool worldSpace)
        {
            auto material = std::make_shared<StandardMaterial>();
            material->setUseLighting(false);
            material->setUseSkybox(false);
            material->setTransparent(true);
            material->setCullMode(CullMode::CULLFACE_NONE);
            material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
            material->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
            material->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
            auto depth = std::make_shared<DepthState>(DepthState::noWrite());
            // A screen-space element is an overlay and must never be occluded; a
            // world-space one is part of the scene, so geometry in front of it hides it.
            // Depth WRITES stay off either way: it is transparent and must not punch holes
            // in the depth buffer.
            depth->setDepthTest(worldSpace);
            material->setDepthState(depth);
            return material;
        }

        /// Upstream text-element's shadow_offset, verbatim. It has TWO conventions:
        /// - the uniform one, for text without markup tags: 0.005 of the page per unit, the
        ///   y term scaled by -width/height. It is NOT sign-flipped for this engine's v-down
        ///   glyph UVs, although upstream writes its glyph UVs v-up: measured on upstream's
        ///   own ui-text thumbnail, a (0.25, -0.25) shadow sits right of and BELOW the
        ///   glyphs (rim below 23 : above 6), and the flipped value drew it above;
        /// - the per-vertex one (msdf.js unpackMsdfParams), which upstream uses for EVERY
        ///   symbol of a text with tags, the element's own included: 0.005 per unit on both
        ///   axes, with no aspect and no minus sign. For a square page the two point y in
        ///   opposite directions; that is upstream's behaviour, reproduced.
        Vector2 msdfShadowUvOffset(const Vector2& offset, const Texture* page, const bool perVertexConvention)
        {
            if (perVertexConvention) {
                return Vector2(0.005f * offset.x, 0.005f * offset.y);
            }
            const float aspect = page && page->height() > 0
                ? static_cast<float>(page->width()) / static_cast<float>(page->height()) : 1.0f;
            return Vector2(0.005f * offset.x, -aspect * 0.005f * offset.y);
        }
    }

    ElementInput::ElementVisual& ElementInput::visualFor(ElementComponent* element)
    {
        auto& visual = _visuals[element];
        if (visual.entity && visual.type != element->type()) {
            // The element changed type: its visual is built for the other kind.
            releaseVisual(visual);
            visual = ElementVisual{};
        }
        if (visual.entity) {
            return visual;
        }

        visual.type = element->type();
        // A screen-space element is drawn in clip space by whatever camera renders its
        // layer (MeshInstance::setScreenSpace); anything else — an element on no screen or
        // on a world-space one — is geometry in the world.
        const ScreenComponent* screen = element->screenComponent();
        visual.worldSpace = !(screen && screen->screenSpace());
        visual.entity = new Entity();
        visual.entity->setEngine(_engine.get());
        visual.entity->setLocalPosition(0.0f, 0.0f, 0.0f);
        // The visual lives under its element, so destroying the element frees it: forget
        // it then, rather than touching freed memory when the element leaves the list.
        visual.destroyHandle = visual.entity->on("destroy", [&visual]() {
            visual.entity = nullptr;
            visual.render = nullptr;
            for (auto& part : visual.parts) {
                part.meshInstance = nullptr;
            }
        });
        visual.render = static_cast<RenderComponent*>(visual.entity->addComponent<RenderComponent>());
        if (!visual.render) {
            static bool warned = false;
            if (!warned) {
                spdlog::warn("ElementInput: no RenderComponentSystem is registered, so UI elements cannot be drawn");
                warned = true;
            }
        }
        // A child of its element with an identity transform: the element's transform — the
        // layout's, for an element on a screen — places it.
        element->entity()->addChild(visual.entity);
        return visual;
    }

    void ElementInput::syncElements()
    {
        if (!_engine || !_engine->graphicsDevice()) {
            return;
        }

        for (auto& [_, visual] : _visuals) {
            visual.activeFrame = false;
        }

        for (auto* element : ElementComponent::instances()) {
            if (!element || !element->entity()) {
                continue;
            }
            const bool isText = element->type() == ElementType::Text && element->fontResource() &&
                element->fontResource()->texture && !element->text().empty();
            const bool isImage = element->type() == ElementType::Image;
            if (!isText && !isImage) {
                continue;
            }

            auto& visual = visualFor(element);
            visual.activeFrame = true;
            if (!visual.entity) {
                continue;
            }

            // The element's own layers, or the element system's choice.
            std::vector<int> layers = element->layers();
            if (layers.empty()) {
                layers = {visual.worldSpace ? LAYERID_WORLD : LAYERID_UI};
            }
            if (visual.render && visual.layers != layers) {
                visual.render->setLayers(layers);
                visual.layers = std::move(layers);
            }

            const bool sizeChanged =
                std::abs(visual.cachedPivot.x - element->pivot().x) > 1e-4f ||
                std::abs(visual.cachedPivot.y - element->pivot().y) > 1e-4f ||
                std::abs(visual.cachedWidth - element->calculatedWidth()) > 1e-4f ||
                std::abs(visual.cachedHeight - element->calculatedHeight()) > 1e-4f;

            bool rebuild = sizeChanged || visual.parts.empty();
            if (isText) {
                // Every text input marks the element dirty (and re-measures it).
                rebuild = rebuild || element->textDirty();
            } else {
                const Sprite* sprite = element->sprite().get();
                const uint64_t atlasVersion = sprite && sprite->atlas() ? sprite->atlas()->version() : 0;
                rebuild = rebuild || visual.cachedImageVersion != element->imageVersion() ||
                    visual.cachedSprite != sprite ||
                    visual.cachedSpriteVersion != (sprite ? sprite->version() : 0) ||
                    visual.cachedAtlasVersion != atlasVersion;
            }

            if (rebuild) {
                if (visual.render) {
                    visual.render->clearMeshInstances();
                }
                visual.parts.clear();
                if (isText) {
                    const FontResource* font = element->fontResource();
                    TextMeshes text = buildTextMeshes(_engine->graphicsDevice(), element);
                    for (auto& [key, mesh] : text.meshes) {
                        const auto [page, style] = key;
                        VisualPart part;
                        part.mesh = std::move(mesh);
                        part.style = style;
                        part.texture = page < static_cast<int>(font->pages.size())
                            ? font->pages[static_cast<size_t>(page)] : font->texture;
                        part.material = makeElementMaterial(visual.worldSpace);
                        if (font->msdf) {
                            part.material->setMsdfMap(part.texture);
                            part.material->setMsdfFont(font->pxRange, font->intensity);
                        } else {
                            part.material->setDiffuseMap(part.texture);   // coverage in alpha
                        }
                        visual.parts.push_back(std::move(part));
                    }
                    visual.styles = std::move(text.styles);
                    visual.markupStyles = !element->markupTags().empty();
                    element->clearTextDirty();
                } else {
                    VisualPart part;
                    part.mesh = buildImageMesh(_engine->graphicsDevice(), element, part.texture);
                    part.material = makeElementMaterial(visual.worldSpace);
                    // An image multiplies its texture into the colour and takes its alpha.
                    part.material->setDiffuseMap(part.texture);
                    part.material->setEmissiveMap(part.texture);
                    if (part.mesh) {
                        visual.parts.push_back(std::move(part));
                    }
                    const Sprite* sprite = element->sprite().get();
                    visual.cachedImageVersion = element->imageVersion();
                    visual.cachedSprite = sprite;
                    visual.cachedSpriteVersion = sprite ? sprite->version() : 0;
                    visual.cachedAtlasVersion = sprite && sprite->atlas() ? sprite->atlas()->version() : 0;
                }
                visual.cachedWidth = element->calculatedWidth();
                visual.cachedHeight = element->calculatedHeight();
                visual.cachedPivot = element->pivot();

                if (visual.render) {
                    for (auto& part : visual.parts) {
                        auto meshInstance = std::make_unique<MeshInstance>(part.mesh.get(), part.material.get(), visual.entity);
                        meshInstance->setScreenSpace(!visual.worldSpace);
                        part.meshInstance = meshInstance.get();
                        visual.render->addMeshInstance(std::move(meshInstance));
                    }
                }
            }

            for (auto& part : visual.parts) {
                const bool msdf = isText && element->fontResource()->msdf;
                // Style 0 is the element's own, read live (a pulsing colour or a changed
                // outline needs no rebuild); a markup style is what its tags resolved to.
                const TextStyle style = (!isText || part.style == 0 || part.style >= static_cast<int>(visual.styles.size()))
                    ? TextStyle{element->color(), element->outlineColor(), element->outlineThickness(),
                                element->shadowColor(), element->shadowOffset()}
                    : visual.styles[static_cast<size_t>(part.style)];
                if (!part.styled || !(part.color == style.color) || part.opacity != element->opacity()) {
                    part.material->setEmissive(style.color);
                    part.material->setOpacity(element->opacity());
                    part.color = style.color;
                    part.opacity = element->opacity();
                }
                if (msdf && (!part.styled || !(part.outlineColor == style.outlineColor) ||
                             part.outlineThickness != style.outlineThickness ||
                             !(part.shadowColor == style.shadowColor) ||
                             part.shadowOffset.x != style.shadowOffset.x ||
                             part.shadowOffset.y != style.shadowOffset.y)) {
                    // Upstream's editor units: thickness x 0.2, offset x 0.005 of the page.
                    part.material->setMsdfOutline(style.outlineColor, 0.2f * style.outlineThickness);
                    part.material->setMsdfShadow(style.shadowColor,
                        msdfShadowUvOffset(style.shadowOffset, part.texture, visual.markupStyles));
                    part.outlineColor = style.outlineColor;
                    part.outlineThickness = style.outlineThickness;
                    part.shadowColor = style.shadowColor;
                    part.shadowOffset = style.shadowOffset;
                }
                part.styled = true;
                if (part.meshInstance) {
                    part.meshInstance->setDrawOrder(element->drawOrder());
                }
            }
            visual.entity->setEnabled(element->enabled() && element->entity()->enabled());
        }

        std::vector<ElementComponent*> toRemove;
        for (auto& [element, visual] : _visuals) {
            if (!visual.activeFrame) {
                releaseVisual(visual);
                toRemove.push_back(element);
            }
        }
        for (auto* element : toRemove) {
            _visuals.erase(element);
        }
    }
}
