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
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include "framework/components/button/buttonComponent.h"
#include "framework/components/componentSystem.h"
#include "framework/components/element/elementComponent.h"
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
        struct GlyphQuad
        {
            float x0 = 0.0f;
            float y0 = 0.0f;
            float x1 = 0.0f;
            float y1 = 0.0f;
            float u0 = 0.0f;
            float v0 = 0.0f;
            float u1 = 0.0f;
            float v1 = 0.0f;
        };

        std::vector<std::string> splitLines(const std::string& text)
        {
            std::vector<std::string> lines;
            std::stringstream ss(text);
            std::string line;
            while (std::getline(ss, line, '\n')) {
                lines.push_back(line);
            }
            if (text.empty() || text.back() == '\n') {
                lines.push_back("");
            }
            return lines;
        }

        float lineWidthForText(const std::string& text, const FontResource* font, const float scale)
        {
            if (!font || text.empty()) {
                return 0.0f;
            }

            float width = 0.0f;
            int prev = -1;
            for (char c : text) {
                const int code = static_cast<unsigned char>(c);
                const auto it = font->glyphs.find(code);
                const float advance = it != font->glyphs.end() ? it->second.xadvance * scale : (font->lineHeight * 0.35f * scale);
                const float kern = prev >= 0 ? font->kerningValue(prev, code) * scale : 0.0f;
                width += kern + advance;
                prev = code;
            }
            return width;
        }

        std::vector<std::string> wrapLine(const std::string& line, const FontResource* font, const float scale, const float maxWidth)
        {
            if (!font || maxWidth <= 0.0f || line.empty()) {
                return {line};
            }

            std::vector<std::string> out;
            std::string current;
            size_t i = 0;
            while (i < line.size()) {
                size_t j = i;
                while (j < line.size() && line[j] != ' ') {
                    ++j;
                }
                const std::string word = line.substr(i, j - i);
                const bool hasSpace = (j < line.size() && line[j] == ' ');
                const std::string token = hasSpace ? (word + " ") : word;

                const std::string candidate = current + token;
                if (!current.empty() && lineWidthForText(candidate, font, scale) > maxWidth) {
                    out.push_back(current);
                    current.clear();
                }

                if (current.empty() && lineWidthForText(token, font, scale) > maxWidth) {
                    // Fallback to per-character split for very long words.
                    std::string part;
                    for (char c : token) {
                        const std::string next = part + c;
                        if (!part.empty() && lineWidthForText(next, font, scale) > maxWidth) {
                            out.push_back(part);
                            part.clear();
                        }
                        part.push_back(c);
                    }
                    current += part;
                } else {
                    current += token;
                }

                i = hasSpace ? (j + 1) : j;
            }

            if (!current.empty()) {
                out.push_back(current);
            }
            return out;
        }

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

        /// One mesh per atlas page the text touches, keyed by page (upstream builds a mesh
        /// instance per `meshInfo` the same way: each page is its own texture).
        std::vector<std::pair<int, std::shared_ptr<Mesh>>> buildTextMeshes(const std::shared_ptr<GraphicsDevice>& gd,
                                                                           const ElementComponent* element)
        {
            if (!gd || !element || !element->fontResource() || element->text().empty()) {
                return {};
            }

            const FontResource* font = element->fontResource();
            const float lineHeight = std::max(font->lineHeight, 1.0f);
            const float scale = static_cast<float>(element->fontSize()) / lineHeight;

            std::vector<std::string> lines = splitLines(element->text());
            if (element->wrapLines()) {
                std::vector<std::string> wrapped;
                wrapped.reserve(lines.size());
                for (const auto& l : lines) {
                    auto sub = wrapLine(l, font, scale, element->calculatedWidth());
                    wrapped.insert(wrapped.end(), sub.begin(), sub.end());
                }
                lines = std::move(wrapped);
            }

            struct PageBuffers
            {
                std::vector<float> vertices;
                std::vector<uint32_t> indices;
                uint32_t base = 0;
            };
            std::map<int, PageBuffers> pages;

            const float boxW = element->calculatedWidth();
            const float boxH = element->calculatedHeight();
            const Vector2 pivot = element->pivot();
            const float lineStep = lineHeight * scale;

            // The block of lines is placed by the vertical alignment: its top at the box's
            // top for 1, its bottom at the box's bottom for 0. The box may be INVERTED (a
            // split axis whose margins cross), which the same formula handles.
            const float blockH = lineStep * static_cast<float>(lines.size());
            float yTop = (1.0f - pivot.y) * boxH - (1.0f - element->verticalAlign()) * (boxH - blockH);

            for (size_t li = 0; li < lines.size(); ++li) {
                const std::string& line = lines[li];

                float lineWidth = 0.0f;
                int prevForWidth = -1;
                for (char c : line) {
                    const int code = static_cast<unsigned char>(c);
                    if (const auto it = font->glyphs.find(code); it != font->glyphs.end()) {
                        lineWidth += (prevForWidth >= 0 ? font->kerningValue(prevForWidth, code) * scale : 0.0f);
                        lineWidth += it->second.xadvance * scale;
                        prevForWidth = code;
                    }
                }

                float x = -pivot.x * boxW;
                if (element->horizontalAlign() == ElementHorizontalAlign::Center) {
                    x += (boxW - lineWidth) * 0.5f;
                } else if (element->horizontalAlign() == ElementHorizontalAlign::Right) {
                    x += (boxW - lineWidth);
                }

                int prev = -1;
                for (char c : line) {
                    const int code = static_cast<unsigned char>(c);
                    const auto gIt = font->glyphs.find(code);
                    if (gIt == font->glyphs.end()) {
                        x += lineHeight * 0.35f * scale;
                        prev = code;
                        continue;
                    }

                    const FontGlyph& g = gIt->second;
                    const float kerning =
                        (prev >= 0 ? font->kerningValue(prev, code) : 0.0f);

                    // Glyph placement mirrors upstream text-element.js exactly:
                    //
                    //   left   = pen - (xoffset - kerning) * scale
                    //   bottom = penY - yoffset * scale
                    //   right/top = left/bottom + quadsize      (a SQUARE cell)
                    //
                    // Two things here are easy to get wrong. The offsets are
                    // SUBTRACTED, not added: in an MSDF atlas every glyph sits in
                    // a fixed cell (64x64 here) and xoffset/yoffset say where the
                    // pen sits INSIDE that cell, so the cell is pulled back to
                    // line the glyph up. Adding them instead displaces every
                    // character by a different amount — proportional fonts come
                    // out visibly scrambled, while a monospace font (courier,
                    // which every earlier example used) hides it because the
                    // offsets are then all equal.
                    //
                    // And the quad is square — (width + height) / 2 — not
                    // width x height, so a non-square atlas cell cannot skew it.
                    const float quadSize = (g.width + g.height) * 0.5f * scale;
                    const float penY = yTop - static_cast<float>(li) * lineStep;

                    const float gx0 = x - (g.xoffset - kerning) * scale;
                    const float gx1 = gx0 + quadSize;
                    const float gyBot = penY - g.yoffset * scale;
                    const float gyTop = gyBot + quadSize;

                    // UVs are fractions of the glyph's OWN page.
                    const int page = (g.page >= 0 && g.page < static_cast<int>(font->pages.size())) ? g.page : 0;
                    const Texture* pageTexture = font->pages.empty() ? font->texture : font->pages[static_cast<size_t>(page)];
                    const float atlasW = static_cast<float>(std::max(pageTexture ? static_cast<int>(pageTexture->width())
                                                                                  : font->atlasWidth, 1));
                    const float atlasH = static_cast<float>(std::max(pageTexture ? static_cast<int>(pageTexture->height())
                                                                                  : font->atlasHeight, 1));
                    const float u0 = g.x / atlasW;
                    const float u1 = (g.x + g.width) / atlasW;
                    // Use native texture-space orientation for this backend.
                    const float v0 = g.y / atlasH;
                    const float v1 = (g.y + g.height) / atlasH;

                    // position(3) normal(3) uv0(2) tangent(4) uv1(2)
                    const std::array<float, 56> quadVerts = {
                        gx0, gyTop, 0.0f,   0.0f, 0.0f, 1.0f,   u0, v0,   1.0f,0.0f,0.0f,1.0f,   u0, v0,
                        gx1, gyTop, 0.0f,   0.0f, 0.0f, 1.0f,   u1, v0,   1.0f,0.0f,0.0f,1.0f,   u1, v0,
                        gx1, gyBot, 0.0f,   0.0f, 0.0f, 1.0f,   u1, v1,   1.0f,0.0f,0.0f,1.0f,   u1, v1,
                        gx0, gyBot, 0.0f,   0.0f, 0.0f, 1.0f,   u0, v1,   1.0f,0.0f,0.0f,1.0f,   u0, v1
                    };
                    PageBuffers& buffers = pages[page];
                    buffers.vertices.insert(buffers.vertices.end(), quadVerts.begin(), quadVerts.end());
                    const uint32_t vbase = buffers.base;
                    // Use front-facing winding for UI camera (+Z looking toward origin).
                    buffers.indices.insert(buffers.indices.end(),
                        {vbase + 0u, vbase + 2u, vbase + 1u, vbase + 0u, vbase + 3u, vbase + 2u});
                    buffers.base += 4u;

                    x += (g.xadvance + kerning) * scale;
                    prev = code;
                }
            }

            BoundingBox bounds;
            bounds.setCenter(Vector3(0.0f, 0.0f, 0.0f));
            bounds.setHalfExtents(Vector3(std::max(boxW * 0.5f, 1.0f), std::max(boxH * 0.5f, 1.0f), 1.0f));
            std::vector<std::pair<int, std::shared_ptr<Mesh>>> meshes;
            for (const auto& [page, buffers] : pages) {
                if (auto mesh = makeUiMesh(gd, buffers.vertices, buffers.indices, bounds)) {
                    meshes.emplace_back(page, std::move(mesh));
                }
            }
            return meshes;
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

        /// Upstream text-element's shadow_offset: 0.005 of the page per unit, the y term
        /// scaled by the page's aspect. Upstream's glyph UVs run v UP and this engine's run
        /// v DOWN, so the y term changes sign to put the shadow in the same place on screen.
        Vector2 msdfShadowUvOffset(const Vector2& offset, const Texture* page)
        {
            const float aspect = page && page->height() > 0
                ? static_cast<float>(page->width()) / static_cast<float>(page->height()) : 1.0f;
            return Vector2(0.005f * offset.x, aspect * 0.005f * offset.y);
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
                rebuild = rebuild || element->textDirty() ||
                    visual.cachedText != element->text() ||
                    visual.cachedFontSize != element->fontSize() ||
                    visual.cachedAlign != element->horizontalAlign() ||
                    visual.cachedWrap != element->wrapLines() ||
                    visual.cachedVerticalAlign != element->verticalAlign() ||
                    visual.cachedFont != element->fontResource();
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
                    for (auto& [page, mesh] : buildTextMeshes(_engine->graphicsDevice(), element)) {
                        VisualPart part;
                        part.mesh = std::move(mesh);
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
                    visual.cachedText = element->text();
                    visual.cachedFontSize = element->fontSize();
                    visual.cachedAlign = element->horizontalAlign();
                    visual.cachedWrap = element->wrapLines();
                    visual.cachedVerticalAlign = element->verticalAlign();
                    visual.cachedFont = element->fontResource();
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
                if (!part.styled || !(part.color == element->color()) || part.opacity != element->opacity()) {
                    part.material->setEmissive(element->color());
                    part.material->setOpacity(element->opacity());
                    part.color = element->color();
                    part.opacity = element->opacity();
                }
                if (msdf && (!part.styled || !(part.outlineColor == element->outlineColor()) ||
                             part.outlineThickness != element->outlineThickness() ||
                             !(part.shadowColor == element->shadowColor()) ||
                             part.shadowOffset.x != element->shadowOffset().x ||
                             part.shadowOffset.y != element->shadowOffset().y)) {
                    // Upstream's editor units: thickness x 0.2, offset x 0.005 of the page.
                    part.material->setMsdfOutline(element->outlineColor(), 0.2f * element->outlineThickness());
                    part.material->setMsdfShadow(element->shadowColor(),
                                                 msdfShadowUvOffset(element->shadowOffset(), part.texture));
                    part.outlineColor = element->outlineColor();
                    part.outlineThickness = element->outlineThickness();
                    part.shadowColor = element->shadowColor();
                    part.shadowOffset = element->shadowOffset();
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
