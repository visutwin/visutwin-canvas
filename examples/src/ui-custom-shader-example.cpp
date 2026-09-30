// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/custom-shader.
//
// Ability buttons whose cooldown is drawn by a ShaderMaterial on an image element: a shaded
// circle that sweeps away clockwise, driven by a uniform. Click an ability to cast it, then wait
// for it to be ready again.
//
// DEVIATIONS from upstream:
//  - the shader is written once per backend language (MSL and GLSL, as the toon custom-shader
//    example does), and its two uniforms travel as one block (`customUniformData`) where
//    upstream names them with setParameter;
//  - the material culls no faces, as the element's own material does: an element's quad is in
//    clip space here, where its winding is not something the example should depend on;
//  - gammaPS's gammaCorrectOutput is written out: the shader always encodes to gamma, as nothing
//    here renders the interface through a camera frame.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiAtlas.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/sprite.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

namespace
{
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);
    const Color SLATE(0.22f, 0.25f, 0.32f, 1.0f);
    const Color PANEL(0.14f, 0.16f, 0.2f, 1.0f);

    // A vertex shader for image elements on a screen-space screen, which pass one UV set through.
    // There, the model matrix already maps the element straight to clip space, so the camera's
    // matrices are not needed. Depth is upstream's GL 0, which is 0.5 in this engine's [0, 1].
    //
    // A radial cooldown: a circle the size of the element, whose shaded part shrinks clockwise
    // from twelve o'clock as the cooldown runs out. `progress` is the fraction left, 1 down to 0;
    // `color` the shading's colour and opacity, in linear space.
    const char* kCooldownMsl = R"MSL(
using namespace metal;

struct VertexData {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct ModelData {
    float4x4 modelMatrix;
    float4x4 normalMatrix;
};

struct Cooldown {
    float4 color;
    float progress;
};

struct Varyings {
    float4 position [[position]];
    float2 uv0;
};

vertex Varyings vertexShader(VertexData v [[stage_in]], constant ModelData& model [[buffer(2)]])
{
    Varyings out;
    out.uv0 = v.uv0;
    float4 clip = model.modelMatrix * float4(v.position, 1.0);
    out.position = float4(clip.xy, 0.5, 1.0);
    return out;
}

fragment float4 fragmentShader(Varyings in [[stage_in]], constant Cooldown& cooldown [[buffer(3)]])
{
    // the position from the center, with y up: v runs down the element
    float2 p = float2(in.uv0.x - 0.5, 0.5 - in.uv0.y);
    float r = length(p);

    // the circle, with an edge a pixel wide
    float inside = 1.0 - smoothstep(0.5 - fwidth(r), 0.5, r);

    // the angle clockwise from twelve o'clock, from 0 to 1, and whether it is still shaded
    float angle = fract(atan2(p.x, p.y) / 6.28318530718 + 1.0);
    float shaded = step(1.0 - cooldown.progress, angle);

    return float4(pow(cooldown.color.rgb + 0.0000001, float3(1.0 / 2.2)), cooldown.color.a * inside * shaded);
}
)MSL";

    // The same in GLSL. The model matrix arrives in the vertex push constant, and the uniform
    // block is the per-draw material block (set 0, binding 0), bound to both stages.
    const char* kCooldownGlsl = R"GLSL(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
} pc;

layout(set = 0, binding = 0) uniform Cooldown {
    vec4 color;
    float progress;
} cooldown;

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec2 vertexTexCoord0;
layout(location = 0) out vec2 vUv0;
void main() {
    vUv0 = vertexTexCoord0;
    vec4 clip = pc.model * vec4(vertexPosition, 1.0);
    gl_Position = vec4(clip.xy, 0.5, 1.0);
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(location = 0) in vec2 vUv0;
layout(location = 0) out vec4 fragColor;
void main() {
    vec2 p = vec2(vUv0.x - 0.5, 0.5 - vUv0.y);
    float r = length(p);
    float inside = 1.0 - smoothstep(0.5 - fwidth(r), 0.5, r);
    float angle = fract(atan(p.x, p.y) / 6.28318530718 + 1.0);
    float shaded = step(1.0 - cooldown.progress, angle);
    fragColor = vec4(pow(cooldown.color.rgb + 0.0000001, vec3(1.0 / 2.2)), cooldown.color.a * inside * shaded);
}
#endif
)GLSL";

    /// The cooldown material: the shader and its one uniform block (upstream's uColor and
    /// uProgress). It blends normally and doesn't write depth, as upstream's does; a custom
    /// material replaces the element's own handling of colour and opacity, so the shading colour
    /// is a uniform of its own.
    class CooldownMaterial final : public ShaderMaterial
    {
    public:
        explicit CooldownMaterial(const std::shared_ptr<GraphicsDevice>& device)
            : ShaderMaterial(device, "cooldown", "vertexShader", "fragmentShader",
                             ShaderSourceSet{.msl = kCooldownMsl, .glsl = kCooldownGlsl})
        {
            setAlphaMode(AlphaMode::BLEND);
            setCullMode(CullMode::CULLFACE_NONE);
        }

        void setProgress(const float value) { _data.progress = value; }

        const void* customUniformData(size_t& outSize) const override
        {
            outSize = sizeof(_data);
            return &_data;
        }

    private:
        struct alignas(16) Data
        {
            float color[4] = {0.02f, 0.02f, 0.03f, 0.75f};
            float progress = 0.0f;
            float pad[3] = {};
        } _data;
    };

    FontResource* fontOf(Asset& asset)
    {
        const auto res = asset.resource();
        return res && std::holds_alternative<FontResource*>(*res) ? std::get<FontResource*>(*res) : nullptr;
    }

    Texture* textureOf(Asset& asset)
    {
        const auto res = asset.resource();
        return res && std::holds_alternative<Texture*>(*res) ? std::get<Texture*>(*res) : nullptr;
    }

    struct ElementProps
    {
        ElementType type = ElementType::Image;
        std::shared_ptr<Sprite> sprite;
        std::shared_ptr<Material> material;
        Color color = LIGHT;
        std::optional<float> width;
        std::optional<float> height;
        bool useInput = false;
        FontResource* font = nullptr;
        std::string text;
        std::optional<int> fontSize;
    };
}

class UiCustomShaderExample final: public ExampleApp
{
public:
    UiCustomShaderExample()
        : ExampleApp({.title = "UI Custom Shader", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ScreenComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        options.registerComponentSystem<ButtonComponentSystem>();
        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _boldAsset = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        _font = fontOf(*_fontAsset);
        _bold = fontOf(*_boldAsset);
        Texture* atlasTexture = textureOf(*_uiAtlasTexture);
        if (!_font || !_bold || !atlasTexture) {
            spdlog::error("Failed to load the Roboto fonts or ui/ui-atlas.png");
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        _screen = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        _screen->setScreenSpace(true);
        _screen->setReferenceResolution(Vector2(1280.0f, 720.0f));
        _screen->setScaleMode(ScreenScaleMode::Blend);
        _screen->setScaleBlend(0.5f);
        root()->addChild(screenEntity);
        _screenEntity = screenEntity;

        _atlas = createUiAtlas(atlasTexture);
        _circle = std::make_shared<Sprite>(_atlas, std::vector<std::string>{"circle"});
        auto rounded = std::make_shared<Sprite>(_atlas, std::vector<std::string>{"panel"}, 2.0f,
                                                SpriteRenderMode::Sliced);

        // The action bar the abilities sit on
        createElement(screenEntity, {.sprite = rounded, .color = PANEL, .width = 520.0f, .height = 330.0f});

        _abilities.push_back(createAbility("Lightning", "icon-bolt", Color(1.0f, 0.85f, 0.3f, 1.0f), -120.0f, 3.0f));
        _abilities.push_back(createAbility("Fireball", "icon-flame", Color(1.0f, 0.5f, 0.2f, 1.0f), 120.0f, 6.0f));
        for (auto& ability : _abilities) {
            Ability* a = &ability;
            a->button->on("click", [this, a]() { cast(*a); });
        }

        // Start with the fireball part of the way through its cooldown
        cast(_abilities[1], 4.0f);

        layout();
        return true;
    }

    // Run the cooldowns: update the shader's uniform every frame, and the seconds when they change
    void update(const float dt) override
    {
        layout();
        for (auto& ability : _abilities) {
            if (ability.left <= 0.0f) {
                continue;
            }
            ability.left = std::max(ability.left - dt, 0.0f);
            ability.material->setProgress(ability.left / ability.duration);

            const std::string text = std::to_string(static_cast<int>(std::ceil(ability.left)));
            if (ability.seconds->text() != text) {
                ability.seconds->setText(text);
            }
            if (ability.left == 0.0f) {
                ability.button->setActive(true);
                ability.overlay->entity()->setEnabled(false);
                ability.seconds->entity()->setEnabled(false);
            }
        }
    }

private:
    struct Ability
    {
        ButtonComponent* button = nullptr;
        std::shared_ptr<CooldownMaterial> material;
        ElementComponent* overlay = nullptr;
        ElementComponent* seconds = nullptr;
        float duration = 0.0f;
        float left = 0.0f;
    };

    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        ElementDesc desc{.type = props.type, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f)};
        desc.width = props.width;
        desc.height = props.height;
        desc.useInput = props.useInput;
        element->setup(desc);
        if (props.sprite) {
            element->setSprite(props.sprite);
        }
        if (props.material) {
            element->setMaterial(props.material);
        }
        element->setColor(props.color);
        if (props.type == ElementType::Text) {
            element->setFontResource(props.font ? props.font : _font);
            if (props.fontSize) {
                element->setFontSize(*props.fontSize);
            }
            element->setText(props.text);
        }
        parent->addChild(entity);
        return element;
    }

    // An ability: a round button with an icon, a cooldown drawn over it by the custom material,
    // and the seconds left
    Ability createAbility(const std::string& name, const std::string& iconFrame, const Color& color, const float x,
                          const float duration)
    {
        Ability ability;
        ElementComponent* element = createElement(_screenEntity, {.sprite = _circle, .color = SLATE, .width = 180.0f,
                                                                  .height = 180.0f, .useInput = true});
        element->entity()->setLocalPosition(x, 35.0f, 0.0f);
        ability.button = static_cast<ButtonComponent*>(element->entity()->addComponent<ButtonComponent>());
        ability.button->setImageEntity(element->entity());
        ability.button->setHoverTint(Color(0.29f, 0.33f, 0.42f, 1.0f));
        ability.button->setPressedTint(Color(0.17f, 0.19f, 0.25f, 1.0f));
        ability.button->setInactiveTint(SLATE);

        auto icon = std::make_shared<Sprite>(_atlas, std::vector<std::string>{iconFrame});
        createElement(element->entity(), {.sprite = icon, .color = color, .width = 96.0f, .height = 96.0f});

        // the cooldown covers the icon, and is only shown while it runs
        ability.material = std::make_shared<CooldownMaterial>(device());
        ability.overlay = createElement(element->entity(), {.material = ability.material, .width = 180.0f,
                                                            .height = 180.0f});
        ability.seconds = createElement(element->entity(), {.type = ElementType::Text, .color = LIGHT,
                                                            .font = _bold, .fontSize = 52});
        ability.overlay->entity()->setEnabled(false);
        ability.seconds->entity()->setEnabled(false);
        createElement(_screenEntity, {.type = ElementType::Text, .color = MUTED, .text = name, .fontSize = 26})
            ->entity()->setLocalPosition(x, -98.0f, 0.0f);
        ability.duration = duration;
        return ability;
    }

    // Cast an ability, which starts its cooldown and makes its button inactive until it is over
    void cast(Ability& ability, const std::optional<float> left = std::nullopt)
    {
        ability.left = left.value_or(ability.duration);
        ability.button->setActive(false);
        ability.overlay->entity()->setEnabled(true);
        ability.seconds->entity()->setEnabled(true);
    }

    // Use a portrait reference resolution on portrait canvases, and scale to whichever axis has
    // less room, so the abilities stay on screen
    void layout()
    {
        const auto [w, h] = engine()->canvasSize();
        if (w == _laidOutWidth && h == _laidOutHeight) {
            return;
        }
        _laidOutWidth = w;
        _laidOutHeight = h;
        const bool portrait = h > w;
        const Vector2 reference = portrait ? Vector2(540.0f, 960.0f) : Vector2(1280.0f, 720.0f);
        _screen->setReferenceResolution(reference);
        _screen->setScaleBlend(static_cast<float>(w) / reference.x > static_cast<float>(h) / reference.y ? 1.0f : 0.0f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    std::shared_ptr<TextureAtlas> _atlas;
    std::shared_ptr<Sprite> _circle;
    Entity* _screenEntity = nullptr;
    ScreenComponent* _screen = nullptr;
    std::vector<Ability> _abilities;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(UiCustomShaderExample)
