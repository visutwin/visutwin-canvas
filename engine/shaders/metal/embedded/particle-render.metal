#include <metal_stdlib>
using namespace metal;

struct Particle {
    float4 posAge;        // xyz = position, w = age (<0 unborn, >lifetime dead)
    float4 velLifetime;   // xyz = velocity, w = lifetime
    float4 rotSeedSize;   // x = rotation (rad), y = rotSpeed (rad/s), z = unused, w = hidden
    float4 motion;        // xyz = velocity this step, w = per-life seed
};

struct ParticleRenderParams {
    float4x4 modelView;
    float4x4 projection;
    float4 animParams;    // tilesX, tilesY, numFrames, animSpeed
    float4 miscParams;    // intensity, particle count, hasColorMap, animIndex
    float4 motionParams;  // alignToMotion, stretch, screenSpace, viewport height / width
    float4 outputParams;  // exposure, tone mapping mode, linear HDR target, unused
    float4 colorLut[16];  // rgb + alpha over normalized life
    float4 scaleLut[16];  // x = world size
};

struct ParticleVaryings {
    float4 position [[position]];
    float2 uv;
    float4 color;
    float hasMap;
    float3 output [[flat]];   // outputParams.xyz
};

// toneMap(color, exposure, mode) is the forward pass's own, from the common-tonemap chunk,
// which ParticleEmitter splices in after the metal_stdlib prologue.

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
    out.output = params.outputParams.xyz;

    if (iid >= uint(params.miscParams.y)) {
        return out;
    }
    const Particle p = particles[iid];
    const float age = p.posAge.w;
    const float lifetime = max(p.velLifetime.w, 1e-5);
    if (age <= 0.0 || age > lifetime || p.rotSeedSize.w > 0.5) {
        return out;   // unborn, dead or hidden (upstream particle.js)
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

    // Screen-aligned billboard, upstream's particle.js + particle_pointAlong / _billboard /
    // _stretch / _end, in view space. In SCREEN SPACE the model matrix already lands in clip
    // space (the view and projection are identity) and the quad is sized in viewport heights.
    const float2 cornerUV[4] = { float2(-1.0, -1.0), float2(1.0, -1.0),
                                 float2(-1.0, 1.0), float2(1.0, 1.0) };
    const float2 corner = cornerUV[vid];
    const bool screenSpace = params.motionParams.z > 0.5;

    float4 viewPos = params.modelView * float4(p.posAge.xyz, 1.0);
    const float3 viewVelocity = (params.modelView * float4(p.motion.xyz, 0.0)).xyz;
    float2 velocityV = viewVelocity.xy;
    if (screenSpace) {
        // The offset x is scaled by height / width below (upstream #9570), so measure the
        // direction of motion in the same units.
        velocityV.x /= params.motionParams.w;
    }
    velocityV = length(velocityV) > 1e-6 ? normalize(velocityV) : velocityV;

    float angle = p.rotSeedSize.x + p.rotSeedSize.y * age;
    if (params.motionParams.x > 0.5) {
        angle = atan2(velocityV.x, velocityV.y);
    }
    // Upstream's rotate(): a positive angle turns the quad clockwise.
    const float ca = cos(angle);
    const float sa = sin(angle);
    const float2 offset = float2(corner.x * ca + corner.y * sa,
                                 -corner.x * sa + corner.y * ca);

    if (params.motionParams.y > 0.0) {
        // Stretch: the vertices trailing the motion are pulled back along it.
        const float3 previous = viewPos.xyz - viewVelocity * params.motionParams.y;
        const float interpolation = dot(-velocityV, normalize(offset)) * 0.5 + 0.5;
        viewPos.xyz = mix(viewPos.xyz, previous, interpolation);
    }

    float2 scaled = offset * size;
    float4 clip;
    if (screenSpace) {
        scaled.x *= params.motionParams.w;
        clip = float4(viewPos.xy + scaled, 0.0, 1.0);
    } else {
        viewPos.xy += scaled;
        clip = params.projection * viewPos;
    }
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
        // The colour map is sRGB-authored (upstream loads every one with srgb: true).
        tex.rgb = pow(max(tex.rgb, float3(0.0)), float3(2.2));
    } else {
        // Procedural soft disc when no color map is assigned.
        const float d = length(fract(in.uv * 1.0) * 2.0 - 1.0);
        tex.a = saturate(1.0 - d);
        tex.a *= tex.a;
    }
    const float alpha = tex.a * in.color.a;
    // Upstream particle_end: the colour is linear; a gamma target tone-maps it with the
    // scene's exposure and encodes it, a camera frame's linear HDR scene leaves both to compose.
    float3 rgb = in.color.rgb * tex.rgb;
    if (in.output.z < 0.5) {
        rgb = toneMap(rgb, in.output.x, in.output.y);
        rgb = pow(max(rgb, float3(0.0)) + 0.0000001, float3(1.0 / 2.2));  // upstream gammaCorrectOutput
    }
    return half4(half3(rgb), half(alpha));
}
