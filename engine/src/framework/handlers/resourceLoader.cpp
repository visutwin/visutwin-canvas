// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//

#include "resourceLoader.h"

#include <algorithm>
#include <fstream>

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "framework/parsers/glbParser.h"
#include "spdlog/spdlog.h"
#include "stb_image.h"

namespace visutwin::canvas
{
    // ── ResourceLoader ─────────────────────────────────────────────────────

    ResourceLoader::ResourceLoader(const std::shared_ptr<Engine>& engine)
        : _engine(engine)
    {
        _running = true;
        _worker = std::thread(&ResourceLoader::workerLoop, this);
        spdlog::info("ResourceLoader: background I/O thread started");
    }

    ResourceLoader::~ResourceLoader()
    {
        shutdown();
    }

    void ResourceLoader::addHandler(const std::string& type, std::unique_ptr<ResourceHandler> handler)
    {
        _handlers[type] = std::move(handler);
    }

    void ResourceLoader::removeHandler(const std::string& type)
    {
        _handlers.erase(type);
    }

    void ResourceLoader::load(const std::string& url, const std::string& type,
                              LoadSuccessCallback onSuccess, LoadErrorCallback onError)
    {
        _pendingCount.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(_requestMutex);
            _requests.push_back({url, type, std::move(onSuccess), std::move(onError)});
        }
        _requestCV.notify_one();
    }

    void ResourceLoader::processCompletions(int maxCompletions)
    {
        // Move up to `maxCompletions` items out of the shared queue (0 = all).
        // Processing one heavy callback per frame (maxCompletions == 1) prevents
        // long main-thread stalls when several large assets finish on the same
        // frame (e.g. a 70 MB GLB whose parseFromMemory blocks for seconds).
        std::deque<Completion> batch;
        {
            std::lock_guard lock(_completionMutex);
            if (maxCompletions <= 0 || static_cast<int>(_completions.size()) <= maxCompletions) {
                batch.swap(_completions);
            } else {
                // Move only the first N completions into the local batch.
                auto end = _completions.begin() + maxCompletions;
                batch.assign(std::make_move_iterator(_completions.begin()),
                             std::make_move_iterator(end));
                _completions.erase(_completions.begin(), end);
            }
        }

        for (auto& c : batch) {
            if (!c.error.empty()) {
                if (c.onError) {
                    c.onError(c.error);
                } else {
                    spdlog::error("ResourceLoader: unhandled error: {}", c.error);
                }
            } else {
                if (c.onSuccess) {
                    c.onSuccess(std::move(c.data));
                }
            }
        }
    }

    void ResourceLoader::shutdown()
    {
        if (!_running.exchange(false)) {
            return; // Already shut down.
        }

        _requestCV.notify_all();
        if (_worker.joinable()) {
            _worker.join();
        }
        spdlog::info("ResourceLoader: background I/O thread stopped");
    }

    bool ResourceLoader::hasPending() const
    {
        return _pendingCount.load(std::memory_order_relaxed) > 0;
    }

    void ResourceLoader::workerLoop()
    {
        while (true) {
            Request req;
            {
                std::unique_lock lock(_requestMutex);
                _requestCV.wait(lock, [this] {
                    return !_requests.empty() || !_running;
                });

                if (!_running && _requests.empty()) {
                    break;
                }
                if (_requests.empty()) {
                    continue;
                }

                req = std::move(_requests.front());
                _requests.pop_front();
            }

            Completion completion;
            completion.onSuccess = std::move(req.onSuccess);
            completion.onError   = std::move(req.onError);

            // Look up the handler for this asset type.
            auto it = _handlers.find(req.type);
            if (it == _handlers.end()) {
                completion.error = "No resource handler registered for type '" + req.type + "'";
                spdlog::error("ResourceLoader: {}", completion.error);
            } else {
                try {
                    auto loaded = it->second->load(req.url);
                    if (loaded) {
                        completion.data = std::move(loaded);
                    } else {
                        completion.error = "Handler returned null for '" + req.url + "'";
                    }
                } catch (const std::exception& e) {
                    completion.error = "Exception loading '" + req.url + "': " + e.what();
                    spdlog::error("ResourceLoader: {}", completion.error);
                }
            }

            {
                std::lock_guard lock(_completionMutex);
                _completions.push_back(std::move(completion));
            }
            _pendingCount.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // ── TextureResourceHandler ─────────────────────────────────────────────

    std::unique_ptr<LoadedData> TextureResourceHandler::load(const std::string& url)
    {
        // Use per-thread flip state for thread safety.
        stbi_set_flip_vertically_on_load_thread(false);

        const bool isHdr = url.size() >= 4 &&
            url.compare(url.size() - 4, 4, ".hdr") == 0;

        auto result = std::make_unique<LoadedData>();
        result->url = url;

        LoadedData::PixelData pd;
        pd.isHdr = isHdr;

        if (isHdr) {
            // ── HDR path: decode to RGBA32F ──
            float* hdrPixels = stbi_loadf(url.c_str(), &pd.width, &pd.height, &pd.channels, 0);
            if (!hdrPixels || pd.width <= 0 || pd.height <= 0) {
                spdlog::error("TextureResourceHandler: failed to decode HDR '{}'", url);
                if (hdrPixels) stbi_image_free(hdrPixels);
                return nullptr;
            }

            const size_t pixelCount = static_cast<size_t>(pd.width) * static_cast<size_t>(pd.height);
            pd.hdrPixels.resize(pixelCount * 4);
            for (size_t i = 0; i < pixelCount; ++i) {
                pd.hdrPixels[i * 4 + 0] = hdrPixels[i * pd.channels + 0];
                pd.hdrPixels[i * 4 + 1] = pd.channels > 1
                    ? hdrPixels[i * pd.channels + 1] : hdrPixels[i * pd.channels + 0];
                pd.hdrPixels[i * 4 + 2] = pd.channels > 2
                    ? hdrPixels[i * pd.channels + 2] : hdrPixels[i * pd.channels + 0];
                pd.hdrPixels[i * 4 + 3] = 1.0f;
            }
            stbi_image_free(hdrPixels);

            spdlog::info("TextureResourceHandler: decoded HDR '{}' {}x{} ch={}",
                url, pd.width, pd.height, pd.channels);
        } else {
            // ── LDR path: decode to RGBA8 ──
            stbi_uc* pixels = stbi_load(url.c_str(), &pd.width, &pd.height, &pd.channels, STBI_rgb_alpha);
            if (!pixels || pd.width <= 0 || pd.height <= 0) {
                spdlog::error("TextureResourceHandler: failed to decode '{}'", url);
                if (pixels) stbi_image_free(pixels);
                return nullptr;
            }

            const size_t dataSize = static_cast<size_t>(pd.width) * static_cast<size_t>(pd.height) * 4;
            pd.pixels.assign(pixels, pixels + dataSize);
            stbi_image_free(pixels);

            spdlog::info("TextureResourceHandler: decoded '{}' {}x{} ch={}",
                url, pd.width, pd.height, pd.channels);
        }

        result->pixelData = std::move(pd);
        return result;
    }

    // ── ContainerResourceHandler ───────────────────────────────────────────

    namespace
    {
        bool hasExtension(const std::string& path, const std::string& ext)
        {
            if (path.size() < ext.size()) return false;
            auto pathExt = path.substr(path.size() - ext.size());
            std::transform(pathExt.begin(), pathExt.end(), pathExt.begin(), ::tolower);
            return pathExt == ext;
        }
    }

    std::unique_ptr<LoadedData> ContainerResourceHandler::load(const std::string& url)
    {
        std::ifstream file(url, std::ios::binary | std::ios::ate);
        if (!file) {
            spdlog::error("ContainerResourceHandler: cannot open '{}'", url);
            return nullptr;
        }

        const auto size = file.tellg();
        file.seekg(0);

        auto result = std::make_unique<LoadedData>();
        result->url = url;
        result->bytes.resize(static_cast<size_t>(size));
        file.read(reinterpret_cast<char*>(result->bytes.data()), size);

        if (!file) {
            spdlog::error("ContainerResourceHandler: read error for '{}'", url);
            return nullptr;
        }

        spdlog::info("ContainerResourceHandler: read {} bytes from '{}'", result->bytes.size(), url);

        // ── Pre-parse GLB on the background thread ──────────────────────
        // tinygltf::LoadBinaryFromMemory does the heavy CPU work: JSON
        // parsing, embedded image decoding via stb_image, and buffer view
        // resolution.  By running it here (on the I/O thread), only the
        // fast GPU resource creation remains for the main-thread callback.
        const bool isGlb = hasExtension(url, ".glb");
        const bool isGltf = hasExtension(url, ".gltf");

        if ((isGlb || isGltf) && !result->bytes.empty()) {
            auto model = std::make_shared<tinygltf::Model>();
            tinygltf::TinyGLTF loader;
            loader.SetImageLoader(GlbParser::loadImageData, nullptr);
            std::string warn, err;

            bool ok = false;
            if (isGlb) {
                ok = loader.LoadBinaryFromMemory(
                    model.get(), &err, &warn,
                    result->bytes.data(),
                    static_cast<unsigned int>(result->bytes.size()));
            } else {
                // GLTF is JSON text — parse from the byte buffer as a string.
                const std::string gltfString(
                    reinterpret_cast<const char*>(result->bytes.data()),
                    result->bytes.size());
                // Base dir for resolving relative URIs (external .bin/.png).
                std::string baseDir;
                if (auto pos = url.find_last_of("/\\"); pos != std::string::npos) {
                    baseDir = url.substr(0, pos + 1);
                }
                ok = loader.LoadASCIIFromString(
                    model.get(), &err, &warn,
                    gltfString.c_str(),
                    static_cast<unsigned int>(gltfString.size()),
                    baseDir);
            }

            if (!warn.empty()) {
                spdlog::warn("ContainerResourceHandler: tinygltf warning [{}]: {}", url, warn);
            }
            if (ok) {
                // Phase 2: pre-process all CPU-heavy work on the bg thread
                // (Draco decompression, vertex extraction, tangent generation,
                //  pixel format conversion, animation parsing).
                auto prepared = std::make_shared<PreparedGlbData>(
                    GlbParser::prepareFromModel(*model));
                result->preparsed = std::move(model);
                result->preparedData = std::move(prepared);
                spdlog::info("ContainerResourceHandler: pre-parsed + prepared {} on bg thread [{}]",
                    isGlb ? "GLB" : "GLTF", url);
            } else {
                // Pre-parse failed — fall back to main-thread parse.
                spdlog::warn("ContainerResourceHandler: bg pre-parse failed [{}]: {}", url, err);
            }
        }

        return result;
    }

    // ── FontResourceHandler ────────────────────────────────────────────────

    std::unique_ptr<LoadedData> FontResourceHandler::load(const std::string& url)
    {
        std::ifstream file(url, std::ios::binary | std::ios::ate);
        if (!file) {
            spdlog::error("FontResourceHandler: cannot open '{}'", url);
            return nullptr;
        }

        const auto size = file.tellg();
        file.seekg(0);

        auto result = std::make_unique<LoadedData>();
        result->url = url;
        result->bytes.resize(static_cast<size_t>(size));
        file.read(reinterpret_cast<char*>(result->bytes.data()), size);

        if (!file) {
            spdlog::error("FontResourceHandler: read error for '{}'", url);
            return nullptr;
        }

        spdlog::info("FontResourceHandler: read {} bytes from '{}'", result->bytes.size(), url);
        return result;
    }
} // visutwin::canvas
