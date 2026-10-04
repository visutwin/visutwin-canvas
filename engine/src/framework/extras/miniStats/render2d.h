// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "framework/input/uiGeometryArena.h"
#include "scene/materials/shaderMaterial.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class GraphNode;
    class Layer;
    class Mesh;
    class MeshInstance;
    class Texture;

    /// Render2d's material: its shader, its three textures and its uniform block.
    class Render2dMaterial final : public ShaderMaterial
    {
    public:
        /// Material texture slots: the same numbers on both backends.
        static constexpr int kRegularPageSlot = 0;
        static constexpr int kBoldPageSlot = 1;
        static constexpr int kGraphSlot = 3;

        explicit Render2dMaterial(const std::shared_ptr<GraphicsDevice>& device);

        void setTextures(Texture* regularPage, Texture* boldPage, Texture* graph);
        /// Moves the uniforms version only when a value changed.
        void setUniforms(const std::array<float, 4>& clr, const std::array<float, 4>& params);

        [[nodiscard]] const std::array<float, 4>& clr() const { return _data.clr; }
        [[nodiscard]] const std::array<float, 4>& params() const { return _data.params; }

        void getTextureSlots(std::vector<TextureSlot>& slots) const override;
        const void* customUniformData(size_t& outSize) const override;

    private:
        struct alignas(16) Data
        {
            std::array<float, 4> clr = {1.0f, 1.0f, 1.0f, 1.0f};
            std::array<float, 4> params = {0.0f, 4.0f, 1.0f, 1.0f};
        } _data;
        Texture* _regularPage = nullptr;
        Texture* _boldPage = nullptr;
        Texture* _graph = nullptr;
    };

    /**
     * A list of screen-space quads drawn as ONE mesh instance with one material: solid
     * rectangles, MSDF glyphs from two font pages, and graph rows scrolling across a
     * history texture. The performance HUD is drawn with it.
     *
     * Positions are in points from the target's bottom-left corner (setTargetSize gives
     * the target in points). Texture coordinates are in texels from the texture's top-left
     * corner. Colours are packed bytes 0xAABBGGRR in display space and are written as they
     * are: no tone mapping, no gamma.
     *
     * The quad list is rebuilt from scratch — startFrame(), then quads — only when what
     * it shows changes. render() commits a changed list into a block of its own geometry
     * arena, which keeps a replaced block out of use until no frame in flight can still
     * read it. Between rebuilds nothing is written: a graph scrolls by setGraphCursor, a
     * uniform. DEVIATION: the history scrolls through a uniform rather than by rewriting
     * each graph quad's texture coordinates every frame, because a vertex rewritten in place
     * on Metal lands in memory the frames still in flight are reading.
     *
     * The mesh instance is screen space and draws once per frame
     * (MeshInstance::setDrawOncePerFrame): the first camera to render its layer draws it,
     * last in that layer's order, and no other camera draws it again.
     */
    class Render2d
    {
    public:
        enum class Mode : uint8_t
        {
            Solid = 0,
            Text = 1,       // the regular font page
            Graph = 2,
            TextBold = 3    // the bold font page
        };

        /// The most quads a list may hold; a quad past it is dropped with one warning.
        static constexpr int kMaxQuads = 8192;

        explicit Render2d(const std::shared_ptr<GraphicsDevice>& device);
        ~Render2d();

        Render2d(const Render2d&) = delete;
        Render2d& operator=(const Render2d&) = delete;

        /// The target's size in points, which quad positions are measured in.
        void setTargetSize(float width, float height);
        [[nodiscard]] float targetWidth() const { return _targetWidth; }
        [[nodiscard]] float targetHeight() const { return _targetHeight; }

        /// Quads are cut to this rectangle (points, bottom-left origin) as they are added.
        void setClip(float x, float y, float width, float height);
        void clearClip();

        /// Begins a new quad list; the previous one stays drawn until the next render().
        void startFrame();

        /**
         * Adds a quad and returns its index, or -1 when the clip removed it or the list is
         * full. (u, v, uw, uh) is the source rectangle in texels of a texture of size
         * (textureWidth, textureHeight), v from its top row.
         */
        int quad(float x, float y, float width, float height, float u, float v, float uw, float uh,
                 float textureWidth, float textureHeight, Mode mode, uint32_t color);

        int rect(float x, float y, float width, float height, uint32_t color);

        /**
         * A graph quad over row `row` of a history texture of the given size, ending at the
         * cursor: its newest column is drawn at the right edge, one texel per point.
         */
        int graph(float x, float y, float width, float height, int row,
                  float textureWidth, float textureHeight, uint32_t color);

        [[nodiscard]] int quadCount() const { return static_cast<int>(_quads.size()); }

        /// The two font pages and the graph history (any may be null while unused).
        void setTextures(Texture* regularPage, Texture* boldPage, Texture* graph);
        /// The MSDF pixel range and the atlas size of the font pages.
        void setMsdf(float pixelRange, float atlasWidth, float atlasHeight);
        /// Where the history texture's write cursor is, in texels.
        void setGraphCursor(float texels, float textureWidth);
        /// Multiplies every fragment.
        void setColor(float r, float g, float b, float a);

        /// Puts the overlay on `layer` (null takes it off), committing the quad list if it
        /// changed. Once a frame.
        void render(Layer* layer);
        void setLayer(Layer* layer);

        [[nodiscard]] MeshInstance* meshInstance() const { return _meshInstance.get(); }
        [[nodiscard]] Render2dMaterial* material() const { return _material.get(); }
        /// The packed vertices of the current list (UiGeometryArena::kFloatsPerVertex per
        /// vertex, four vertices per quad), for inspection.
        [[nodiscard]] const std::vector<float>& vertices() const { return _vertices; }

    private:
        void commit();

        struct Quad
        {
            float x0, y0, x1, y1;   // points, clipped
            float u0, v0, u1, v1;   // normalised; v0 at the bottom edge
            float bottom, top;      // height fractions of the clipped edges
            float invHeight;
            Mode mode;
            uint32_t color;
        };

        std::shared_ptr<GraphicsDevice> _device;
        std::shared_ptr<UiGeometryArena> _arena;
        std::unique_ptr<UiGeometryArena::Block> _block;
        std::shared_ptr<Mesh> _mesh;
        std::shared_ptr<Render2dMaterial> _material;
        std::unique_ptr<GraphNode> _node;
        std::unique_ptr<MeshInstance> _meshInstance;
        Layer* _layer = nullptr;

        std::vector<Quad> _quads;
        std::vector<float> _vertices;
        std::vector<uint32_t> _indices;
        bool _dirty = true;
        bool _warnedFull = false;

        std::array<float, 4> _clr = {1.0f, 1.0f, 1.0f, 1.0f};
        // (graph cursor u, MSDF pixel range, MSDF atlas width, MSDF atlas height)
        std::array<float, 4> _params = {0.0f, 4.0f, 1.0f, 1.0f};

        float _targetWidth = 1.0f;
        float _targetHeight = 1.0f;
        float _clipLeft = 0.0f;
        float _clipBottom = 0.0f;
        float _clipRight = 0.0f;
        float _clipTop = 0.0f;
        bool _clipped = false;
    };
}
