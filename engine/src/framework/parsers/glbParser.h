// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026.
//
#pragma once

#include "scene/gsplat/gsplatData.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "framework/parsers/glbContainerResource.h"
#include "scene/morph.h"

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

            /// KHR_texture_basisu: KTX2 payload transcoded to a block-compressed
            /// format on the background thread (rgbaPixels stays empty).
            bool isCompressed = false;
            uint32_t compressedFormat = 0;     ///< PixelFormat enum value.
            std::vector<std::vector<uint8_t>> compressedLevels;
        };

        /// Pre-built vertex/index byte buffers for one mesh primitive.
        struct PrimitiveData
        {
            std::vector<uint8_t> vertexBytes;  ///< PackedVertex data (88-byte skinned layout when skinned).
            std::vector<uint8_t> indexBytes;    ///< uint32_t index data.
            int vertexCount = 0;
            int drawCount   = 0;
            bool indexed    = false;
            bool skinned    = false;           ///< vertexBytes use the skinned layout (weights+joints).
            int mode        = 4;               ///< glTF primitive mode.
            Vector3 boundsMin;
            Vector3 boundsMax;
            int materialIndex = -1;
            std::vector<MorphTarget> morphTargets;      ///< CPU morph deltas (GPU buffer built on main thread).
            std::vector<float> morphInitialWeights;     ///< glTF mesh.weights.
            /// A POINTS primitive: vertexBytes hold the 28-byte position + colour
            /// layout, drawn unlit with vertex colours rather than lit as triangles.
            bool pointCloud = false;
            /// KHR_materials_variants: (variant index, material index) pairs.
            std::vector<std::pair<int, int>> variantMaterials;
        };

        /// Pre-converted images indexed by tinygltf image index.
        std::vector<ImageData> images;

        /// Per-mesh primitives: meshPrimitives[meshIndex][primIndex].
        std::vector<std::vector<PrimitiveData>> meshPrimitives;

        /// KHR_gaussian_splatting: per mesh, the splat sets its POINTS primitives
        /// carry, decoded here (the CPU-heavy half) and turned into GPU resources on
        /// the main thread. Splat primitives appear in neither meshPrimitives nor the
        /// point-cloud merge.
        std::vector<std::vector<std::unique_ptr<GSplatData>>> meshSplats;

        /// POINTS primitives of a model WITHOUT animations are merged into this one
        /// world-space cloud (one draw call) instead of appearing in meshPrimitives,
        /// and the leaf nodes that held nothing else are skipped. A model with
        /// animations keeps each cloud in its node's local space, so animating the
        /// node still moves it.
        bool pointCloudsMerged = false;
        PrimitiveData mergedPoints;   ///< Valid when pointCloudsMerged and vertexCount > 0.

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
         * Create GPU resources from a pre-parsed tinygltf model: prepareFromModel()
         * followed by createFromPrepared(), both on the calling thread. parse() and
         * parseFromMemory() end here too, so every load path builds its container
         * with the same code. Prefer calling the two halves yourself to put the
         * CPU-heavy half on a worker.
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
        /// `ktx2TargetFormat` is the block-compressed format KHR_texture_basisu images
        /// transcode to. It must be decided from the device on the MAIN thread and passed
        /// in, because this runs on a worker; see GraphicsDevice::preferredCompressedRgbaFormat.
        /// `debugName` names the file in any warning this raises (it runs on a
        /// worker, where nothing else identifies the load).
        static PreparedGlbData prepareFromModel(tinygltf::Model& model,
            PixelFormat ktx2TargetFormat, const std::string& debugName = {});

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
