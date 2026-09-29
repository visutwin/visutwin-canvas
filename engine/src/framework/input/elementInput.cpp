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
#include <sstream>
#include <string>
#include <unordered_set>
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

        std::shared_ptr<Mesh> buildTextMesh(const std::shared_ptr<GraphicsDevice>& gd, const ElementComponent* element)
        {
            if (!gd || !element || !element->fontResource() || element->text().empty()) {
                return nullptr;
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

            std::vector<float> vertices;
            std::vector<uint32_t> indices;
            vertices.reserve(lines.size() * 64u * 14u);
            indices.reserve(lines.size() * 64u * 6u);

            const float boxW = element->calculatedWidth();
            const float boxH = element->calculatedHeight();
            const Vector2 pivot = element->pivot();
            const float lineStep = lineHeight * scale;

            // The block of lines is placed by the vertical alignment: its top at the box's
            // top for 1, its bottom at the box's bottom for 0. The box may be INVERTED (a
            // split axis whose margins cross), which the same formula handles.
            const float blockH = lineStep * static_cast<float>(lines.size());
            float yTop = (1.0f - pivot.y) * boxH - (1.0f - element->verticalAlign()) * (boxH - blockH);
            uint32_t vbase = 0;

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

                    const float atlasW = static_cast<float>(std::max(font->atlasWidth, 1));
                    const float atlasH = static_cast<float>(std::max(font->atlasHeight, 1));
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
                    vertices.insert(vertices.end(), quadVerts.begin(), quadVerts.end());
                    // Use front-facing winding for UI camera (+Z looking toward origin).
                    indices.insert(indices.end(), {vbase + 0u, vbase + 2u, vbase + 1u, vbase + 0u, vbase + 3u, vbase + 2u});
                    vbase += 4u;

                    x += (g.xadvance + kerning) * scale;
                    prev = code;
                }
            }

            if (vertices.empty() || indices.empty()) {
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

            BoundingBox bounds;
            bounds.setCenter(Vector3(0.0f, 0.0f, 0.0f));
            bounds.setHalfExtents(Vector3(std::max(boxW * 0.5f, 1.0f), std::max(boxH * 0.5f, 1.0f), 1.0f));
            mesh->setAabb(bounds);
            return mesh;
        }
    }

    void ElementInput::detach()
    {
        for (auto& [_, visual] : _textVisuals) {
            if (visual.destroyHandle) {
                visual.destroyHandle->off();
            }
            if (visual.entity) {
                auto removed = visual.entity->remove();
                removed.reset();
                visual.entity = nullptr;
                visual.render = nullptr;
            }
        }
        _textVisuals.clear();
        _engine.reset();
        _sdlRenderer = nullptr;
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

    void ElementInput::renderElements()
    {
        if (!_sdlRenderer) {
            return;
        }

        SDL_SetRenderDrawBlendMode(_sdlRenderer, SDL_BLENDMODE_BLEND);

        for (auto* element : ElementComponent::instances()) {
            if (!element || !element->entity() || !element->enabled() || !element->entity()->enabled()) {
                continue;
            }

            SDL_FRect rect{};
            if (!computeElementRect(element, rect)) {
                continue;
            }

            const Color c = element->color();
            const int alpha = static_cast<int>(std::round(std::clamp(element->opacity() * c.a, 0.0f, 1.0f) * 255.0f));
            const Uint8 r = static_cast<Uint8>(std::round(std::clamp(c.r, 0.0f, 1.0f) * 255.0f));
            const Uint8 g = static_cast<Uint8>(std::round(std::clamp(c.g, 0.0f, 1.0f) * 255.0f));
            const Uint8 b = static_cast<Uint8>(std::round(std::clamp(c.b, 0.0f, 1.0f) * 255.0f));
            const Uint8 a = static_cast<Uint8>(std::clamp(alpha, 0, 255));

            if (element->type() == ElementType::Image) {
                SDL_SetRenderDrawColor(_sdlRenderer, r, g, b, a);
                SDL_RenderFillRect(_sdlRenderer, &rect);
            }
        }
    }

    void ElementInput::syncTextElements()
    {
        if (!_engine || !_engine->graphicsDevice()) {
            return;
        }

        for (auto& [_, visual] : _textVisuals) {
            visual.activeFrame = false;
        }

        for (auto* element : ElementComponent::instances()) {
            if (!element || element->type() != ElementType::Text || !element->entity()) {
                continue;
            }
            if (!element->fontResource() || !element->fontResource()->texture) {
                continue;
            }

            auto& visual = _textVisuals[element];
            visual.activeFrame = true;

            if (!visual.entity) {
                // Screen-space text is drawn in clip space by whatever camera renders the UI
                // layer (MeshInstance::setScreenSpace); anything else — an element on no
                // screen or on a world-space one — is geometry in the world.
                const ScreenComponent* screen = element->screenComponent();
                visual.worldSpace = !(screen && screen->screenSpace());
                visual.entity = new Entity();
                visual.entity->setEngine(_engine.get());
                visual.entity->setLocalPosition(0.0f, 0.0f, 0.0f);
                // The visual lives under its element, so destroying the element frees it:
                // forget it then, rather than touching freed memory when the element
                // leaves the instance list.
                visual.destroyHandle = visual.entity->on("destroy", [&visual]() {
                    visual.entity = nullptr;
                    visual.render = nullptr;
                });
                visual.render = static_cast<RenderComponent*>(visual.entity->addComponent<RenderComponent>());
                visual.material = std::make_shared<StandardMaterial>();
                visual.material->setUseLighting(false);
                visual.material->setUseSkybox(false);
                visual.material->setTransparent(true);
                visual.material->setCullMode(CullMode::CULLFACE_NONE);
                visual.material->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
                visual.material->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
                auto alphaBlend = std::make_shared<BlendState>(BlendState::alphaBlend());
                visual.material->setBlendState(alphaBlend);
                auto textDepth = std::make_shared<DepthState>(DepthState::noWrite());
                // Screen-space text is an overlay and must never be occluded; a
                // world-space label is part of the scene, so geometry in front of it
                // should hide it. Depth WRITES stay off either way — the text is
                // transparent and must not punch holes in the depth buffer.
                textDepth->setDepthTest(visual.worldSpace);
                visual.material->setDepthState(textDepth);
                // DEVIATION: upstream's text material takes colour from
                // emissiveMap and alpha from opacityMap (channel 'a'). The opacity
                // map is Metal-only here, so the glyph alpha comes from the diffuse
                // map's alpha instead, which both backends read. Setting the same
                // texture as the opacity map as well multiplies it in a second
                // time on Metal (alpha squared) and thins every anti-aliased edge.
                visual.material->setDiffuseMap(element->fontResource()->texture);
                if (visual.render) {
                    visual.render->setMaterial(visual.material.get());
                }
                // A child of its element with an identity transform: the element's
                // transform — the layout's, for an element on a screen — places it.
                element->entity()->addChild(visual.entity);
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

            const bool needsRebuild = element->textDirty() ||
                visual.cachedText != element->text() ||
                visual.cachedFontSize != element->fontSize() ||
                visual.cachedAlign != element->horizontalAlign() ||
                visual.cachedWrap != element->wrapLines() ||
                visual.cachedVerticalAlign != element->verticalAlign() ||
                visual.cachedFont != element->fontResource() ||
                std::abs(visual.cachedPivot.x - element->pivot().x) > 1e-4f ||
                std::abs(visual.cachedPivot.y - element->pivot().y) > 1e-4f ||
                std::abs(visual.cachedWidth - element->calculatedWidth()) > 1e-4f ||
                std::abs(visual.cachedHeight - element->calculatedHeight()) > 1e-4f;

            if (needsRebuild) {
                visual.mesh = buildTextMesh(_engine->graphicsDevice(), element);
                if (visual.render) {
                    visual.render->clearMeshInstances();
                    if (visual.mesh) {
                        visual.material->setDiffuseMap(element->fontResource()->texture);
                        auto meshInstance = std::make_unique<MeshInstance>(visual.mesh.get(), visual.material.get(), visual.entity);
                        meshInstance->setScreenSpace(!visual.worldSpace);
                        visual.render->addMeshInstance(std::move(meshInstance));
                    }
                }
                visual.cachedText = element->text();
                visual.cachedFontSize = element->fontSize();
                visual.cachedWidth = element->calculatedWidth();
                visual.cachedHeight = element->calculatedHeight();
                visual.cachedPivot = element->pivot();
                visual.cachedAlign = element->horizontalAlign();
                visual.cachedWrap = element->wrapLines();
                visual.cachedVerticalAlign = element->verticalAlign();
                visual.cachedFont = element->fontResource();
                element->clearTextDirty();
            }

            static std::unordered_set<const ElementComponent*> logged;
            if (logged.find(element) == logged.end()) {
                const Vector3 pos = element->entity()->position();
                spdlog::info("{} text element '{}': glyphs={}, meshBuilt={}, size=({}, {}), pos=({}, {}, {})",
                    visual.worldSpace ? "World-space" : "UI",
                    element->text(),
                    element->fontResource()->glyphs.size(),
                    visual.mesh ? "true" : "false",
                    element->width(),
                    element->height(),
                    pos.getX(),
                    pos.getY(),
                    pos.getZ());
                logged.insert(element);
            }

            const Color c = element->color();
            visual.material->setDiffuse(c);
            visual.material->setEmissive(c);
            visual.material->setOpacity(element->opacity());

            if (visual.entity) {
                visual.entity->setEnabled(element->enabled() && element->entity()->enabled());
            }
        }

        std::vector<ElementComponent*> toRemove;
        toRemove.reserve(_textVisuals.size());
        for (auto& [element, visual] : _textVisuals) {
            if (!visual.activeFrame) {
                if (visual.entity) {
                    auto removed = visual.entity->remove();
                    removed.reset();
                    visual.entity = nullptr;
                    visual.render = nullptr;
                }
                toRemove.push_back(element);
            }
        }
        for (auto* element : toRemove) {
            if (auto& handle = _textVisuals[element].destroyHandle) {
                handle->off();   // it captures the map entry erased next
            }
            _textVisuals.erase(element);
        }
    }
}
