// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2025.
//
#pragma once

#include <functional>
#include <string>
#include <variant>
#include <core/eventHandler.h>
#include <framework/handlers/containerResource.h>
#include <framework/handlers/fontResource.h>
#include <platform/graphics/texture.h>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class ResourceLoader;

    namespace AssetType {
        constexpr const char* ANIMATION = "animation";
        constexpr const char* AUDIO = "audio";
        constexpr const char* BINARY = "binary";
        constexpr const char* CONTAINER = "container";
        constexpr const char* CUBEMAP = "cubemap";
        constexpr const char* CSS = "css";
        constexpr const char* FONT = "font";
        constexpr const char* GSPLAT = "gsplat";
        constexpr const char* JSON = "json";
        constexpr const char* HTML = "html";
        constexpr const char* MATERIAL = "material";
        constexpr const char* MODEL = "model";
        constexpr const char* RENDER = "render";
        constexpr const char* SCRIPT = "script";
        constexpr const char* SHADER = "shader";
        constexpr const char* SPRITE = "sprite";
        constexpr const char* TEMPLATE = "template";
        constexpr const char* TEXT = "text";
        constexpr const char* TEXTURE = "texture";
        constexpr const char* TEXTUREATLAS = "textureatlas";
    }

    // Texture types
    namespace TextureType
    {
        constexpr const char* TEXTURETYPE_DEFAULT = "default";
        constexpr const char* TEXTURETYPE_RGBM = "rgbm";
        constexpr const char* TEXTURETYPE_RGBE = "rgbe";
        constexpr const char* TEXTURETYPE_RGBP = "rgbp";
        constexpr const char* TEXTURETYPE_SWIZZLEGGGR = "swizzleGGGR";
    }

    struct AssetData
    {
        std::string type = TextureType::TEXTURETYPE_DEFAULT;
        bool mipmaps = false;
    };

    using Resource = std::variant<Texture*, ContainerResource*, FontResource*>;

    class Asset : public EventHandler
    {
    public:
        Asset(const std::string& name, const std::string& type, const std::string& file, const AssetData& data = {});

        static void setDefaultGraphicsDevice(const std::shared_ptr<GraphicsDevice>& graphicsDevice);

        bool preload() const { return _preload; }

        const std::string& name() const { return _name; }
        const std::string& type() const { return _type; }
        const std::string& file() const { return _file; }
        const AssetData& data() const { return _data; }

        // Destroys the associated resource and marks asset as unloaded
        void unload();

        /// Synchronous load — blocks until the resource is ready.
        std::optional<Resource> resource();

        /**
         * Asynchronous load — queues I/O on the ResourceLoader's background
         * thread and invokes @p callback on the main thread (during
         * ResourceLoader::processCompletions) once the resource is ready.
         *
         * If the asset is already loaded the callback fires immediately (on
         * the current call, not deferred).
         *
         * @note The Asset must outlive the in-flight request.
         */
        void loadAsync(ResourceLoader& loader,
                       std::function<void(std::optional<Resource>)> callback);

    private:
        static std::weak_ptr<GraphicsDevice> _defaultGraphicsDevice;

        bool _preload;

        std::string _name;
        std::string _type;
        std::string _file;

        AssetData _data;

        std::vector<Resource> _resources;
    };
}
