// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026.
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "framework/parsers/glbContainerResource.h"

namespace tinygltf { class Model; struct Image; }

namespace visutwin::canvas
{
    class GraphicsDevice;

    // ── Pre-processed model data (background thread → main thread) ──────

    /**
     * Pre-processed model data produced on the background thread.
     *
     * Contains all CPU-heavy results (pixel format conversion, Draco
     * decompression, vertex extraction, tangent generation, animation
     * parsing) so the main-thread createFromPrepared() only performs
     * fast GPU resource creation.
     */
    struct PreparedGlbData
    {
        /// Pre-converted RGBA8 image, one per tinygltf::Model::images entry.
        struct ImageData
        {
            std::vector<uint8_t> rgbaPixels;   ///< RGBA8 interleaved pixels.
            int width  = 0;
            int height = 0;
            bool valid = false;                ///< True if conversion succeeded.
        };

        /// Pre-built vertex/index byte buffers for one mesh primitive.
        struct PrimitiveData
        {
            std::vector<uint8_t> vertexBytes;  ///< PackedVertex data.
            std::vector<uint8_t> indexBytes;    ///< uint32_t index data.
            int vertexCount = 0;
            int drawCount   = 0;
            bool indexed    = false;
            int mode        = 4;               ///< glTF primitive mode.
            Vector3 boundsMin;
            Vector3 boundsMax;
            int materialIndex = -1;
        };

        /// Pre-converted images indexed by tinygltf image index.
        std::vector<ImageData> images;

        /// Per-mesh primitives: meshPrimitives[meshIndex][primIndex].
        std::vector<std::vector<PrimitiveData>> meshPrimitives;

        /// Fully parsed animation tracks (keyed by animation name).
        std::unordered_map<std::string, std::shared_ptr<AnimTrack>> animTracks;

        size_t dracoPrimitiveCount      = 0;
        size_t dracoDecodeSuccessCount  = 0;
        size_t dracoDecodeFailureCount  = 0;
    };

    // ── GLB parser ──────────────────────────────────────────────────────

    class GlbParser
    {
    public:
        /// Parse a GLB file from disk.
        static std::unique_ptr<GlbContainerResource> parse(const std::string& path,
            const std::shared_ptr<GraphicsDevice>& device);

        /// Parse a GLB from an in-memory byte buffer (e.g. extracted from b3dm).
        static std::unique_ptr<GlbContainerResource> parseFromMemory(
            const std::uint8_t* data, std::size_t length,
            const std::shared_ptr<GraphicsDevice>& device,
            const std::string& debugName = "memory");

        /**
         * Create GPU resources from a pre-parsed tinygltf model.
         *
         * Call this on the **main thread** after the background thread has
         * completed tinygltf parsing (the heavy CPU work: JSON parse + image
         * decode + Draco decompress).  This method still performs Draco decode,
         * vertex extraction, tangent generation, and pixel conversion on the
         * main thread.  Prefer prepareFromModel() + createFromPrepared() for
         * the fully offloaded path.
         *
         * @param model      Pre-parsed tinygltf model (moved in — consumed).
         * @param device     Graphics device for GPU resource creation.
         * @param debugName  Label for log messages.
         */
        static std::unique_ptr<GlbContainerResource> createFromModel(
            tinygltf::Model& model,
            const std::shared_ptr<GraphicsDevice>& device,
            const std::string& debugName = "memory");

        /**
         * Pre-process a tinygltf model on the **background thread**.
         *
         * Performs all CPU-heavy work: pixel format conversion, Draco
         * decompression, vertex extraction, tangent generation, and
         * animation parsing.  The result can be passed to
         * createFromPrepared() on the main thread for fast GPU resource
         * creation.
         *
         * @param model  Pre-parsed tinygltf model.
         * @return       Prepared data ready for GPU resource creation.
         */
        static PreparedGlbData prepareFromModel(tinygltf::Model& model);

        /**
         * Create GPU resources from fully pre-processed model data.
         *
         * Call on the **main thread** with data produced by
         * prepareFromModel() on the background thread.  Only creates GPU
         * buffers and textures — all CPU-intensive work has already been
         * done.
         *
         * @param model      Pre-parsed tinygltf model (for metadata only).
         * @param prepared   Pre-processed data (consumed via move).
         * @param device     Graphics device for GPU resource creation.
         * @param debugName  Label for log messages.
         */
        static std::unique_ptr<GlbContainerResource> createFromPrepared(
            tinygltf::Model& model,
            PreparedGlbData&& prepared,
            const std::shared_ptr<GraphicsDevice>& device,
            const std::string& debugName = "memory");

        /**
         * Image-loader callback for tinygltf.
         *
         * Public so the background ContainerResourceHandler can register it
         * when calling tinygltf::LoadBinaryFromMemory.  Uses per-thread
         * stb_image flip state for thread safety.
         */
        static bool loadImageData(tinygltf::Image* image, int imageIndex,
            std::string* err, std::string* warn, int reqWidth, int reqHeight,
            const unsigned char* bytes, int size, void* userData);
    };
}
