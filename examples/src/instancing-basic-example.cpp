// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream's graphics/instancing-basic: hardware instancing with a
// StandardMaterial — 1000 randomly placed, randomly scaled cylinders drawn in a
// single call from per-instance model matrices, lit by the helipad env atlas
// alone. The scene matches upstream value for value.
//
// The only deliberate difference is the fixed RNG seed, so the instance layout is
// reproducible for screenshot comparison; upstream reseeds from Math.random().
//
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// Number of instances to render
constexpr int instanceCount = 1000;

class InstancingBasicExample final: public ExampleApp
{
public:
    InstancingBasicExample(): ExampleApp({.title = "Instancing Basic"}) {}

protected:
    bool create() override
    {
        spdlog::info("*** Instancing-Basic Example ***");

        // setup skydome — JS: app.scene.skyboxMip = 2; app.scene.exposure = 0.3;
        scene()->setSkyboxMip(2);
        scene()->setExposure(0.3f);
        scene()->setAmbientLight(0.1f, 0.1f, 0.1f);

        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        const auto helipadResource = _helipad->resource();
        if (!helipadResource) {
            spdlog::error("Failed to load helipad texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        // Create an Entity with a camera component, moved back to see the cylinders.
        // JS: camera.addComponent('camera', { toneMapping: TONEMAP_ACES })
        _camera = createCamera(Vector3(0.0f, 0.0f, 10.0f));
        if (auto* cameraComp = _camera->findComponent<CameraComponent>()) {
            cameraComp->setToneMapping(TONEMAP_ACES);
        }

        // Create standard material — instancing needs nothing from the material, the
        // instanced shader variant follows the mesh instance's per-instance buffer.
        // JS: material.gloss = 0.6; material.metalness = 0.7; material.useMetalness = true;
        _material = std::make_shared<StandardMaterial>();
        _material->setGloss(0.6f);
        _material->setMetalness(0.7f);
        _material->setUseMetalness(true);

        // Create a Entity with a cylinder render component and the instancing material
        auto* cylinder = createPrimitive("cylinder", _material.get());
        cylinder->setName("InstancingEntity");
        auto* renderComp = cylinder->findComponent<RenderComponent>();

        // ── Build per-instance data buffer ─────────────────────────────
        //
        // JS: const matrices = new Float32Array(instanceCount * 16)
        // One column-major float4x4 model matrix per instance; base color comes from the
        // material, as in any other draw (VertexFormat::defaultInstancingFormat below).
        constexpr int kInstanceDataBytes = VertexFormat::INSTANCING_MATRIX_SIZE;

        std::vector<uint8_t> instanceBytes(static_cast<size_t>(instanceCount) * kInstanceDataBytes);

        std::mt19937 rng(42);  // fixed seed for deterministic layout
        std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

        constexpr float radius = 5.0f;

        for (int i = 0; i < instanceCount; ++i) {
            // Generate random positions, scales and rotations — matching JS:
            // pos.set(random * radius - radius * 0.5, ...)
            const float px = dist01(rng) * radius - radius * 0.5f;
            const float py = dist01(rng) * radius - radius * 0.5f;
            const float pz = dist01(rng) * radius - radius * 0.5f;

            // JS: scl.set(0.1 + random * 0.1, 0.1 + random * 0.3, 0.1 + random * 0.1)
            const float sx = 0.1f + dist01(rng) * 0.1f;
            const float sy = 0.1f + dist01(rng) * 0.3f;
            const float sz = 0.1f + dist01(rng) * 0.1f;

            // JS: rot.setFromEulerAngles(i * 30, i * 50, i * 70)
            const auto rot = Quaternion::fromEulerAngles(
                static_cast<float>(i) * 30.0f,
                static_cast<float>(i) * 50.0f,
                static_cast<float>(i) * 70.0f
            );

            const auto pos = Vector3(px, py, pz);
            const auto scl = Vector3(sx, sy, sz);

            // Build TRS matrix
            const auto matrix = Matrix4::trs(pos, rot, scl);

            // Copy matrix elements into the instance buffer (JS copies matrix.data)
            auto* dst = instanceBytes.data() + static_cast<size_t>(i) * kInstanceDataBytes;
            std::memcpy(dst, &matrix, kInstanceDataBytes);
        }

        // Create static vertex buffer containing the matrices
        // JS: VertexFormat.getDefaultInstancingFormat(app.graphicsDevice)
        auto instanceFormat = VertexFormat::defaultInstancingFormat();
        VertexBufferOptions vbOptions;
        vbOptions.data = std::move(instanceBytes);
        auto instanceBuffer = device()->createVertexBuffer(instanceFormat, instanceCount, vbOptions);

        // Initialize instancing using the vertex buffer on meshInstance of the created cylinder
        if (renderComp && !renderComp->meshInstances().empty()) {
            auto* cylinderMeshInst = renderComp->meshInstances()[0];
            if (cylinderMeshInst) {
                cylinderMeshInst->setInstancing(instanceBuffer, instanceCount);
                spdlog::info("Instancing enabled: {} instances", instanceCount);
            }
        } else {
            spdlog::warn("No mesh instances found on cylinder render component");
        }

        spdlog::info("Instancing-Basic: {} cylinders in one draw call. ESC to exit.", instanceCount);
        return true;
    }

    void update(const float dt) override
    {
        // Orbit camera around — JS: angle += dt * 0.2;
        _angle += dt * 0.2f;
        _camera->setLocalPosition(
            8.0f * std::sin(_angle),
            0.0f,
            8.0f * std::cos(_angle)
        );

        // JS: camera.lookAt(Vec3.ZERO)
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
    }

    void destroy() override
    {
        spdlog::info("*** Instancing-Basic Example Finished ***");
    }

private:
    std::unique_ptr<Asset> _helipad;
    std::shared_ptr<StandardMaterial> _material;
    Entity* _camera = nullptr;
    float _angle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(InstancingBasicExample)
