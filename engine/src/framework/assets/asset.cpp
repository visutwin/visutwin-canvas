// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2025.
//
#include "asset.h"

#define STB_IMAGE_IMPLEMENTATION
#include "framework/handlers/resourceLoader.h"
#include "framework/parsers/assimpParser.h"
#include "framework/parsers/glbParser.h"
#include "framework/parsers/objParser.h"
#include "framework/parsers/stlParser.h"
#include "spdlog/spdlog.h"
#include "stb_image.h"

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

namespace visutwin::canvas
{
    std::weak_ptr<GraphicsDevice> Asset::_defaultGraphicsDevice;

    Asset::Asset(const std::string& name, const std::string& type, const std::string& file,
        const AssetData& data) : EventHandler(), _name(name), _type(type), _file(file), _data(data)
    {
    }

    void Asset::setDefaultGraphicsDevice(const std::shared_ptr<GraphicsDevice>& graphicsDevice)
    {
        _defaultGraphicsDevice = graphicsDevice;
    }

    void Asset::unload()
    {
        for (auto& resource : _resources) {
            if (std::holds_alternative<Texture*>(resource)) {
                delete std::get<Texture*>(resource);
            } else if (std::holds_alternative<ContainerResource*>(resource)) {
                delete std::get<ContainerResource*>(resource);
            } else if (std::holds_alternative<FontResource*>(resource)) {
                auto* font = std::get<FontResource*>(resource);
                if (font) {
                    delete font->texture;
                    font->texture = nullptr;
                    delete font;
                }
            }
        }
        _resources.clear();
    }

    std::optional<Resource> Asset::resource() {
        spdlog::info("Asset::resource loading '{}' type '{}'", _name, _type);
        if (_resources.empty()) {
            if (_type == AssetType::CONTAINER) {
                auto graphicsDevice = _defaultGraphicsDevice.lock();
                if (!graphicsDevice) {
                    spdlog::error("Cannot load container asset '{}': no graphics device set", _name);
                    return std::nullopt;
                }

                // Route by file extension: .obj → ObjParser, .stl → StlParser, else → GlbParser
                const bool isObj = _file.size() >= 4 &&
                    (_file.compare(_file.size() - 4, 4, ".obj") == 0 ||
                     _file.compare(_file.size() - 4, 4, ".OBJ") == 0);

                const bool isStl = _file.size() >= 4 &&
                    (_file.compare(_file.size() - 4, 4, ".stl") == 0 ||
                     _file.compare(_file.size() - 4, 4, ".STL") == 0);

                const bool isAssimp = _file.size() >= 4 &&
                    (_file.compare(_file.size() - 4, 4, ".dae") == 0 ||
                     _file.compare(_file.size() - 4, 4, ".DAE") == 0 ||
                     _file.compare(_file.size() - 4, 4, ".fbx") == 0 ||
                     _file.compare(_file.size() - 4, 4, ".FBX") == 0 ||
                     _file.compare(_file.size() - 4, 4, ".3ds") == 0 ||
                     _file.compare(_file.size() - 4, 4, ".ply") == 0 ||
                     _file.compare(_file.size() - 4, 4, ".PLY") == 0);

                std::unique_ptr<GlbContainerResource> container;
                if (isObj) {
                    container = ObjParser::parse(_file, graphicsDevice);
                } else if (isStl) {
                    container = StlParser::parse(_file, graphicsDevice);
                } else if (isAssimp) {
                    container = AssimpParser::parse(_file, graphicsDevice);
                } else {
                    container = GlbParser::parse(_file, graphicsDevice);
                }
                if (!container) {
                    return std::nullopt;
                }
                _resources.push_back(static_cast<ContainerResource*>(container.release()));
            } else if (_type == AssetType::TEXTURE) {
                auto graphicsDevice = _defaultGraphicsDevice.lock();
                if (!graphicsDevice) {
                    spdlog::error("Cannot load texture asset '{}': no graphics device set", _name);
                    return std::nullopt;
                }

                int width = 0;
                int height = 0;
                int channels = 0;
                // upstream texture loading keeps source orientation; env-atlas UV layout depends on this.
                stbi_set_flip_vertically_on_load(false);

                const bool isHdr = _file.size() >= 4 &&
                    _file.compare(_file.size() - 4, 4, ".hdr") == 0;

                if (isHdr) {
                    // HDR path: load as floating-point data
                    float* hdrPixels = stbi_loadf(_file.c_str(), &width, &height, &channels, 0);
                    if (!hdrPixels || width <= 0 || height <= 0) {
                        spdlog::error("Failed to decode HDR texture asset '{}' from '{}'", _name, _file);
                        if (hdrPixels) {
                            stbi_image_free(hdrPixels);
                        }
                        return std::nullopt;
                    }

                    // Convert to RGBA float (stbi_loadf returns RGB for HDR)
                    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
                    std::vector<float> rgbaData(pixelCount * 4);
                    for (size_t i = 0; i < pixelCount; ++i) {
                        rgbaData[i * 4 + 0] = hdrPixels[i * channels + 0];
                        rgbaData[i * 4 + 1] = channels > 1 ? hdrPixels[i * channels + 1] : hdrPixels[i * channels + 0];
                        rgbaData[i * 4 + 2] = channels > 2 ? hdrPixels[i * channels + 2] : hdrPixels[i * channels + 0];
                        rgbaData[i * 4 + 3] = 1.0f;
                    }
                    stbi_image_free(hdrPixels);

                    TextureOptions options;
                    options.width = static_cast<uint32_t>(width);
                    options.height = static_cast<uint32_t>(height);
                    options.format = PixelFormat::PIXELFORMAT_RGBA32F;
                    options.mipmaps = _data.mipmaps;
                    options.numLevels = _data.mipmaps ? 0 : 1;
                    options.name = _name;

                    auto* texture = new Texture(graphicsDevice.get(), options);
                    texture->setEncoding(TextureEncoding::Default);
                    const auto dataSize = pixelCount * 4 * sizeof(float);
                    texture->setLevelData(0, reinterpret_cast<const uint8_t*>(rgbaData.data()), dataSize);
                    texture->upload();

                    spdlog::info("Loaded HDR texture '{}': {}x{} channels={}", _name, width, height, channels);
                    _resources.push_back(texture);
                } else {
                    // LDR path: load as 8-bit RGBA
                    stbi_uc* pixels = stbi_load(_file.c_str(), &width, &height, &channels, STBI_rgb_alpha);
                    if (!pixels || width <= 0 || height <= 0) {
                        spdlog::error("Failed to decode texture asset '{}' from '{}'", _name, _file);
                        if (pixels) {
                            stbi_image_free(pixels);
                        }
                        return std::nullopt;
                    }

                    TextureOptions options;
                    options.width = static_cast<uint32_t>(width);
                    options.height = static_cast<uint32_t>(height);
                    options.format = PixelFormat::PIXELFORMAT_RGBA8;
                    options.mipmaps = _data.mipmaps;
                    options.numLevels = _data.mipmaps ? 0 : 1;
                    options.name = _name;

                    auto* texture = new Texture(graphicsDevice.get(), options);
                    if (_data.type == TextureType::TEXTURETYPE_RGBP) {
                        texture->setEncoding(TextureEncoding::RGBP);
                    } else if (_data.type == TextureType::TEXTURETYPE_RGBM) {
                        texture->setEncoding(TextureEncoding::RGBM);
                    } else {
                        texture->setEncoding(TextureEncoding::Default);
                    }
                    const auto dataSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
                    texture->setLevelData(0, reinterpret_cast<const uint8_t*>(pixels), dataSize);
                    texture->upload();

                    stbi_image_free(pixels);
                    _resources.push_back(texture);
                }
            } else if (_type == AssetType::FONT) {
                auto graphicsDevice = _defaultGraphicsDevice.lock();
                if (!graphicsDevice) {
                    spdlog::error("Cannot load font asset '{}': no graphics device set", _name);
                    return std::nullopt;
                }

                const auto font = loadBitmapFontResource(_file, graphicsDevice);
                if (!font.has_value()) {
                    spdlog::error("Failed to decode bitmap font asset '{}' from '{}'", _name, _file);
                    return std::nullopt;
                }
                _resources.push_back(*font);
            }
        }

        if (!_resources.empty()) {
            return _resources[0];
        }
        return std::nullopt;
    }

    void Asset::loadAsync(ResourceLoader& loader,
                          std::function<void(std::optional<Resource>)> callback)
    {
        // If already loaded, invoke callback immediately.
        if (!_resources.empty()) {
            spdlog::info("Asset::loadAsync '{}' already loaded — returning cached", _name);
            if (callback) callback(_resources[0]);
            return;
        }

        auto graphicsDevice = _defaultGraphicsDevice.lock();
        if (!graphicsDevice) {
            spdlog::error("Cannot async-load asset '{}': no graphics device set", _name);
            if (callback) callback(std::nullopt);
            return;
        }

        // Map asset type → handler type key used by ResourceLoader.
        // (Asset types and handler keys deliberately share the same strings.)
        const std::string& handlerType = _type;

        // Capture fields by value so the closure is self-contained.
        // `this` is captured for storing the result in _resources.
        // SAFETY: the Asset must outlive the in-flight request.
        auto* self   = this;
        auto  name   = _name;
        auto  type   = _type;
        auto  file   = _file;
        auto  data   = _data;
        auto  device = graphicsDevice;

        spdlog::info("Asset::loadAsync queuing '{}' type '{}'", _name, _type);

        loader.load(_file, handlerType,
            // ── onSuccess (main thread) ────────────────────────────────────
            [self, name, type, file, data, device, callback](std::unique_ptr<LoadedData> loaded) {
                if (type == AssetType::TEXTURE && loaded->pixelData) {
                    // GPU upload from pre-decoded pixels.
                    auto& pd = *loaded->pixelData;

                    TextureOptions options;
                    options.width    = static_cast<uint32_t>(pd.width);
                    options.height   = static_cast<uint32_t>(pd.height);
                    options.format   = pd.isHdr ? PixelFormat::PIXELFORMAT_RGBA32F
                                                : PixelFormat::PIXELFORMAT_RGBA8;
                    options.mipmaps  = data.mipmaps;
                    options.numLevels = data.mipmaps ? 0 : 1;
                    options.name     = name;

                    auto* texture = new Texture(device.get(), options);
                    if (data.type == TextureType::TEXTURETYPE_RGBP) {
                        texture->setEncoding(TextureEncoding::RGBP);
                    } else if (data.type == TextureType::TEXTURETYPE_RGBM) {
                        texture->setEncoding(TextureEncoding::RGBM);
                    } else {
                        texture->setEncoding(TextureEncoding::Default);
                    }

                    if (pd.isHdr) {
                        texture->setLevelData(0,
                            reinterpret_cast<const uint8_t*>(pd.hdrPixels.data()),
                            pd.hdrPixels.size() * sizeof(float));
                    } else {
                        texture->setLevelData(0, pd.pixels.data(), pd.pixels.size());
                    }
                    texture->upload();

                    spdlog::info("Asset::loadAsync texture '{}' GPU upload done {}x{}",
                        name, pd.width, pd.height);

                    self->_resources.push_back(texture);
                    if (callback) callback(texture);

                } else if (type == AssetType::CONTAINER) {
                    std::unique_ptr<GlbContainerResource> container;

                    // Fast path: if the background thread already did FULL
                    // CPU pre-processing (Draco, vertex extraction, tangent
                    // gen, pixel conversion, animation parse), only create
                    // GPU resources here — this is fast and avoids the
                    // main-thread stall that caused the spinning-wait cursor.
                    if (loaded->preparsed && loaded->preparedData) {
                        auto model = std::static_pointer_cast<tinygltf::Model>(loaded->preparsed);
                        auto prepared = std::static_pointer_cast<PreparedGlbData>(loaded->preparedData);
                        container = GlbParser::createFromPrepared(*model, std::move(*prepared), device, name);
                    } else if (loaded->preparsed) {
                        // Medium path: model pre-parsed but CPU work not done.
                        auto model = std::static_pointer_cast<tinygltf::Model>(loaded->preparsed);
                        container = GlbParser::createFromModel(*model, device, name);
                    } else {
                        // Slow fallback: pre-parse wasn't done (non-GLB format,
                        // or bg parse failed).
                        const bool isGlb = file.size() >= 4 &&
                            (file.compare(file.size() - 4, 4, ".glb") == 0 ||
                             file.compare(file.size() - 4, 4, ".GLB") == 0);

                        const bool isGltf = file.size() >= 5 &&
                            (file.compare(file.size() - 5, 5, ".gltf") == 0 ||
                             file.compare(file.size() - 5, 5, ".GLTF") == 0);

                        if ((isGlb || isGltf) && !loaded->bytes.empty()) {
                            container = GlbParser::parseFromMemory(
                                loaded->bytes.data(), loaded->bytes.size(), device, name);
                        } else {
                            // OBJ / STL / Assimp — these parsers need file
                            // paths (their libraries read from disk directly).
                            // The bg thread pre-read the bytes to warm cache.
                            const bool isObj = file.size() >= 4 &&
                                (file.compare(file.size() - 4, 4, ".obj") == 0 ||
                                 file.compare(file.size() - 4, 4, ".OBJ") == 0);

                            const bool isStl = file.size() >= 4 &&
                                (file.compare(file.size() - 4, 4, ".stl") == 0 ||
                                 file.compare(file.size() - 4, 4, ".STL") == 0);

                            const bool isAssimp = file.size() >= 4 &&
                                (file.compare(file.size() - 4, 4, ".dae") == 0 ||
                                 file.compare(file.size() - 4, 4, ".DAE") == 0 ||
                                 file.compare(file.size() - 4, 4, ".fbx") == 0 ||
                                 file.compare(file.size() - 4, 4, ".FBX") == 0 ||
                                 file.compare(file.size() - 4, 4, ".3ds") == 0 ||
                                 file.compare(file.size() - 4, 4, ".ply") == 0 ||
                                 file.compare(file.size() - 4, 4, ".PLY") == 0);

                            if (isObj) {
                                container = ObjParser::parse(file, device);
                            } else if (isStl) {
                                container = StlParser::parse(file, device);
                            } else if (isAssimp) {
                                container = AssimpParser::parse(file, device);
                            } else {
                                // Fallback: try GlbParser from memory.
                                if (!loaded->bytes.empty()) {
                                    container = GlbParser::parseFromMemory(
                                        loaded->bytes.data(), loaded->bytes.size(),
                                        device, name);
                                }
                            }
                        }
                    }

                    if (container) {
                        auto* res = static_cast<ContainerResource*>(container.release());
                        self->_resources.push_back(res);
                        spdlog::info("Asset::loadAsync container '{}' parse done", name);
                        if (callback) callback(res);
                    } else {
                        spdlog::error("Asset::loadAsync container '{}' parse failed", name);
                        if (callback) callback(std::nullopt);
                    }

                } else if (type == AssetType::FONT) {
                    // Font parsing still uses the file path (loadBitmapFontResource).
                    // The bg thread pre-read the bytes to warm the OS cache.
                    const auto font = loadBitmapFontResource(file, device);
                    if (font.has_value()) {
                        self->_resources.push_back(*font);
                        spdlog::info("Asset::loadAsync font '{}' parse done", name);
                        if (callback) callback(*font);
                    } else {
                        spdlog::error("Asset::loadAsync font '{}' parse failed", name);
                        if (callback) callback(std::nullopt);
                    }

                } else {
                    spdlog::warn("Asset::loadAsync '{}' unsupported type '{}'", name, type);
                    if (callback) callback(std::nullopt);
                }
            },
            // ── onError (main thread) ──────────────────────────────────────
            [name, callback](const std::string& error) {
                spdlog::error("Asset::loadAsync '{}' failed: {}", name, error);
                if (callback) callback(std::nullopt);
            }
        );
    }
}
