// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026
//
// The glTF / GLB parser: loading a model, prepareFromModel (worker-safe) and createFromPrepared
// (main thread). The helpers live in glbParser<Part>.cpp, shared through glbParserInternal.h.
//
// Every load path ends in prepareFromModel() + createFromPrepared(): parse() and
// parseFromMemory() load a model and call createFromModel(), which runs both
// halves on the calling thread, and the asset loader runs the first half on a
// worker. Texture creation, vertex extraction and node building live once, in the
// helpers: per-path copies drift, and no example loads asynchronously to notice.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE

#include "glbParserInternal.h"

#include "glbParser.h"
#include "framework/parsers/packedVertex.h"

#include "framework/components/light/lightComponent.h"

#include <algorithm>
#include <atomic>
#include <numbers>
#include <mutex>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <meshoptimizer.h>

#include "core/shape/boundingBox.h"
#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "core/math/vector3.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/textureMips.h"
#include "platform/graphics/textureUtils.h"
#include "platform/graphics/vertexFormat.h"
#include "framework/parsers/texture/ktx2Transcoder.h"
#include "framework/parsers/texture/webpDecoder.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    using namespace gltf;

    // A glTF camera: perspective yfov in radians
    // to degrees, orthographic ymag as the half height, a manual aspect only where
    // the file gives one (perspective aspectRatio, orthographic xmag / ymag), and the
    // far plane only when present — glTF's infinite perspective has none.
    static GlbCameraPayload gltfCameraPayload(const tinygltf::Camera& camera)
    {
        GlbCameraPayload payload;
        if (camera.type == "orthographic") {
            const auto& ortho = camera.orthographic;
            payload.projection = ProjectionType::Orthographic;
            payload.nearClip = static_cast<float>(ortho.znear);
            if (ortho.zfar > 0.0) {
                payload.farClip = static_cast<float>(ortho.zfar);
            }
            payload.orthoHeight = static_cast<float>(ortho.ymag);
            if (ortho.xmag != 0.0 && ortho.ymag != 0.0) {
                payload.aspectRatio = static_cast<float>(ortho.xmag / ortho.ymag);
            }
        } else {
            const auto& perspective = camera.perspective;
            payload.projection = ProjectionType::Perspective;
            payload.nearClip = static_cast<float>(perspective.znear);
            if (perspective.zfar > 0.0) {
                payload.farClip = static_cast<float>(perspective.zfar);
            }
            payload.fovDegrees = static_cast<float>(perspective.yfov * 180.0 / std::numbers::pi);
            if (perspective.aspectRatio > 0.0) {
                payload.aspectRatio = static_cast<float>(perspective.aspectRatio);
            }
        }
        return payload;
    }

    // A KHR_lights_punctual light. "point" is an
    // omni light, cone angles go from radians to degrees (defaults 0 and 45), an
    // absent range becomes 9999 (infinity would poison the bounds), the falloff is
    // inverse-squared. The file's intensity is photometric (candela, lux), so it is
    // stored twice: as the LUMINANCE, times the unit conversion, which a scene with
    // physical units shines with (luminance / conversion gives the file's value
    // back), and CLAMPED to [0, 2] as the intensity everything else uses. The colour
    // is taken as the file gives it.
    // DEVIATION: an intensity the file leaves out is its spec default of 1, where
    // upstream leaves the luminance at 0 — tinygltf does not say which it was.
    static GlbLightPayload gltfLightPayload(const tinygltf::Light& light)
    {
        GlbLightPayload payload;
        if (light.type == "directional") {
            payload.type = LightType::LIGHTTYPE_DIRECTIONAL;
        } else if (light.type == "spot") {
            payload.type = LightType::LIGHTTYPE_SPOT;
            payload.innerConeDegrees = static_cast<float>(light.spot.innerConeAngle * 180.0 / std::numbers::pi);
            payload.outerConeDegrees = static_cast<float>(light.spot.outerConeAngle * 180.0 / std::numbers::pi);
        } else {
            payload.type = LightType::LIGHTTYPE_OMNI;
        }
        if (light.color.size() >= 3) {
            payload.color = Color(static_cast<float>(light.color[0]), static_cast<float>(light.color[1]),
                static_cast<float>(light.color[2]), 1.0f);
        }
        payload.intensity = std::clamp(static_cast<float>(light.intensity), 0.0f, 2.0f);
        constexpr double degToRad = std::numbers::pi / 180.0;
        payload.luminance = static_cast<float>(light.intensity) * LightComponent::lightUnitConversion(payload.type,
            static_cast<float>(payload.outerConeDegrees * degToRad), static_cast<float>(payload.innerConeDegrees * degToRad));
        if (light.range > 0.0) {
            payload.range = static_cast<float>(light.range);
        }
        return payload;
    }

    static void warnUnsupportedRequiredExtensions(const tinygltf::Model& model, const std::string& debugName)
    {
        // What the parser actually acts on. The texture-container extensions are
        // here because the parser resolves the image index through them; whether
        // the image PAYLOAD decodes is a separate question the image loader
        // answers with its own per-image warning.
        static const std::set<std::string> supported = {
            "EXT_mesh_gpu_instancing",
            "EXT_meshopt_compression",
            "KHR_draco_mesh_compression",
            "KHR_gaussian_splatting",
            "KHR_lights_punctual",
            "KHR_materials_anisotropy",
            "KHR_materials_clearcoat",
            "KHR_materials_diffuse_transmission",
            "KHR_materials_dispersion",
            "KHR_materials_emissive_strength",
            "KHR_materials_ior",
            "KHR_materials_iridescence",
            "KHR_materials_pbrSpecularGlossiness",
            "KHR_materials_sheen",
            "KHR_materials_specular",
            "KHR_materials_transmission",
            "KHR_materials_unlit",
            "KHR_materials_variants",
            "KHR_materials_volume",
            "KHR_mesh_quantization",
            "KHR_meshopt_compression",
            "KHR_texture_basisu",
            "KHR_texture_transform",
            "EXT_texture_avif",
            "EXT_texture_webp",
            "MSFT_texture_dds",
        };

        std::string missing;
        for (const auto& extension : model.extensionsRequired) {
            if (supported.contains(extension)) {
                continue;
            }
            if (!missing.empty()) {
                missing += ", ";
            }
            missing += extension;
        }
        if (!missing.empty()) {
            spdlog::warn("GLB [{}] requires extensions this parser does not implement: {}. "
                "The file will load, but not as its author intended.", debugName, missing);
        }
    }

    std::unique_ptr<GlbContainerResource> GlbParser::parse(const std::string& path,
        const std::shared_ptr<GraphicsDevice>& device)
    {
        if (!device) {
            spdlog::error("GLB parse failed: graphics device is null");
            return nullptr;
        }

        // Detect file format: .gltf (JSON text) vs .glb (binary)
        const auto dot = path.rfind('.');
        const bool isAscii = (dot != std::string::npos &&
            (path.substr(dot) == ".gltf" || path.substr(dot) == ".GLTF"));

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            spdlog::error("GLB parse failed [{}]: cannot open file", path);
            return nullptr;
        }
        std::vector<std::uint8_t> bytes(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            spdlog::error("GLB parse failed [{}]: read error", path);
            return nullptr;
        }
        const auto slash = path.find_last_of("/\\");
        const std::string baseDir = slash == std::string::npos ? std::string() : path.substr(0, slash);

        tinygltf::Model model;
        std::string warn;
        std::string err;
        const bool ok = loadModel(model, bytes.data(), bytes.size(), !isAscii, baseDir, err, warn);
        if (!warn.empty()) {
            spdlog::warn("GLB parse warning [{}]: {}", path, warn);
        }
        if (!ok) {
            spdlog::error("GLB parse failed [{}]: {}", path, err);
            return nullptr;
        }
        return createFromModel(model, device, path);
    }

    std::unique_ptr<GlbContainerResource> GlbParser::parseFromMemory(
        const std::uint8_t* data, const std::size_t length,
        const std::shared_ptr<GraphicsDevice>& device,
        const std::string& debugName)
    {
        if (!device) {
            spdlog::error("GLB parseFromMemory failed: graphics device is null");
            return nullptr;
        }
        if (!data || length == 0) {
            spdlog::error("GLB parseFromMemory failed [{}]: empty data", debugName);
            return nullptr;
        }

        tinygltf::Model model;
        std::string warn;
        std::string err;
        const bool ok = loadModel(model, data, length, true, std::string(), err, warn);
        if (!warn.empty()) {
            spdlog::warn("GLB parse warning [{}]: {}", debugName, warn);
        }
        if (!ok) {
            spdlog::error("GLB parseFromMemory failed [{}]: {}", debugName, err);
            return nullptr;
        }

        return createFromModel(model, device, debugName);
    }

    bool GlbParser::loadModel(tinygltf::Model& model, const std::uint8_t* data, const std::size_t length,
        const bool binary, const std::string& baseDir, std::string& err, std::string& warn)
    {
        tinygltf::TinyGLTF loader;
        loader.SetImageLoader(GlbParser::loadImageData, nullptr);

        if (binary) {
            std::optional<std::vector<std::uint8_t>> rewritten;
            if (length >= 20) {
                std::uint32_t jsonLength;
                std::memcpy(&jsonLength, data + 12, sizeof(jsonLength));
                if (20 + static_cast<size_t>(jsonLength) <= length) {
                    const std::string_view json(reinterpret_cast<const char*>(data + 20), jsonLength);
                    if (const auto stripped = withoutMeshoptFallbackBuffers(json, true)) {
                        rewritten = withGlbJson(data, length, *stripped);
                    }
                }
            }
            const auto* bytes = rewritten ? rewritten->data() : data;
            const size_t size = rewritten ? rewritten->size() : length;
            return loader.LoadBinaryFromMemory(&model, &err, &warn, bytes, static_cast<unsigned int>(size), baseDir);
        }

        const std::string_view json(reinterpret_cast<const char*>(data), length);
        const auto stripped = withoutMeshoptFallbackBuffers(json, false);
        const std::string_view text = stripped ? std::string_view(*stripped) : json;
        return loader.LoadASCIIFromString(&model, &err, &warn, text.data(), static_cast<unsigned int>(text.size()),
            baseDir);
    }

    std::unique_ptr<GlbContainerResource> GlbParser::createFromModel(
        tinygltf::Model& model,
        const std::shared_ptr<GraphicsDevice>& device,
        const std::string& debugName)
    {
        if (!device) {
            spdlog::error("GLB createFromModel failed: graphics device is null");
            return nullptr;
        }
        return createFromPrepared(model,
            prepareFromModel(model, device->preferredCompressedRgbaFormat(), debugName), device, debugName);
    }

    // ── prepareFromModel: the CPU-heavy half, safe on a worker thread ────

    PreparedGlbData GlbParser::prepareFromModel(tinygltf::Model& model,
        const PixelFormat ktx2TargetFormat, const std::string& debugName)
    {
        warnUnsupportedRequiredExtensions(model, debugName.empty() ? "glTF" : debugName);
        // Before anything reads a buffer view.
        decodeMeshoptBufferViews(model, debugName.empty() ? "glTF" : debugName);

        PreparedGlbData result;

        // ── Images: RGBA8, or transcoded KTX2 ────────────────────────
        // All of them at once, one per thread: decoding is most of the time a textured
        // model takes to load, and no image depends on another.
        result.images.resize(model.images.size());
        // An image is colour when a colour map's texture shows it.
        std::vector<bool> colourImages(model.images.size(), false);
        {
            const std::vector<bool> colourTextures = gltfColourTextures(model);
            for (size_t t = 0; t < model.textures.size(); ++t) {
                const int source = textureImageSource(model.textures[t]);
                if (colourTextures[t] && source >= 0 && source < static_cast<int>(colourImages.size())) {
                    colourImages[static_cast<size_t>(source)] = true;
                }
            }
        }
        // The mip chain of an RGBA8 image, on its decoding thread.
        const auto buildMips = [&colourImages](const size_t i, PreparedGlbData::ImageData& img) {
            img.srgb = colourImages[i];
            if (img.valid && !img.isCompressed && img.width > 0 && img.height > 0) {
                const auto w = static_cast<uint32_t>(img.width);
                const auto h = static_cast<uint32_t>(img.height);
                img.mipLevels = generateMipChain8(img.rgbaPixels.data(), w, h, 4, img.srgb,
                    TextureUtils::calcMipLevelsCount(w, h));
            }
        };
        parallelFor(model.images.size(), [&](const size_t i) {
            auto& img = result.images[i];
            const auto& srcImage = model.images[i];
            if (imageHoldsKtx2(srcImage)) {
                // KHR_texture_basisu: transcode here, off the main thread.
                auto transcoded = Ktx2Transcoder::transcode(srcImage.image.data(),
                    srcImage.image.size(), srcImage.name.empty() ? "ktx2" : srcImage.name,
                    ktx2TargetFormat);
                if (transcoded.valid) {
                    img.isCompressed = true;
                    img.compressedFormat = static_cast<uint32_t>(transcoded.format);
                    img.compressedLevels = std::move(transcoded.levels);
                    img.width = static_cast<int>(transcoded.width);
                    img.height = static_cast<int>(transcoded.height);
                    img.valid = true;
                }
                return;
            }
            if (srcImage.as_is) {
                // Left encoded by loadImageData.
                decodeEncodedImage(srcImage, i, img);
                buildMips(i, img);
                return;
            }
            // Pixels someone else decoded (a model built in memory).
            img.valid = buildRgba8Image(srcImage, img.rgbaPixels);
            if (img.valid) {
                img.width  = srcImage.width;
                img.height = srcImage.height;
                buildMips(i, img);
            } else {
                spdlog::warn("glTF image '{}' unsupported format (bits={}, components={}, pixelType={})",
                    srcImage.name, srcImage.bits, srcImage.component, srcImage.pixel_type);
            }
        });

        // ── Primitives ───────────────────────────────────────────────
        // With animations, POINTS stay per node in local space so animating the node
        // moves the cloud; without, they merge into one world-space draw call.
        result.pointCloudsMerged = model.animations.empty();
        std::vector<Matrix4> nodeWorld;
        std::vector<int> meshToNodeIndex;
        std::vector<PackedPointVertex> mergedPoints;
        Vector3 mergedMin(std::numeric_limits<float>::max());
        Vector3 mergedMax(std::numeric_limits<float>::lowest());
        int mergedMaterialIndex = -1;

        result.meshPrimitives.resize(model.meshes.size());
        result.meshSplats.resize(model.meshes.size());
        for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
            const auto& mesh = model.meshes[meshIndex];
            auto& primResults = result.meshPrimitives[meshIndex];

            for (const auto& primitive : mesh.primitives) {
                // A splat primitive is POINTS too, but it is a gaussian splat set, not a
                // point cloud: it never reaches the point path or the merge.
                if (isGaussianSplatPrimitive(primitive)) {
                    if (auto splats = gaussianSplatData(model, primitive, debugName)) {
                        result.meshSplats[meshIndex].push_back(std::move(splats));
                    }
                    continue;
                }
                if (primitive.mode == TINYGLTF_MODE_POINTS) {
                    const auto* positions = readablePositions(model, primitive);
                    if (!positions) {
                        continue;
                    }
                    const auto* colors = primitiveAttribute(model, primitive, "COLOR_0");
                    if (!result.pointCloudsMerged) {
                        std::vector<PackedPointVertex> points;
                        Vector3 pointsMin(std::numeric_limits<float>::max());
                        Vector3 pointsMax(std::numeric_limits<float>::lowest());
                        appendPointVertices(model, *positions, colors, nullptr, points, pointsMin, pointsMax);
                        primResults.push_back(pointCloudData(std::move(points), pointsMin, pointsMax,
                            primitive.material >= 0 ? primitive.material : 0));
                        continue;
                    }
                    if (nodeWorld.empty()) {
                        nodeWorld = computeNodeWorldMatrices(model);
                        // The last node referencing a mesh places it.
                        meshToNodeIndex.assign(model.meshes.size(), -1);
                        for (size_t i = 0; i < model.nodes.size(); ++i) {
                            const int meshRef = model.nodes[i].mesh;
                            if (meshRef >= 0 && meshRef < static_cast<int>(model.meshes.size())) {
                                meshToNodeIndex[static_cast<size_t>(meshRef)] = static_cast<int>(i);
                            }
                        }
                    }
                    if (mergedMaterialIndex < 0) {
                        mergedMaterialIndex = primitive.material >= 0 ? primitive.material : 0;
                    }
                    const int nodeIndex = meshToNodeIndex[meshIndex];
                    const Matrix4 world = nodeIndex >= 0 ? nodeWorld[static_cast<size_t>(nodeIndex)] : Matrix4::identity();
                    appendPointVertices(model, *positions, colors, &world, mergedPoints, mergedMin, mergedMax);
                    continue;
                }

                PreparedGlbData::PrimitiveData pd;
                if (extractTrianglePrimitive(model, mesh, primitive, meshIndex, result, pd)) {
                    primResults.push_back(std::move(pd));
                }
            }
        }
        if (!mergedPoints.empty()) {
            result.mergedPoints = pointCloudData(std::move(mergedPoints), mergedMin, mergedMax, mergedMaterialIndex);
        }

        // ── Animations ───────────────────────────────────────────────
        parseAnimations(model, result.animTracks);

        return result;
    }

    // ── createFromPrepared: GPU resource creation on the main thread ─────

    std::unique_ptr<GlbContainerResource> GlbParser::createFromPrepared(
        tinygltf::Model& model,
        PreparedGlbData&& prepared,
        const std::shared_ptr<GraphicsDevice>& device,
        const std::string& debugName)
    {
        if (!device) {
            spdlog::error("GLB createFromPrepared failed: graphics device is null");
            return nullptr;
        }

        auto container = std::make_unique<GlbContainerResource>();
        const auto vertexFormat = std::make_shared<VertexFormat>(
            sizeof(PackedVertex), VertexFormat::standardElements(), true, false);
        const auto skinnedVertexFormat = std::make_shared<VertexFormat>(
            static_cast<int>(SKINNED_VERTEX_STRIDE), VertexFormat::skinnedElements(), true, false);
        const auto pointVertexFormat = std::make_shared<VertexFormat>(
            static_cast<int>(sizeof(PackedPointVertex)), VertexFormat::pointElements(), true, false);
        auto coloredElements = VertexFormat::standardElements();
        coloredElements.push_back({VertexSemantic::SEMANTIC_COLOR, VertexDataType::TYPE_FLOAT32, 4,
            static_cast<uint32_t>(sizeof(PackedVertex))});
        const auto coloredVertexFormat = std::make_shared<VertexFormat>(
            static_cast<int>(COLORED_VERTEX_STRIDE), std::move(coloredElements), true, false);

        // ── Textures, created on first reference by a material ───────
        // Sampling state lives on the Texture, so an image used with several states needs
        // a Texture (and GPU memory) for each; glTF textures using one image with one
        // resolved state share it.
        std::vector<std::shared_ptr<Texture>> gltfTextures(model.textures.size());
        std::map<std::pair<int, GltfSamplerState>, std::shared_ptr<Texture>> imageTextures;
        auto getOrCreateTexture = [&](const int textureIndex) -> std::shared_ptr<Texture> {
            if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size())) {
                return nullptr;
            }
            auto& cached = gltfTextures[static_cast<size_t>(textureIndex)];
            if (cached) {
                return cached;
            }

            const auto& srcTexture = model.textures[static_cast<size_t>(textureIndex)];
            const int imageSource = textureImageSource(srcTexture);
            if (imageSource < 0 || imageSource >= static_cast<int>(prepared.images.size())) {
                std::string extensions;
                for (const auto& [name, value] : srcTexture.extensions) {
                    extensions += (extensions.empty() ? "" : ", ") + name;
                }
                spdlog::warn("glTF texture {} has no valid image source (source={}, extensions: {})",
                    textureIndex, srcTexture.source, extensions.empty() ? "none" : extensions);
                return nullptr;
            }
            const auto& image = prepared.images[static_cast<size_t>(imageSource)];
            if (!image.valid || (image.rgbaPixels.empty() && !image.isCompressed)) {
                return nullptr;   // the prepare half already said why
            }

            const GltfSamplerState samplerState = resolveGltfSampler(model, srcTexture);
            auto& shared = imageTextures[{imageSource, samplerState}];
            if (shared) {
                cached = shared;
                return cached;
            }

            const auto& srcImage = model.images[static_cast<size_t>(imageSource)];
            auto texture = createPreparedTexture(image, srcImage.name.empty() ? srcTexture.name : srcImage.name,
                device);
            applyGltfSampler(*texture, samplerState);
            texture->upload();
            container->addOwnedTexture(texture);
            shared = texture;
            cached = texture;
            return cached;
        };

        // ── Materials ────────────────────────────────────────────────
        std::vector<std::shared_ptr<Material>> gltfMaterials;
        gltfMaterials.reserve(std::max<size_t>(1, model.materials.size()));
        size_t actualTextureCount = 0;
        for (const auto& srcMaterial : model.materials) {
            gltfMaterials.push_back(createGltfMaterial(srcMaterial, getOrCreateTexture, &actualTextureCount));
        }
        // Last, the default: what a primitive naming no material (or an invalid one) draws
        // with, whether or not the file has materials of its own.
        const size_t defaultMaterialIndex = gltfMaterials.size();
        gltfMaterials.push_back(createDefaultGltfMaterial());

        // A primitive with vertex colours draws with a COPY of its material that
        // compiles the vertex-colour variant: the variant reads the colour attribute,
        // which a mesh without one does not have, so it cannot go on the shared
        // material. One copy per source material, shared by every coloured primitive
        // using it. An invalid index, or none, takes the default material.
        std::vector<std::shared_ptr<Material>> vertexColorMaterials(gltfMaterials.size());
        const auto primitiveMaterial = [&](const int materialIndex, const bool vertexColors) {
            const size_t index = (materialIndex >= 0 && materialIndex < static_cast<int>(defaultMaterialIndex))
                ? static_cast<size_t>(materialIndex) : defaultMaterialIndex;
            if (!vertexColors) {
                return gltfMaterials[index];
            }
            auto& copy = vertexColorMaterials[index];
            if (!copy) {
                auto material = std::make_shared<StandardMaterial>(
                    *std::static_pointer_cast<StandardMaterial>(gltfMaterials[index]));
                material->setShaderVariantKey(material->shaderVariantKey() | (1ull << 21));  // VT_FEATURE_VERTEX_COLORS
                copy = material;
            }
            return copy;
        };

        // ── Meshes ───────────────────────────────────────────────────
        std::vector<std::vector<size_t>> meshToPayloadIndices(model.meshes.size());
        size_t nextPayloadIndex = 0;
        for (size_t meshIndex = 0; meshIndex < prepared.meshPrimitives.size(); ++meshIndex) {
            for (auto& pd : prepared.meshPrimitives[meshIndex]) {
                if (pd.vertexBytes.empty()) {
                    continue;
                }
                const auto& format = pd.pointCloud ? pointVertexFormat
                    : pd.skinned ? skinnedVertexFormat
                    : pd.vertexColors ? coloredVertexFormat : vertexFormat;
                auto mesh = createPreparedMesh(pd, device, format);
                if (!mesh) {
                    spdlog::warn("GLB [{}]: vertex buffer creation failed (mesh {})", debugName, meshIndex);
                    continue;
                }

                GlbMeshPayload payload;
                payload.mesh = mesh;
                if (pd.pointCloud) {
                    payload.material = pointCloudMaterial(gltfMaterials, pd.materialIndex, true);
                    payload.castShadow = false;
                } else {
                    payload.material = primitiveMaterial(pd.materialIndex, pd.vertexColors);
                    // A material drawing a mesh with the file's own tangents lays its
                    // anisotropy on them, as on a mesh whose vertex format has tangents.
                    if (pd.authoredTangents) {
                        if (auto* standard = dynamic_cast<StandardMaterial*>(payload.material.get())) {
                            standard->setMeshTangents(true);
                        }
                    }
                }
                for (const auto& [variant, materialIndex] : pd.variantMaterials) {
                    if (materialIndex >= 0 && materialIndex < static_cast<int>(defaultMaterialIndex)) {
                        payload.variantMaterials[variant] = primitiveMaterial(materialIndex, pd.vertexColors);
                    }
                }
                // Morph deltas were extracted by the prepare half; the GPU buffer is built here.
                if (!pd.morphTargets.empty()) {
                    payload.morph = std::make_shared<Morph>(std::move(pd.morphTargets), pd.vertexCount, device.get());
                    payload.morphInitialWeights = std::move(pd.morphInitialWeights);
                }
                container->addMeshPayload(payload);
                meshToPayloadIndices[meshIndex].push_back(nextPayloadIndex++);
            }
        }

        size_t mergedPointPayloadIndex = SIZE_MAX;
        if (prepared.pointCloudsMerged && prepared.mergedPoints.vertexCount > 0) {
            const int mergedVertexCount = prepared.mergedPoints.vertexCount;
            if (auto mesh = createPreparedMesh(prepared.mergedPoints, device, pointVertexFormat)) {
                GlbMeshPayload payload;
                payload.mesh = mesh;
                payload.material = pointCloudMaterial(gltfMaterials, prepared.mergedPoints.materialIndex, false);
                payload.castShadow = false;  // Points don't cast meaningful shadows.
                container->addMeshPayload(payload);
                mergedPointPayloadIndex = nextPayloadIndex++;
                spdlog::info("GLB merged {} point vertices into 1 draw call (AABB {:.2f}–{:.2f})",
                    mergedVertexCount, prepared.mergedPoints.boundsMin.getX(), prepared.mergedPoints.boundsMax.getX());
            }
        }

        // A leaf node whose mesh was ALL points is fully consumed by the merge: its
        // transform is baked into the merged vertices, so it serves no purpose.
        std::vector<bool> meshFullyConsumed(model.meshes.size(), false);
        if (prepared.pointCloudsMerged) {
            for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
                const auto& primitives = model.meshes[mi].primitives;
                meshFullyConsumed[mi] = !primitives.empty() && std::all_of(primitives.begin(), primitives.end(),
                    [](const tinygltf::Primitive& p) {
                        return p.mode == TINYGLTF_MODE_POINTS && !isGaussianSplatPrimitive(p);
                    });
            }
        }

        // KHR_gaussian_splatting: the decoded splat sets become GPU resources here, one
        // per primitive, shared by every node that uses the mesh.
        std::vector<std::vector<std::shared_ptr<GSplatResource>>> meshSplatResources(model.meshes.size());
        for (size_t meshIndex = 0; meshIndex < prepared.meshSplats.size() && meshIndex < model.meshes.size(); ++meshIndex) {
            for (auto& data : prepared.meshSplats[meshIndex]) {
                if (data && device) {
                    meshSplatResources[meshIndex].push_back(std::make_shared<GSplatResource>(std::move(data), device));
                }
            }
        }

        // KHR_materials_variants: the variant names, by index.
        if (const auto ext = model.extensions.find("KHR_materials_variants");
            ext != model.extensions.end() && ext->second.IsObject() && ext->second.Has("variants")) {
            std::vector<std::string> names;
            const auto& variants = ext->second.Get("variants");
            for (size_t v = 0; variants.IsArray() && v < variants.ArrayLen(); ++v) {
                const auto& variant = variants.Get(static_cast<int>(v));
                names.push_back(variant.IsObject() && variant.Has("name") && variant.Get("name").IsString()
                    ? variant.Get("name").Get<std::string>() : "variant_" + std::to_string(v));
            }
            container->setMaterialVariants(std::move(names));
        }

        // ── Nodes: the glTF hierarchy and local transforms ───────────
        const std::vector<std::string> nodeNames = glbNodeNames(model, glbNodeParents(model));
        for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex) {
            const auto& node = model.nodes[nodeIndex];
            GlbNodePayload nodePayload;
            // Never empty: an unnamed node is `node_<index>`, and a repeated sibling
            // name is made unique; the animation channels and skin entries carry the
            // same names (glbNodeNames).
            nodePayload.name = nodeNames[nodeIndex];
            if (!node.matrix.empty()) {
                decomposeNodeMatrix(node.matrix, nodePayload.translation, nodePayload.rotation, nodePayload.scale);
            }
            if (node.translation.size() == 3) {
                nodePayload.translation = Vector3(static_cast<float>(node.translation[0]),
                    static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2]));
            }
            if (node.rotation.size() == 4) {
                nodePayload.rotation = Quaternion(static_cast<float>(node.rotation[0]),
                    static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]),
                    static_cast<float>(node.rotation[3])).normalized();
            }
            if (node.scale.size() == 3) {
                nodePayload.scale = Vector3(static_cast<float>(node.scale[0]),
                    static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2]));
            }
            if (node.mesh >= 0 && node.mesh < static_cast<int>(meshToPayloadIndices.size())) {
                const auto& mapped = meshToPayloadIndices[static_cast<size_t>(node.mesh)];
                nodePayload.meshPayloadIndices.insert(nodePayload.meshPayloadIndices.end(), mapped.begin(), mapped.end());
                nodePayload.skip = node.children.empty() && meshFullyConsumed[static_cast<size_t>(node.mesh)];
            }
            nodePayload.skinIndex = node.skin;
            nodePayload.children = node.children;
            if (node.mesh >= 0 && node.mesh < static_cast<int>(meshSplatResources.size())) {
                nodePayload.splats = meshSplatResources[static_cast<size_t>(node.mesh)];
            }
            if (node.camera >= 0 && node.camera < static_cast<int>(model.cameras.size())) {
                nodePayload.camera = gltfCameraPayload(model.cameras[static_cast<size_t>(node.camera)]);
            }
            if (node.light >= 0 && node.light < static_cast<int>(model.lights.size())) {
                nodePayload.light = gltfLightPayload(model.lights[static_cast<size_t>(node.light)]);
            }
            if (const auto matrices = gltfInstanceMatrices(model, node); !matrices.empty() && device &&
                !nodePayload.meshPayloadIndices.empty()) {
                const int count = static_cast<int>(matrices.size() / 16);
                VertexBufferOptions options;
                options.data.resize(matrices.size() * sizeof(float));
                std::memcpy(options.data.data(), matrices.data(), options.data.size());
                nodePayload.instanceBuffer = device->createVertexBuffer(VertexFormat::defaultInstancingFormat(), count, options);
                nodePayload.instanceCount = nodePayload.instanceBuffer ? count : 0;
            }
            // A node that carries a camera or a light is not a disposable points leaf.
            if (nodePayload.camera || nodePayload.light) {
                nodePayload.skip = false;
            }
            container->addNodePayload(nodePayload);
        }

        // The merged cloud hangs off a synthetic root node with an identity transform.
        if (mergedPointPayloadIndex != SIZE_MAX) {
            GlbNodePayload pointNode;
            pointNode.name = "__merged_point_cloud";
            pointNode.meshPayloadIndices.push_back(mergedPointPayloadIndex);
            container->addNodePayload(pointNode);
            container->addRootNodeIndex(static_cast<int>(model.nodes.size()));
        }

        int sceneIndex = model.defaultScene;
        if (sceneIndex < 0 && !model.scenes.empty()) {
            sceneIndex = 0;
        }
        if (sceneIndex >= 0 && sceneIndex < static_cast<int>(model.scenes.size())) {
            for (const auto nodeIndex : model.scenes[static_cast<size_t>(sceneIndex)].nodes) {
                container->addRootNodeIndex(nodeIndex);
            }
        }

        // ── Skins and animations ─────────────────────────────────────
        parseSkins(model, container.get());
        for (auto& [name, track] : prepared.animTracks) {
            container->addAnimTrack(name, track);
        }

        if (prepared.dracoPrimitiveCount > 0) {
            spdlog::info("GLB Draco summary [{}]: primitives={}, decoded={}, failed={}",
                debugName, prepared.dracoPrimitiveCount, prepared.dracoDecodeSuccessCount,
                prepared.dracoDecodeFailureCount);
        }
        spdlog::debug("GLB [{}]: textures={} (bound {}), meshes={}, nodes={}",
            debugName, gltfTextures.size(), actualTextureCount, nextPayloadIndex, model.nodes.size());

        return container;
    }
}
