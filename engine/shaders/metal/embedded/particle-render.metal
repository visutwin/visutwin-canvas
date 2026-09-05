#include <metal_stdlib>
using namespace metal;

struct Particle {
    float4 posAge;        // xyz = position, w = age (<0 unborn, >lifetime dead)
    float4 velLifetime;   // xyz = velocity, w = lifetime
    float4 rotSeedSize;   // x = rotation (rad), y = rotSpeed (rad/s), z = seed, w = unused
};

struct ParticleRenderParams {
    float4x4 modelView;
    float4x4 projection;
    float4 animParams;    // tilesX, tilesY, numFrames, animSpeed
    float4 miscParams;    // intensity, particle count, hasColorMap, pad
    float4 colorLut[16];  // rgb + alpha over normalized life
    float4 scaleLut[16];  // x = world size
};

struct ParticleVaryings {
    float4 position [[position]];
    float2 uv;
    float4 color;
    float hasMap;
};

vertex ParticleVaryings particleVS(uint vid [[vertex_id]],
                                   uint iid [[instance_id]],
                                   constant Particle* particles [[buffer(7)]],
                                   constant ParticleRenderParams& params [[buffer(11)]])
{
    ParticleVaryings out;
    out.position = float4(0.0, 0.0, 2.0, 1.0);  // default: clipped
    out.uv = float2(0.0);
    out.color = float4(0.0);
    out.hasMap = params.miscParams.z;

    if (iid >= uint(params.miscParams.y)) {
        return out;
    }
    const Particle p = particles[iid];
    const float age = p.posAge.w;
    const float lifetime = max(p.velLifetime.w, 1e-5);
    if (age < 0.0 || age > lifetime) {
        return out;   // unborn or dead
    }
    const float lifeT = saturate(age / lifetime);

    // LUT lookup with linear interpolation between the 16 samples.
    const float lutPos = lifeT * 15.0;
    const int lutIdx = int(lutPos);
    const int lutIdx2 = min(lutIdx + 1, 15);
    const float lutFrac = lutPos - float(lutIdx);
    const float4 colorA = mix(params.colorLut[lutIdx], params.colorLut[lutIdx2], lutFrac);
    const float size = mix(params.scaleLut[lutIdx].x, params.scaleLut[lutIdx2].x, lutFrac);

    if (colorA.a <= 0.001 || size <= 0.0001) {
        return out;
    }

    // Screen-aligned billboard: offset the corners in view space.
    const float2 cornerUV[4] = { float2(-1.0, -1.0), float2(1.0, -1.0),
                                 float2(-1.0, 1.0), float2(1.0, 1.0) };
    const float2 corner = cornerUV[vid];

    const float angle = p.rotSeedSize.x + p.rotSeedSize.y * age;
    const float ca = cos(angle);
    const float sa = sin(angle);
    const float2 rotated = float2(corner.x * ca - corner.y * sa,
                                  corner.x * sa + corner.y * ca) * (size * 0.5);

    float4 viewPos = params.modelView * float4(p.posAge.xyz, 1.0);
    viewPos.xy += rotated;

    float4 clip = params.projection * viewPos;
    // DEVIATION: OpenGL NDC z range is [-1,1]; Metal requires [0,1].
    clip.z = 0.5 * (clip.z + clip.w);

    // Sprite-sheet frame from particle life (tile 0 = top-left).
    const float2 tiles = max(params.animParams.xy, float2(1.0));
    const float numFrames = max(params.animParams.z, 1.0);
    // animIndex selects WHICH animation in the sheet: each is numFrames tiles long
    // and they run in reading order, so a 4x4 sheet at 4 frames holds four of them.
    const float frame = floor(fmod(lifeT * numFrames * max(params.animParams.w, 0.0001), numFrames))
        + params.miscParams.w * numFrames;
    const float2 tileUv = (corner * 0.5 + 0.5);
    const float2 frameOrigin = float2(fmod(frame, tiles.x), floor(frame / tiles.x));
    out.uv = (frameOrigin + float2(tileUv.x, 1.0 - tileUv.y)) / tiles;

    out.position = clip;
    out.color = float4(colorA.rgb * params.miscParams.x, colorA.a);
    return out;
}

fragment half4 particleFS(ParticleVaryings in [[stage_in]],
                          texture2d<float> colorMap [[texture(0)]])
{
    constexpr sampler mapSampler(filter::linear, mip_filter::linear, address::clamp_to_edge);
    float4 tex = float4(1.0);
    if (in.hasMap > 0.5 && colorMap.get_width() > 0) {
        tex = colorMap.sample(mapSampler, in.uv);
    } else {
        // Procedural soft disc when no color map is assigned.
        const float2 c = in.uv * 2.0 - 1.0;   // NOTE: uv covers the frame tile
        const float d = length(fract(in.uv * 1.0) * 2.0 - 1.0);
        tex.a = saturate(1.0 - d);
        tex.a *= tex.a;
    }
    const float alpha = tex.a * in.color.a;
    return half4(half3(float3(in.color.rgb * tex.rgb)), half(alpha));
}
