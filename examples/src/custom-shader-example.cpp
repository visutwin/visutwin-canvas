// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Custom shader material example — port of upstream shaders/shader-toon.
// A ShaderMaterial carrying a user-supplied toon shader (quantised N·L into 6 bands
// over a single warm-grey ramp) replaces the materials of every mesh instance in the
// loaded statue model, which rotates at 60°/s.
//
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/materials/shaderMaterial.h"

using namespace visutwin::canvas;

// Self-contained toon shader. It declares the same vertex-input attributes and
// per-draw uniform buffers the forward pass binds (SceneData @1, ModelData @2), plus
// its own custom block at @3 in place of MaterialData — see ToonMaterial below.
static const char* kToonShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexData {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct SceneData { float4x4 projViewMatrix; };

struct ModelData {
    float4x4 modelMatrix;
    float4x4 normalMatrix;
    float     normalSign;
    float3    _pad;
};

// Replaces MaterialData at buffer(3) — the port's equivalent of upstream's
// material.setParameter('uLightPos', ...).
struct ToonData { float4 lightPos; };

struct Varyings {
    float4 position [[position]];
    float  vertOutTexCoord;
    float2 texCoord;
};

vertex Varyings vertexShader(VertexData v [[stage_in]],
                             constant SceneData &scene [[buffer(1)]],
                             constant ModelData &model [[buffer(2)]],
                             constant ToonData  &toon  [[buffer(3)]])
{
    Varyings out;
    float4 world = model.modelMatrix * float4(v.position, 1.0);
    float3 worldNormal = normalize((model.normalMatrix * float4(v.normal, 0.0)).xyz) * model.normalSign;

    // Vector to the light source.
    float3 lightDir = normalize(toon.lightPos.xyz - world.xyz);

    // Dot product gives us diffuse intensity, used as the 1D colour ramp
    // coordinate in the fragment shader.
    out.vertOutTexCoord = max(0.0, dot(worldNormal, lightDir));
    out.texCoord = v.uv0;

    float4 clip = scene.projViewMatrix * world;
    clip.z = 0.5 * (clip.z + clip.w);   // GL [-1,1] -> Metal [0,1]
    out.position = clip;
    return out;
}

fragment float4 fragmentShader(Varyings in [[stage_in]])
{
    float v = in.vertOutTexCoord;
    v = float(int(v * 6.0)) / 6.0;
    float3 linearColor = float3(0.218, 0.190, 0.156) * v;
    // gammaCorrectOutput: nothing else in the pipeline encodes a custom shader's output.
    return float4(pow(linearColor + 0.0000001, float3(1.0 / 2.2)), 1.0);
}
)MSL";

// The same toon shader in GLSL, for the Vulkan backend.
//
// Differences from the MSL above, all forced by the backend rather than by choice:
//  - transforms arrive in a 128-byte vertex push constant (viewProjection, model),
//    not in buffer(1)/buffer(2) uniform blocks;
//  - there is no normalMatrix/normalSign, so the normal is transformed by mat3(model),
//    exactly as engine/shaders/vulkan/forward.vert does;
//  - the custom uniform block is set 0 / binding 0 (the per-draw material UBO, bound
//    to both stages) rather than buffer(3);
//  - the clip.z remap is MANDATORY. Engine projections are GL-style (NDC z in [-1,1])
//    while Vulkan clips to [0,w], so forward.vert (and particle.vert / gsplat.vert)
//    all apply `z = 0.5 * (z + w)`. A custom shader that skips it stores roughly half
//    the depth every other draw stores, and then wrongly wins depth tests against all
//    standard-material geometry — verified 2026-08-21 with a box that the statue
//    occluded on Vulkan but correctly intersected on Metal.
static const char* kToonShaderSourceGlsl = R"GLSL(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
} pc;

layout(set = 0, binding = 0) uniform ToonData {
    vec4 uLightPos;
} toon;

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec3 vertexNormal;
layout(location = 0) out float vertOutTexCoord;
void main() {
    vec4 world = pc.model * vec4(vertexPosition, 1.0);
    vec3 worldNormal = normalize(mat3(pc.model) * vertexNormal);
    vec3 lightDir = normalize(toon.uLightPos.xyz - world.xyz);
    vertOutTexCoord = max(0.0, dot(worldNormal, lightDir));
    vec4 clip = pc.viewProjection * world;
    clip.z = 0.5 * (clip.z + clip.w);   // GL [-1,1] -> Vulkan [0,1], as forward.vert does
    gl_Position = clip;
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(location = 0) in float vertOutTexCoord;
layout(location = 0) out vec4 fragColor;
void main() {
    float v = vertOutTexCoord;
    v = float(int(v * 6.0)) / 6.0;
    vec3 linearColor = vec3(0.218, 0.190, 0.156) * v;
    fragColor = vec4(pow(linearColor + 0.0000001, vec3(1.0 / 2.2)), 1.0);
}
#endif
)GLSL";

// ShaderMaterial carrying one custom uniform. The engine has no named-parameter path
// for user shaders (upstream's setParameter('uLightPos', ...)), so the material
// supplies its whole uniform block instead: customUniformData() replaces MaterialData
// at buffer(3) / set 0 with these bytes.
class ToonMaterial final : public ShaderMaterial
{
public:
    using ShaderMaterial::ShaderMaterial;

    void setLightPosition(const Vector3& position)
    {
        _data.lightPos[0] = position.getX();
        _data.lightPos[1] = position.getY();
        _data.lightPos[2] = position.getZ();
        _data.lightPos[3] = 1.0f;
    }

    const void* customUniformData(size_t& outSize) const override
    {
        outSize = sizeof(_data);
        return &_data;
    }

private:
    struct alignas(16) ToonData
    {
        float lightPos[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    } _data;
};

class CustomShaderExample final: public ExampleApp
{
public:
    CustomShaderExample(): ExampleApp({.title = "Custom Shader (Toon)"}) {}

protected:
    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // Camera. Upstream translates it without rotating, so it looks straight down -Z.
        const Vector3 cameraPosition(0.0f, 7.0f, 24.0f);
        auto* camera = createCamera(cameraPosition);
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        }

        // Omni light. The toon shader does its own lighting, so this entity only supplies
        // the light position the material passes to the shader.
        const Vector3 lightPosition(0.0f, 1.0f, 0.0f);
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lc->setType(LightType::LIGHTTYPE_OMNI);
            lc->setColor(Color(1.0f, 1.0f, 1.0f));
            lc->setRange(10.0f);
        }
        light->setLocalPosition(lightPosition);
        root()->addChild(light);

        // Custom toon material — bypasses the PBR pipeline entirely.
        _toonMaterial = std::make_shared<ToonMaterial>(
            device(), "toon", "vertexShader", "fragmentShader",
            ShaderSourceSet{.msl = kToonShaderSource, .glsl = kToonShaderSourceGlsl});

        // DEVIATION: upstream's shader compares a WORLD-space normal (matrix_normal is the
        // world normal matrix, despite the "eye coordinates" comment) against a light
        // direction built from a VIEW-space vertex position. With its unrotated camera the
        // view matrix is a pure translation, so that mix is exactly a world-space light at
        // lightPosition + cameraPosition — which is what this port feeds the shader, keeping
        // the lighting identical while the shader stays consistently world-space.
        _toonMaterial->setLightPosition(lightPosition + cameraPosition);

        _statueAsset = std::make_unique<Asset>(
            "statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        const auto statueResource = _statueAsset->resource();
        if (!statueResource) {
            spdlog::error("Failed to load models/statue.glb");
            return false;
        }
        _statue = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
        root()->addChild(_statue);

        // Set the new material on every mesh in the model.
        int meshInstanceCount = 0;
        for (auto* render : _statue->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                meshInstance->setMaterial(_toonMaterial.get());
                ++meshInstanceCount;
            }
        }

        spdlog::info("*** Custom Shader (Toon) Example ***");
        spdlog::info("ShaderMaterial toon shader applied to {} mesh instances. Esc quits.",
                     meshInstanceCount);

        return true;
    }

    void update(const float dt) override
    {
        // Rotate the statue.
        _angle += 60.0f * dt;
        _statue->setLocalEulerAngles(0.0f, _angle, 0.0f);
    }

    void destroy() override
    {
        spdlog::info("*** Custom Shader (Toon) Example Finished ***");
    }

private:
    std::unique_ptr<Asset> _statueAsset;
    std::shared_ptr<ToonMaterial> _toonMaterial;
    Entity* _statue = nullptr;
    float _angle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(CustomShaderExample)
