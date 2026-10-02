// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025
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
#include <functional>
#include <map>
#include <unordered_map>
#include <sstream>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/hash.h"
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
        /// `#rrggbb` or `#rrggbbaa`, sRGB; null otherwise.
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

        /// A tag attribute as a number, null when absent or not one.
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

        /// The per-symbol colour, outline and shadow for one symbol's
        /// tags, falling back to the element's own values where a tag leaves one out.
        ElementInput::TextStyle resolveTextStyle(const ElementComponent* element, const MarkupTags& tags)
        {
            ElementInput::TextStyle style{element->color(), element->outlineColor(), element->outlineThickness(),
                                          element->shadowColor(), element->shadowOffset()};
            if (const auto it = tags.find("color"); it != tags.end() && it->second.value) {
                // Only #rrggbb is accepted here; the element's opacity stays the alpha.
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

        struct TextRuns
        {
            struct Run
            {
                std::pair<int, int> key;   // (page, style)
                /// 14 floats a vertex, four vertices a glyph; indices relative to the run.
                std::vector<float> vertices;
                std::vector<uint32_t> indices;
                /// The symbol (code point index) of each quad, ascending: what a draw range
                /// narrows the index range by.
                std::vector<uint32_t> quadSymbols;
            };
            /// One per (page, style), in key order.
            std::vector<Run> runs;
            /// Style 0 is the element's own, read live each frame; the rest come from tags.
            std::vector<ElementInput::TextStyle> styles;
            BoundingBox bounds;
        };

        /// The text's geometry, one run per (atlas page, style): each page is its own
        /// texture, and each markup style its own material. DEVIATION: upstream draws a page
        /// in ONE mesh with the style in vertex attributes; here glyphs of different styles
        /// are separate draws, which can order two overlapping neighbours' outlines or
        /// shadows differently from upstream's glyph order.
        TextRuns buildTextRuns(const ElementComponent* element)
        {
            TextRuns result;
            const FontResource* font = element ? element->fontResource() : nullptr;
            if (!font || element->textCodePoints().empty()) {
                return result;
            }
            const std::u32string& symbols = element->textCodePoints();
            const auto& tags = element->markupTags();

            const TextMeasure measure = element->measureLayout();
            const float horizontal = element->horizontalAlign() == ElementHorizontalAlign::Left ? 0.0f
                : element->horizontalAlign() == ElementHorizontalAlign::Right ? 1.0f : 0.5f;
            const std::vector<PlacedGlyph> glyphs = placeText(*font, symbols, measure, element->calculatedWidth(),
                element->calculatedHeight(), element->pivot(), horizontal, element->verticalAlign(), element->justify());

            // The palettes: index 0 the element's own style, one more per distinct
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
                std::vector<uint32_t> symbols;
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
                b.symbols.push_back(static_cast<uint32_t>(g.symbol));
            }

            const float boxW = element->calculatedWidth();
            const float boxH = element->calculatedHeight();
            result.bounds.setCenter(Vector3(0.0f, 0.0f, 0.0f));
            result.bounds.setHalfExtents(Vector3(std::max(boxW * 0.5f, 1.0f), std::max(boxH * 0.5f, 1.0f), 1.0f));
            for (auto& [key, b] : runs) {
                if (!b.vertices.empty() && !b.indices.empty()) {
                    result.runs.push_back({key, std::move(b.vertices), std::move(b.indices), std::move(b.symbols)});
                }
            }
            return result;
        }

        struct ImageMeshData
        {
            std::vector<float> vertices;
            std::vector<uint32_t> indices;
            BoundingBox bounds;
            /// The texture the geometry's UVs address: the sprite's atlas, or the element's.
            Texture* texture = nullptr;
        };

        ImageMeshData buildImageMeshData(const ElementComponent* element)
        {
            ImageMeshData result;
            Texture*& outTexture = result.texture;
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
                    geometry = sprite->renderMode() == SpriteRenderMode::Tiled
                        ? buildTiledImageGeometry(size, pivot, *frame, texW, texH, ppu)
                        : buildSlicedImageGeometry(size, pivot, *frame, texW, texH, ppu);
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

            std::vector<float>& vertices = result.vertices;
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
            result.bounds.setCenter(Vector3((minX + maxX) * 0.5f, (minY + maxY) * 0.5f, 0.0f));
            result.bounds.setHalfExtents(Vector3((maxX - minX) * 0.5f, (maxY - minY) * 0.5f, 0.001f));
            result.indices = std::move(geometry.indices);
            return result;
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
        // The records the elements hold point into the map just cleared. Through the live
        // list, not the map's keys: a key may be an element that is already gone.
        for (auto* element : ElementComponent::instances()) {
            if (element && element->drawRecord(this)) {
                element->setDrawRecord(this, nullptr);
            }
        }
        _materials.clear();
        _geometry.reset();
        _masksApplied = false;
        for (auto& [_, handle] : _watched) {
            handle->off();
        }
        _watched.clear();
        _hoveredElement = nullptr;
        _pressedElement = nullptr;
        _touchedElements.clear();
        _touchLeaveFired.clear();
        _clickedElements.clear();
        _engine.reset();
    }

    namespace
    {
        /// Element materials are EMISSIVE-only: black diffuse, the element
        /// colour as the emissive (times an image's texture), alpha from the texture's
        /// alpha through the opacity map or, for MSDF text, from the distance field.
        std::shared_ptr<StandardMaterial> makeBaseElementMaterial(const bool worldSpace)
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

        /// The text shadow's UV offset. It has TWO conventions:
        /// - the uniform one, for text without markup tags: 0.005 of the page per unit, the
        ///   y term scaled by -width/height. It is NOT sign-flipped for this engine's v-down
        ///   glyph UVs, although upstream writes its glyph UVs v-up: measured on upstream's
        ///   own ui-text thumbnail, a (0.25, -0.25) shadow sits right of and BELOW the
        ///   glyphs (rim below 23 : above 6), and the flipped value drew it above;
        /// - the per-vertex one, which is used for EVERY
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

    std::shared_ptr<StencilParameters> ElementInput::stencilParameters(const StencilCompareFunction func,
                                                                       const StencilOperation pass,
                                                                       const uint32_t ref)
    {
        const uint64_t key = (static_cast<uint64_t>(func) << 40) | (static_cast<uint64_t>(pass) << 32) | ref;
        auto& entry = _stencilCache[key];
        if (!entry) {
            entry = std::make_shared<StencilParameters>();
            entry->setCompareFunction(func);
            entry->setPassOperation(pass);
            entry->setReference(ref);
        }
        return entry;
    }

    bool ElementInput::MaterialKey::operator==(const MaterialKey& other) const
    {
        // Spelled out so it inlines: this runs for every part of every element each frame.
        const auto same = [](const Color& a, const Color& b) {
            return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
        };
        return kind == other.kind && worldSpace == other.worldSpace && texture == other.texture &&
            same(color, other.color) && opacity == other.opacity && pxRange == other.pxRange &&
            intensity == other.intensity && same(outlineColor, other.outlineColor) &&
            outlineThickness == other.outlineThickness && same(shadowColor, other.shadowColor) &&
            shadowUvOffset.x == other.shadowUvOffset.x && shadowUvOffset.y == other.shadowUvOffset.y;
    }

    bool ElementInput::MaterialKey::sameBuild(const MaterialKey& other) const
    {
        return kind == other.kind && worldSpace == other.worldSpace && texture == other.texture &&
            pxRange == other.pxRange && intensity == other.intensity;
    }

    size_t ElementInput::MaterialKeyHash::operator()(const MaterialKey& key) const
    {
        // FNV-1a over the fields, one by one: the struct has padding, and 0.0f == -0.0f
        // must hash alike since they compare equal.
        uint64_t hash = kFnv1aOffsetBasis;
        const auto mix = [&hash](const uint64_t value) { hash = fnv1aMix(hash, value); };
        const auto mixFloat = [&mix](const float value) {
            uint32_t bits = 0;
            const float canonical = value == 0.0f ? 0.0f : value;
            std::memcpy(&bits, &canonical, sizeof(bits));
            mix(bits);
        };
        const auto mixColor = [&mixFloat](const Color& c) {
            mixFloat(c.r);
            mixFloat(c.g);
            mixFloat(c.b);
            mixFloat(c.a);
        };
        mix(static_cast<uint64_t>(key.kind) | (key.worldSpace ? 0x100u : 0u));
        mix(reinterpret_cast<uintptr_t>(key.texture));
        mixColor(key.color);
        mixFloat(key.opacity);
        if (key.kind == MaterialKey::Kind::MsdfText) {
            mixFloat(key.pxRange);
            mixFloat(key.intensity);
            mixColor(key.outlineColor);
            mixFloat(key.outlineThickness);
            mixColor(key.shadowColor);
            mixFloat(key.shadowUvOffset.x);
            mixFloat(key.shadowUvOffset.y);
        }
        return static_cast<size_t>(hash);
    }

    namespace
    {
        /// What changes when an element is restyled: colour, opacity and, for MSDF text,
        /// its outline and shadow.
        void writeMaterialStyle(StandardMaterial& material, const ElementInput::MaterialKey& key)
        {
            material.setEmissive(key.color);
            material.setOpacity(key.opacity);
            if (key.kind == ElementInput::MaterialKey::Kind::MsdfText) {
                material.setMsdfOutline(key.outlineColor, key.outlineThickness);
                material.setMsdfShadow(key.shadowColor, key.shadowUvOffset);
            }
        }

        std::shared_ptr<StandardMaterial> makeElementMaterial(const ElementInput::MaterialKey& key)
        {
            using Kind = ElementInput::MaterialKey::Kind;
            auto material = makeBaseElementMaterial(key.worldSpace);
            switch (key.kind) {
            case Kind::MsdfText:
                material->setMsdfMap(key.texture);
                material->setMsdfFont(key.pxRange, key.intensity);
                break;
            case Kind::BitmapText:
                material->setOpacityMap(key.texture);   // coverage in alpha
                break;
            case Kind::Image:
            case Kind::ImageMask:
                // An image multiplies its texture into the colour and takes its alpha.
                material->setOpacityMap(key.texture);
                material->setEmissiveMap(key.texture);
                break;
            }
            if (key.kind == Kind::ImageMask) {
                // The mask material: into the stencil alone, every colour
                // channel off, and only where the image is fully opaque (alpha test 1),
                // which is what lets a sprite's transparent corners shape the mask.
                // setAlphaMode resets the blend, the depth state and transparency,
                // so it goes first and the element's own depth state and sublayer (the
                // transparent one, sorted by draw order) are put back after it.
                const auto depthState = material->depthState();
                material->setAlphaMode(AlphaMode::MASK);
                material->setAlphaCutoff(1.0f);
                material->setDepthState(depthState);
                material->setTransparent(true);
                auto blend = std::make_shared<BlendState>(BlendState::alphaBlend());
                blend->setRedWrite(false);
                blend->setGreenWrite(false);
                blend->setBlueWrite(false);
                blend->setAlphaWrite(false);
                material->setBlendState(blend);
            }
            writeMaterialStyle(*material, key);
            return material;
        }
    }

    ElementInput::MaterialKey ElementInput::materialKeyFor(const ElementVisual& visual, const VisualPart& part,
                                                           const ElementComponent* element)
    {
        MaterialKey key;
        key.worldSpace = visual.worldSpace;
        key.texture = part.texture;
        key.opacity = element->opacity();
        if (visual.type != ElementType::Text) {
            key.kind = element->mask() ? MaterialKey::Kind::ImageMask : MaterialKey::Kind::Image;
            key.color = element->color();
            return key;
        }

        // Style 0 is the element's own, read live (a pulsing colour or a changed outline
        // needs no rebuild); a markup style is what its tags resolved to.
        const bool own = part.style == 0 || part.style >= static_cast<int>(visual.styles.size());
        const FontResource* font = element->fontResource();
        key.color = own ? element->color() : visual.styles[static_cast<size_t>(part.style)].color;
        if (!font || !font->msdf) {
            key.kind = MaterialKey::Kind::BitmapText;
            return key;
        }
        key.kind = MaterialKey::Kind::MsdfText;
        key.pxRange = font->pxRange;
        key.intensity = font->intensity;
        const TextStyle style = own
            ? TextStyle{element->color(), element->outlineColor(), element->outlineThickness(),
                        element->shadowColor(), element->shadowOffset()}
            : visual.styles[static_cast<size_t>(part.style)];
        // Editor units: thickness x 0.2, offset x 0.005 of the page.
        key.outlineColor = style.outlineColor;
        key.outlineThickness = 0.2f * style.outlineThickness;
        key.shadowColor = style.shadowColor;
        key.shadowUvOffset = msdfShadowUvOffset(style.shadowOffset, part.texture, visual.markupStyles);
        return key;
    }

    void ElementInput::applyPartMaterial(VisualPart& part, const MaterialKey& key, MeshInstance* unmask)
    {
        if (part.material && part.materialKey == key) {
            return;
        }

        std::shared_ptr<StandardMaterial> next;
        if (const auto it = _materials.find(key); it != _materials.end()) {
            next = it->second.lock();
        }
        if (!next) {
            if (part.material && part.material.use_count() == 1 && key.sameBuild(part.materialKey)) {
                // Nobody else draws with this part's material: restyle it, under its new
                // key, rather than build one. An element whose colour animates lands here
                // every frame.
                _materials.erase(part.materialKey);
                next = part.material;
                writeMaterialStyle(*next, key);
            } else {
                next = makeElementMaterial(key);
            }
            _materials.insert_or_assign(key, next);
            if (_materials.size() >= _materialPruneAt) {
                std::erase_if(_materials, [](const auto& entry) { return entry.second.expired(); });
                _materialPruneAt = std::max<size_t>(64, _materials.size() * 2);
            }
        }

        part.material = std::move(next);
        part.materialKey = key;
        if (part.meshInstance) {
            part.meshInstance->setMaterial(part.material.get());
        }
        if (unmask) {
            unmask->setMaterial(part.material.get());
        }
    }

    void ElementInput::styleParts(ElementVisual& visual, const ElementComponent* element)
    {
        for (auto& part : visual.parts) {
            if (part.meshInstance) {
                part.meshInstance->setDrawOrder(element->drawOrder());
            }
            if (part.customMaterial) {   // a custom material styles itself
                continue;
            }
            applyPartMaterial(part, materialKeyFor(visual, part, element),
                &part == &visual.parts.front() ? visual.unmask : nullptr);
        }
    }

    bool ElementInput::setPartGeometry(VisualPart& part, const std::vector<float>& vertices,
                                       const std::vector<uint32_t>& indices, const BoundingBox& bounds)
    {
        auto block = _geometry ? _geometry->allocate(vertices, indices) : nullptr;
        if (!block) {
            return false;
        }
        if (!part.mesh) {
            part.mesh = std::make_shared<Mesh>();
        }
        part.mesh->setVertexBuffer(block->vertexBuffer());
        part.mesh->setIndexBuffer(block->indexBuffer(), 0);
        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.base = static_cast<int>(block->firstIndex());
        primitive.count = static_cast<int>(block->indexCount());
        primitive.indexed = true;
        part.mesh->setPrimitive(primitive, 0);
        part.mesh->setAabb(bounds);
        // The block this replaces goes back to the arena, which keeps it out of use until
        // no frame in flight can still be drawing it.
        part.geometry = std::move(block);
        return true;
    }

    void ElementInput::syncMasks()
    {
        // How far past its last descendant an unmask draws, from
        // 0.5 down by 0.001 each time the same element is asked in a frame, so masks ending
        // on the same element unmask innermost first (the walk meets the outer one first).
        std::unordered_map<const ElementComponent*, double> maskOffsets;
        const auto maskOffset = [&maskOffsets](const ElementComponent* element) {
            const auto [it, inserted] = maskOffsets.emplace(element, 0.5);
            const double offset = it->second;
            it->second -= 0.001;
            return offset;
        };
        const auto elementOf = [](GraphNode* node) -> ElementComponent* {
            auto* entity = dynamic_cast<Entity*>(node);
            return entity ? entity->findComponent<ElementComponent>() : nullptr;
        };
        // The last element child, followed down to its own last one
        const auto lastDescendant = [&elementOf](ElementComponent* element) {
            ElementComponent* last = nullptr;
            for (ElementComponent* current = element; current;) {
                ElementComponent* next = nullptr;
                for (const auto& child : current->entity()->children()) {
                    if (ElementComponent* childElement = elementOf(child.get())) {
                        next = childElement;
                    }
                }
                if (next) {
                    last = next;
                }
                current = next;
            }
            return last;
        };
        const auto visualOf = [this](const ElementComponent* element) {
            return static_cast<ElementVisual*>(element->drawRecord(this));
        };
        const auto setPartsStencil = [&visualOf](ElementComponent* element, const std::shared_ptr<StencilParameters>& sp) {
            if (ElementVisual* visual = visualOf(element)) {
                for (auto& part : visual->parts) {
                    if (part.meshInstance) {
                        part.meshInstance->setStencil(sp, sp);
                    }
                }
            }
        };
        std::unordered_map<const ElementComponent*, uint32_t> maskRefs;
        bool anyMask = false;

        // Depth-first, each element tested against the mask above it,
        // and each mask written (the outermost with REPLACE, a nested one with INCREMENT
        // inside its parent's value) and unmasked after its last descendant.
        const auto update = [&](auto&& self, ElementComponent* element, ElementComponent* currentMask,
                                uint32_t depth) -> void {
            element->setMaskedBy(currentMask);
            const uint32_t parentRef = currentMask ? maskRefs[currentMask] : 0u;
            const bool isMask = element->mask() && element->type() == ElementType::Image;
            if (!isMask) {
                setPartsStencil(element, currentMask
                    ? stencilParameters(StencilCompareFunction::Equal, StencilOperation::Keep, parentRef) : nullptr);
            } else {
                anyMask = true;
                setPartsStencil(element, currentMask
                    ? stencilParameters(StencilCompareFunction::Equal, StencilOperation::IncrementClamp, parentRef)
                    : stencilParameters(StencilCompareFunction::Always, StencilOperation::Replace, depth));
                maskRefs[element] = depth;
                if (ElementVisual* visual = visualOf(element); visual && visual->unmask) {
                    // Back to the parent's value: where the stencil holds this mask's
                    // (parentRef + 1), decrement it.
                    const auto sp = stencilParameters(StencilCompareFunction::Equal,
                                                      StencilOperation::DecrementClamp, parentRef + 1u);
                    visual->unmask->setStencil(sp, sp);
                    ElementComponent* last = lastDescendant(element);
                    visual->unmask->setDrawOrder(last
                        ? static_cast<double>(last->drawOrder()) + maskOffset(last)
                        : static_cast<double>(element->drawOrder()) + maskOffset(element));
                }
                ++depth;
                currentMask = element;
            }
            for (const auto& child : element->entity()->children()) {
                if (ElementComponent* childElement = elementOf(child.get())) {
                    self(self, childElement, currentMask, depth);
                }
            }
        };

        // Every element tree: an element whose parent entity has none (the
        // element directly under a screen, or at the root).
        for (auto* element : ElementComponent::instances()) {
            if (!element || !element->entity() || element->entity()->engine() != _engine.get()) {
                continue;
            }
            if (elementOf(element->entity()->parent())) {
                continue;
            }
            update(update, element, nullptr, 1u);
        }
        _masksApplied = anyMask;
    }

    ElementInput::ElementVisual& ElementInput::visualFor(ElementComponent* element)
    {
        auto* known = static_cast<ElementVisual*>(element->drawRecord(this));
        if (!known) {
            const auto [it, inserted] = _visuals.try_emplace(element);
            if (!inserted) {
                // An entry under this address already: it belongs to an element destroyed
                // since the last sync, whose memory this new element was given. Nothing of
                // it applies here.
                releaseVisual(it->second);
                it->second = ElementVisual{};
            }
            known = &it->second;
            element->setDrawRecord(this, known);
        }
        auto& visual = *known;
        if (visual.entity && visual.type != element->type()) {
            // The element changed type: its visual is built for the other kind.
            releaseVisual(visual);
            visual = ElementVisual{};
        }
        if (visual.entity) {
            return visual;
        }
        if (!visual.parts.empty() || visual.destroyHandle) {
            // The entity was destroyed from outside and took its mesh instances with it;
            // what was built for it is rebuilt with the new one.
            releaseVisual(visual);
            visual = ElementVisual{};
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
        // The element's clone gets a visual of its own from the next sync; a copy of this
        // one would draw this element's geometry beside it.
        visual.entity->setExcludedFromClone(true);
        // The visual lives under its element, so destroying the element frees it: forget
        // it then, rather than touching freed memory when the element leaves the list.
        visual.destroyHandle = visual.entity->on("destroy", [&visual]() {
            visual.entity = nullptr;
            visual.render = nullptr;
            visual.unmask = nullptr;
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
        if (!_geometry) {
            _geometry = UiGeometryArena::create(_engine->graphicsDevice().get());
        }
        _geometry->beginFrame();
        ++_syncSerial;

        size_t seen = 0;
        bool anyMask = false;
        for (auto* element : ElementComponent::instances()) {
            if (!element) {
                continue;
            }
            const bool isText = element->type() == ElementType::Text && element->fontResource() &&
                element->fontResource()->texture && !element->text().empty();
            const bool isImage = element->type() == ElementType::Image;
            if (!element->entity() || (!isText && !isImage)) {
                // Nothing to draw. If it had a visual, that goes now, while the element is
                // here to be told.
                if (auto* stale = static_cast<ElementVisual*>(element->drawRecord(this))) {
                    releaseVisual(*stale);
                    element->setDrawRecord(this, nullptr);
                    _visuals.erase(element);
                }
                continue;
            }

            auto& visual = visualFor(element);
            visual.syncSerial = _syncSerial;
            ++seen;
            if (!visual.entity) {
                continue;
            }
            anyMask = anyMask || (isImage && element->mask());

            // The element's own layers, or the element system's choice. Compared in place:
            // a copy here is an allocation per element per frame.
            const std::vector<int>& ownLayers = element->layers();
            const int fallbackLayer = element->screen() ? LAYERID_UI : LAYERID_WORLD;
            const bool layersCurrent = ownLayers.empty()
                ? visual.layers.size() == 1 && visual.layers.front() == fallbackLayer
                : visual.layers == ownLayers;
            if (visual.render && !layersCurrent) {
                visual.layers = ownLayers.empty() ? std::vector<int>{fallbackLayer} : ownLayers;
                visual.render->setLayers(visual.layers);
            }

            const bool sizeChanged =
                std::abs(visual.cachedPivot.x - element->pivot().x) > 1e-4f ||
                std::abs(visual.cachedPivot.y - element->pivot().y) > 1e-4f ||
                std::abs(visual.cachedWidth - element->calculatedWidth()) > 1e-4f ||
                std::abs(visual.cachedHeight - element->calculatedHeight()) > 1e-4f;

            // What changed decides how much is rebuilt: `rebuild` makes the parts again,
            // while a change of size alone only gives the existing parts new geometry.
            bool rebuild = visual.parts.empty();
            if (isText) {
                // Every text input marks the element dirty (and re-measures it).
                rebuild = rebuild || element->textDirty();
            } else {
                const Sprite* sprite = element->sprite().get();
                const uint64_t atlasVersion = sprite && sprite->atlas() ? sprite->atlas()->version() : 0;
                rebuild = rebuild || visual.cachedMask != element->mask() ||
                    visual.cachedImageVersion != element->imageVersion() ||
                    visual.cachedSprite != sprite ||
                    visual.cachedSpriteVersion != (sprite ? sprite->version() : 0) ||
                    visual.cachedAtlasVersion != atlasVersion ||
                    visual.parts.front().customMaterial != element->material();
            }

            bool rangeStale = false;
            if (!rebuild && sizeChanged) {
                // Same parts, new geometry: each part's mesh instance and material stay.
                if (isText) {
                    TextRuns text = buildTextRuns(element);
                    bool sameRuns = text.runs.size() == visual.parts.size();
                    for (size_t i = 0; sameRuns && i < text.runs.size(); ++i) {
                        sameRuns = text.runs[i].key == std::pair<int, int>{visual.parts[i].page, visual.parts[i].style};
                    }
                    if (sameRuns) {
                        for (size_t i = 0; i < text.runs.size(); ++i) {
                            setPartGeometry(visual.parts[i], text.runs[i].vertices, text.runs[i].indices, text.bounds);
                            visual.parts[i].quadSymbols = std::move(text.runs[i].quadSymbols);
                        }
                        rangeStale = true;
                    } else {
                        rebuild = true;
                    }
                } else {
                    const ImageMeshData image = buildImageMeshData(element);
                    if (image.texture != visual.parts.front().texture ||
                        !setPartGeometry(visual.parts.front(), image.vertices, image.indices, image.bounds)) {
                        rebuild = true;
                    }
                }
            }

            if (rebuild) {
                if (visual.render) {
                    visual.render->clearMeshInstances();
                }
                // The old parts stay until the new ones have their materials, so a part
                // that comes back the same finds its material still alive to share.
                std::vector<VisualPart> previous = std::move(visual.parts);
                visual.parts.clear();
                visual.unmask = nullptr;
                if (isText) {
                    const FontResource* font = element->fontResource();
                    TextRuns text = buildTextRuns(element);
                    for (auto& run : text.runs) {
                        const auto [page, style] = run.key;
                        VisualPart part;
                        if (!setPartGeometry(part, run.vertices, run.indices, text.bounds)) {
                            continue;
                        }
                        part.quadSymbols = std::move(run.quadSymbols);
                        part.page = page;
                        part.style = style;
                        part.texture = page < static_cast<int>(font->pages.size())
                            ? font->pages[static_cast<size_t>(page)] : font->texture;
                        visual.parts.push_back(std::move(part));
                    }
                    visual.styles = std::move(text.styles);
                    visual.markupStyles = !element->markupTags().empty();
                    element->clearTextDirty();
                } else {
                    const ImageMeshData image = buildImageMeshData(element);
                    VisualPart part;
                    part.texture = image.texture;
                    // A custom material draws the quad, and owns what the
                    // element's own material would have done with its colour and texture.
                    part.customMaterial = element->material();
                    visual.cachedMask = element->mask();
                    if (setPartGeometry(part, image.vertices, image.indices, image.bounds)) {
                        visual.parts.push_back(std::move(part));
                    }
                    const Sprite* sprite = element->sprite().get();
                    visual.cachedImageVersion = element->imageVersion();
                    visual.cachedSprite = sprite;
                    visual.cachedSpriteVersion = sprite ? sprite->version() : 0;
                    visual.cachedAtlasVersion = sprite && sprite->atlas() ? sprite->atlas()->version() : 0;
                }

                // Materials before the mesh instances that are created with them.
                styleParts(visual, element);
                if (visual.render) {
                    for (auto& part : visual.parts) {
                        Material* material = part.customMaterial ? part.customMaterial.get() : part.material.get();
                        auto meshInstance = std::make_unique<MeshInstance>(part.mesh.get(), material, visual.entity);
                        meshInstance->setScreenSpace(!visual.worldSpace);
                        part.meshInstance = meshInstance.get();
                        visual.render->addMeshInstance(std::move(meshInstance));
                    }
                    if (!isText && element->mask() && !visual.parts.empty() && visual.parts.front().material) {
                        const VisualPart& part = visual.parts.front();
                        auto unmask = std::make_unique<MeshInstance>(part.mesh.get(), part.material.get(), visual.entity);
                        unmask->setScreenSpace(!visual.worldSpace);
                        visual.unmask = unmask.get();
                        visual.render->addMeshInstance(std::move(unmask));
                    }
                }
                rangeStale = true;
            }
            if (rebuild || sizeChanged) {
                visual.cachedWidth = element->calculatedWidth();
                visual.cachedHeight = element->calculatedHeight();
                visual.cachedPivot = element->pivot();
            }

            // Draw only the symbols in [rangeStart, rangeEnd), by
            // narrowing each part's index range to its quads inside it — no new layout.
            if (isText && (rangeStale || visual.cachedRangeVersion != element->rangeVersion())) {
                const auto start = static_cast<uint32_t>(element->rangeStart());
                const auto end = static_cast<uint32_t>(element->rangeEnd());
                for (auto& part : visual.parts) {
                    const auto& q = part.quadSymbols;
                    const auto first = static_cast<int>(std::lower_bound(q.begin(), q.end(), start) - q.begin());
                    const auto last = static_cast<int>(std::lower_bound(q.begin(), q.end(), end) - q.begin());
                    Primitive primitive;
                    primitive.type = PRIMITIVE_TRIANGLES;
                    // within the part's own run of the shared index buffer
                    primitive.base = static_cast<int>(part.geometry->firstIndex()) + first * 6;
                    primitive.count = std::max(last - first, 0) * 6;
                    primitive.indexed = true;
                    part.mesh->setPrimitive(primitive, 0);
                    if (part.meshInstance) {
                        // a part with nothing in the range would draw no indices
                        part.meshInstance->setVisible(last > first);
                    }
                }
                visual.cachedRangeVersion = element->rangeVersion();
            }

            styleParts(visual, element);
            visual.entity->setEnabled(element->enabled() && element->entity()->enabled());
        }

        // With no mask anywhere, now or at the last walk, there is no stencil state and no
        // `maskedBy` to maintain, and the walk over every element tree is skipped.
        if (anyMask || _masksApplied) {
            syncMasks();
        }

        // A visual this sync did not see belongs to an element that is gone. Looked for
        // only when the counts say there is one.
        if (seen != _visuals.size()) {
            std::erase_if(_visuals, [this](auto& entry) {
                if (entry.second.syncSerial == _syncSerial) {
                    return false;
                }
                releaseVisual(entry.second);
                return true;
            });
        }
    }
}
