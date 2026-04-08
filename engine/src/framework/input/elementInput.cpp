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
                    auto sub = wrapLine(l, font, scale, element->width());
                    wrapped.insert(wrapped.end(), sub.begin(), sub.end());
                }
                lines = std::move(wrapped);
            }

            std::vector<float> vertices;
            std::vector<uint32_t> indices;
            vertices.reserve(lines.size() * 64u * 14u);
            indices.reserve(lines.size() * 64u * 6u);

            const float boxW = element->width();
            const float boxH = element->height();
            const Vector2 pivot = element->pivot();
            const float lineStep = lineHeight * scale;

            float yTop = (1.0f - pivot.y) * boxH;
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
                    x += (prev >= 0 ? font->kerningValue(prev, code) * scale : 0.0f);

                    const float gx0 = x + g.xoffset * scale;
                    const float gyTop = yTop - static_cast<float>(li) * lineStep - g.yoffset * scale;
                    const float gx1 = gx0 + g.width * scale;
                    const float gyBot = gyTop - g.height * scale;

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

                    x += g.xadvance * scale;
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
            auto vertexFormat = std::make_shared<VertexFormat>(14 * static_cast<int>(sizeof(float)), true, false);
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
            if (visual.entity) {
                visual.entity->remove();
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

        const Vector3 pos = element->entity()->position();
        const float width = std::max(element->width(), 0.0f);
        const float height = std::max(element->height(), 0.0f);
        const Vector2 pivot = element->pivot();

        outRect.x = pos.getX() - pivot.x * width;
        outRect.y = pos.getY() - pivot.y * height;
        outRect.w = width;
        outRect.h = height;
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

        int windowW = 1;
        int windowH = 1;
        if (_engine && _engine->sdlWindow()) {
            SDL_GetWindowSize(_engine->sdlWindow(), &windowW, &windowH);
        }

        for (auto* screen : ScreenComponent::instances()) {
            if (screen && screen->enabled()) {
                screen->updateScaleFromWindow(windowW, windowH);
            }
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

        int windowW = 1;
        int windowH = 1;
        if (_engine->sdlWindow()) {
            SDL_GetWindowSize(_engine->sdlWindow(), &windowW, &windowH);
        }
        float uiWidth = static_cast<float>(std::max(windowW, 1));
        float uiHeight = static_cast<float>(std::max(windowH, 1));
        for (auto* screen : ScreenComponent::instances()) {
            if (!screen || !screen->enabled() || !screen->screenSpace()) {
                continue;
            }
            const float scale = std::max(screen->scale(), 1e-6f);
            uiWidth = screen->resolution().x / scale;
            uiHeight = screen->resolution().y / scale;
            break;
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
                visual.entity = new Entity();
                visual.entity->setEngine(_engine.get());
                visual.entity->setLocalPosition(0.0f, 0.0f, 5.0f);
                visual.render = static_cast<RenderComponent*>(visual.entity->addComponent<RenderComponent>());
                if (visual.render) {
                    visual.render->setLayers({LAYERID_UI});
                }
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
                textDepth->setDepthTest(false);
                visual.material->setDepthState(textDepth);
                visual.material->setDiffuseMap(element->fontResource()->texture);
                visual.material->setOpacityMap(element->fontResource()->texture);
                if (visual.render) {
                    visual.render->setMaterial(visual.material.get());
                }
                _engine->root()->addChild(visual.entity);
            }

            const bool needsRebuild = element->textDirty() ||
                visual.cachedText != element->text() ||
                visual.cachedFontSize != element->fontSize() ||
                visual.cachedAlign != element->horizontalAlign() ||
                visual.cachedWrap != element->wrapLines() ||
                visual.cachedFont != element->fontResource() ||
                std::abs(visual.cachedPivot.x - element->pivot().x) > 1e-4f ||
                std::abs(visual.cachedPivot.y - element->pivot().y) > 1e-4f ||
                std::abs(visual.cachedWidth - element->width()) > 1e-4f ||
                std::abs(visual.cachedHeight - element->height()) > 1e-4f;

            if (needsRebuild) {
                visual.mesh = buildTextMesh(_engine->graphicsDevice(), element);
                if (visual.render) {
                    visual.render->clearMeshInstances();
                    if (visual.mesh) {
                        visual.material->setDiffuseMap(element->fontResource()->texture);
                        visual.material->setOpacityMap(element->fontResource()->texture);
                        auto meshInstance = std::make_unique<MeshInstance>(visual.mesh.get(), visual.material.get(), visual.entity);
                        visual.render->addMeshInstance(std::move(meshInstance));
                    }
                }
                visual.cachedText = element->text();
                visual.cachedFontSize = element->fontSize();
                visual.cachedWidth = element->width();
                visual.cachedHeight = element->height();
                visual.cachedPivot = element->pivot();
                visual.cachedAlign = element->horizontalAlign();
                visual.cachedWrap = element->wrapLines();
                visual.cachedFont = element->fontResource();
                element->clearTextDirty();
            }

            static std::unordered_set<const ElementComponent*> logged;
            if (logged.find(element) == logged.end()) {
                const Vector3 pos = element->entity()->position();
                spdlog::info("UI text element '{}': glyphs={}, meshBuilt={}, size=({}, {}), worldPos=({}, {}, {})",
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
                const Vector3 pos = element->entity()->position();
                const float worldX = pos.getX() - uiWidth * 0.5f;
                const float worldY = uiHeight * 0.5f - pos.getY();
                visual.entity->setLocalPosition(worldX, worldY, 5.0f);
                visual.entity->setEnabled(element->enabled() && element->entity()->enabled());
            }
        }

        std::vector<ElementComponent*> toRemove;
        toRemove.reserve(_textVisuals.size());
        for (auto& [element, visual] : _textVisuals) {
            if (!visual.activeFrame) {
                if (visual.entity) {
                    visual.entity->remove();
                }
                toRemove.push_back(element);
            }
        }
        for (auto* element : toRemove) {
            _textVisuals.erase(element);
        }
    }
}
