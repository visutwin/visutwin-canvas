// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace visutwin::canvas
{
    class Engine;

    // ── Loaded data produced by background thread ──────────────────────────

    /**
     * Raw data produced by a ResourceHandler on the background I/O thread.
     *
     * For textures the handler decodes pixels into PixelData so only the GPU
     * upload step remains on the main thread.  For containers and fonts the
     * handler reads raw bytes; parsing happens on the main thread because the
     * parsers create GPU resources.
     */
    struct LoadedData
    {
        /// Source file path.
        std::string url;

        /// Raw file bytes (always available for container / font types).
        std::vector<uint8_t> bytes;

        /// Decoded pixel data — populated only by TextureResourceHandler.
        struct PixelData
        {
            std::vector<uint8_t>  pixels;      ///< LDR: RGBA8 interleaved.
            std::vector<float>    hdrPixels;   ///< HDR: RGBA32F interleaved.
            int  width    = 0;
            int  height   = 0;
            int  channels = 0;
            bool isHdr    = false;
        };

        std::optional<PixelData> pixelData;

        /// Type-erased pre-parsed data produced by the handler on the
        /// background thread.  For GLB containers this holds a
        /// shared_ptr<tinygltf::Model> so the main-thread callback only
        /// performs fast GPU resource creation instead of the full parse.
        std::shared_ptr<void> preparsed;

        /// Type-erased fully pre-processed data produced by the handler
        /// on the background thread.  For GLB containers this holds a
        /// shared_ptr<PreparedGlbData> with all CPU-heavy results (pixel
        /// conversion, Draco decode, vertex/index extraction, tangent
        /// generation, animation parsing) so the main-thread callback
        /// only creates GPU resources.
        std::shared_ptr<void> preparedData;
    };

    // ── Callback types ─────────────────────────────────────────────────────

    using LoadSuccessCallback = std::function<void(std::unique_ptr<LoadedData>)>;
    using LoadErrorCallback   = std::function<void(const std::string& error)>;

    // ── Resource handler base ──────────────────────────────────────────────

    /**
     * Base class for type-specific resource loading.
     *
     * Subclasses implement `load()` which runs **entirely on the background
     * thread** and therefore must NOT call any GPU / Metal / GraphicsDevice
     * functions.
     */
    class ResourceHandler
    {
    public:
        virtual ~ResourceHandler() = default;

        /**
         * Load raw data from disk.
         *
         * @param url  Local file-system path.
         * @return  Loaded data on success, nullptr on failure.
         */
        virtual std::unique_ptr<LoadedData> load(const std::string& url) = 0;
    };

    // ── Async resource loader ──────────────────────────────────────────────

    /**
     * @brief Async resource loader with background I/O thread, pixel decoding, and main-thread callbacks.
     * @ingroup group_framework_assets
     *
     * Asynchronous resource loader with a single background I/O thread.
     *
     * **Thread model**
     *
     *  ┌──────────────┐   load()    ┌────────────────┐
     *  │  Main thread  │ ────────▶  │  Worker thread  │
     *  │               │            │  (I/O + decode) │
     *  │  process      │  ◀──────── │                 │
     *  │  Completions()│  callback  └────────────────┘
     *  └──────────────┘
     *
     * 1. Call `load()` from the main thread to queue a request.
     * 2. The worker thread picks up the request, calls the registered
     *    ResourceHandler::load(), and pushes the result to a completion queue.
     * 3. Call `processCompletions()` each frame (from Engine::update) to
     *    dispatch success / error callbacks on the main thread.
     *
     * Usage:
     * @code
     *   loader->addHandler("texture",   std::make_unique<TextureResourceHandler>());
     *   loader->addHandler("container",  std::make_unique<ContainerResourceHandler>());
     *   loader->load("path/to/img.png", "texture",
     *       [](auto data) { ... GPU upload ... },
     *       [](auto& err) { ... handle error ... });
     *   // each frame:
     *   loader->processCompletions();
     * @endcode
     */
    class ResourceLoader
    {
    public:
        explicit ResourceLoader(const std::shared_ptr<Engine>& engine);
        ~ResourceLoader();

        // Non-copyable, non-movable (owns a thread).
        ResourceLoader(const ResourceLoader&) = delete;
        ResourceLoader& operator=(const ResourceLoader&) = delete;

        /** Register a handler for a given asset type (e.g. "texture", "container"). */
        void addHandler(const std::string& type, std::unique_ptr<ResourceHandler> handler);

        /** Remove a handler for a given asset type. */
        void removeHandler(const std::string& type);

        /**
         * Queue an asynchronous load request.
         *
         * The background thread calls the appropriate ResourceHandler::load().
         * On the next processCompletions() call the onSuccess or onError
         * callback is invoked **on the main thread**.
         *
         * @param url       Local file-system path.
         * @param type      Asset type string used to look up the handler.
         * @param onSuccess Called on the main thread with loaded data.
         * @param onError   Called on the main thread with an error message.
         */
        void load(const std::string& url, const std::string& type,
                  LoadSuccessCallback onSuccess, LoadErrorCallback onError = nullptr);

        /**
         * Must be called from the main thread each frame (from Engine::update).
         * Dispatches completed load callbacks.
         *
         * @param maxCompletions  Maximum number of callbacks to dispatch per
         *                        call.  0 means unlimited (drain all).  Use 1
         *                        to spread heavy main-thread work (e.g. GLB
         *                        parsing) across frames and avoid stalling the
         *                        event loop.
         */
        void processCompletions(int maxCompletions = 0);

        /** Shut down the worker thread.  Safe to call multiple times. */
        void shutdown();

        /** Returns true if there are pending or in-flight requests. */
        bool hasPending() const;

    private:
        void workerLoop();

        struct Request
        {
            std::string url;
            std::string type;
            LoadSuccessCallback onSuccess;
            LoadErrorCallback   onError;
        };

        struct Completion
        {
            std::unique_ptr<LoadedData> data;
            std::string                 error;
            LoadSuccessCallback         onSuccess;
            LoadErrorCallback           onError;
        };

        std::weak_ptr<Engine> _engine;

        // Handler registry — written from the main thread only, read from worker.
        std::unordered_map<std::string, std::unique_ptr<ResourceHandler>> _handlers;

        // Worker thread.
        std::thread      _worker;
        std::atomic<bool> _running{false};

        // Request queue  (main thread → worker).
        std::mutex              _requestMutex;
        std::condition_variable _requestCV;
        std::deque<Request>     _requests;

        // Completion queue  (worker → main thread).
        std::mutex          _completionMutex;
        std::deque<Completion> _completions;

        std::atomic<int> _pendingCount{0};
    };

    // ── Built-in resource handlers ─────────────────────────────────────────

    /** Reads an image file and decodes pixels using stb_image on the
     *  background thread.  The main-thread callback only needs to create
     *  the Texture object and upload to the GPU. */
    class TextureResourceHandler : public ResourceHandler
    {
    public:
        std::unique_ptr<LoadedData> load(const std::string& url) override;
    };

    /** Reads raw file bytes and, for GLB files, also pre-parses the tinygltf
     *  model on the background thread.  The main-thread callback only performs
     *  GPU resource creation (fast) rather than the full parse.  For OBJ/STL
     *  etc. only raw bytes are read — parsing still runs on the main thread. */
    class ContainerResourceHandler : public ResourceHandler
    {
    public:
        std::unique_ptr<LoadedData> load(const std::string& url) override;
    };

    /** Reads raw font file bytes on the background thread. */
    class FontResourceHandler : public ResourceHandler
    {
    public:
        std::unique_ptr<LoadedData> load(const std::string& url) override;
    };
}
