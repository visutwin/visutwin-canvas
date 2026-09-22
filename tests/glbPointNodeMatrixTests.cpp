// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// glTF node MATRIX on the merged point-cloud path.
//
// GlbParser::parse bakes every static POINTS primitive into one world-space
// buffer, composing each node's local matrix from either its TRS triple or its
// explicit `matrix`. The matrix branch used to write the sixteen column-major
// values through setElement with (row, col) swapped, so a point-cloud node
// authored as a matrix landed TRANSPOSED — its rotation inverted — while the
// same node authored as TRS was right. Found on 2026-09-22 by an audit of
// hand-written matrix code.
//
// The model is built in memory with tinygltf, written as a GLB to a temporary
// file (only the file-based parse reaches the merge), and parsed twice: once
// with the node as `matrix`, once as translation + rotation. The merged points
// must agree with each other AND with the closed-form transform, so both
// branches being wrong the same way cannot pass.

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/mesh.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << what << '\n';
        }
    }

    bool near(const float a, const float b)
    {
        return std::fabs(a - b) < 1e-4f;
    }

    /// Keeps the CPU copy the base class makes so the test can read the merged points back.
    class StubVertexBuffer final : public VertexBuffer
    {
    public:
        using VertexBuffer::VertexBuffer;
        void unlock() override {}
    };

    /// Creates nothing but vertex buffers; the model has no images or index data.
    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format, int numVertices,
            const VertexBufferOptions& options) override
        {
            return std::make_shared<StubVertexBuffer>(this, format, numVertices, options);
        }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    // Three asymmetric points, one per axis, so no rotation but the identity
    // maps the set onto itself.
    const std::vector<float> kPoints = {1.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 3.0f};

    // A quarter turn about +Y (x -> -z, z -> x) then a translation of (1, 2, 3).
    const float kHalfSqrt2 = std::sqrt(0.5f);
    const std::vector<double> kRotation = {0.0, kHalfSqrt2, 0.0, kHalfSqrt2};
    const std::vector<double> kTranslation = {1.0, 2.0, 3.0};
    // The same transform as a column-major matrix: columns are R's basis vectors, then T.
    const std::vector<double> kMatrix = {
        0.0, 0.0, -1.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        1.0, 0.0, 0.0, 0.0,
        1.0, 2.0, 3.0, 1.0,
    };

    // Closed form: p' = R p + T.
    const std::vector<float> kExpected = {
        1.0f, 2.0f, 2.0f, // (1,0,0) -> (0,0,-1) + T
        1.0f, 4.0f, 3.0f, // (0,2,0) -> (0,2,0) + T
        4.0f, 2.0f, 3.0f, // (0,0,3) -> (3,0,0) + T
    };

    tinygltf::Model buildModel(const bool asMatrix)
    {
        tinygltf::Model model;
        model.asset.version = "2.0";

        tinygltf::Buffer buffer;
        buffer.data.resize(kPoints.size() * sizeof(float));
        std::memcpy(buffer.data.data(), kPoints.data(), buffer.data.size());
        model.buffers.push_back(buffer);

        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = 0;
        view.byteLength = buffer.data.size();
        model.bufferViews.push_back(view);

        tinygltf::Accessor accessor;
        accessor.bufferView = 0;
        accessor.byteOffset = 0;
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        accessor.count = 3;
        accessor.type = TINYGLTF_TYPE_VEC3;
        accessor.minValues = {0.0, 0.0, 0.0};
        accessor.maxValues = {1.0, 2.0, 3.0};
        model.accessors.push_back(accessor);

        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_POINTS;
        primitive.attributes["POSITION"] = 0;
        tinygltf::Mesh mesh;
        mesh.name = "cloud";
        mesh.primitives.push_back(primitive);
        model.meshes.push_back(mesh);

        tinygltf::Node node;
        node.name = "cloudNode";
        node.mesh = 0;
        if (asMatrix) {
            node.matrix = kMatrix;
        } else {
            node.rotation = kRotation;
            node.translation = kTranslation;
        }
        model.nodes.push_back(node);

        tinygltf::Scene scene;
        scene.nodes = {0};
        model.scenes.push_back(scene);
        model.defaultScene = 0;
        return model;
    }

    /// Writes the model as a GLB and parses it through the file path, which is the
    /// only entry that reaches the point-cloud merge. Returns the merged positions.
    std::vector<float> mergedPositions(const bool asMatrix, const std::shared_ptr<GraphicsDevice>& device,
        const std::string& label)
    {
        tinygltf::Model model = buildModel(asMatrix);
        const std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("visutwin-glb-point-node-" + label + ".glb");

        tinygltf::TinyGLTF writer;
        const bool written = writer.WriteGltfSceneToFile(&model, path.string(),
            /*embedImages*/ false, /*embedBuffers*/ true, /*prettyPrint*/ false, /*writeBinary*/ true);
        check(written, label + ": the model writes as a GLB");
        if (!written) {
            return {};
        }

        auto container = GlbParser::parse(path.string(), device);
        std::filesystem::remove(path);
        check(container != nullptr, label + ": the GLB parses");
        if (!container) {
            return {};
        }

        // One payload: the merged point cloud (a static POINTS-only mesh is consumed whole).
        check(container->meshPayloads().size() == 1, label + ": the points merge into one payload");
        std::vector<float> positions;
        for (const auto& payload : container->meshPayloads()) {
            const auto vb = payload.mesh ? payload.mesh->getVertexBuffer() : nullptr;
            if (!vb) {
                continue;
            }
            const std::vector<uint8_t>& bytes = vb->storage();
            const int stride = vb->format()->size();
            check(stride == 7 * static_cast<int>(sizeof(float)), label + ": point vertex is xyz + rgba");
            for (int i = 0; i < vb->numVertices(); ++i) {
                float xyz[3];
                std::memcpy(xyz, bytes.data() + static_cast<size_t>(i) * static_cast<size_t>(stride), sizeof(xyz));
                positions.insert(positions.end(), xyz, xyz + 3);
            }
        }
        check(positions.size() == kExpected.size(), label + ": all three points survive the merge");
        return positions;
    }

    void checkAgainstExpected(const std::vector<float>& positions, const std::string& label)
    {
        if (positions.size() != kExpected.size()) {
            return;
        }
        for (size_t i = 0; i < kExpected.size(); ++i) {
            check(near(positions[i], kExpected[i]),
                label + ": component " + std::to_string(i) + " is " + std::to_string(positions[i]) +
                    ", expected " + std::to_string(kExpected[i]));
        }
    }
}

int main()
{
    auto device = std::make_shared<StubDevice>();

    const std::vector<float> fromMatrix = mergedPositions(true, device, "matrix");
    const std::vector<float> fromTrs = mergedPositions(false, device, "trs");

    checkAgainstExpected(fromTrs, "TRS node");
    checkAgainstExpected(fromMatrix, "matrix node");

    if (fromMatrix.size() == fromTrs.size()) {
        for (size_t i = 0; i < fromMatrix.size(); ++i) {
            check(near(fromMatrix[i], fromTrs[i]),
                "matrix and TRS spellings of one node agree at component " + std::to_string(i));
        }
    }

    if (failures == 0) {
        std::cout << "glb-point-node-matrix: all checks passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
