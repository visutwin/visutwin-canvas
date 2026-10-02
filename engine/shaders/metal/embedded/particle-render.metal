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
    float4x4 view;
    float4 animParams;    // tilesX, tilesY, numFrames, animSpeed
    float4 miscParams;    // intensity, particle count, hasColorMap, animIndex
    float4 motionParams;  // alignToMotion, stretch, screenSpace, viewport height / width
    float4 outputParams;  // exposure, tone mapping mode, linear HDR target, sorted
    float4 faceTangent;   // xyz, w = WORLD / EMITTER oriented quad
    float4 faceBinorm;    // xyz, w = mesh particles
    float4 wrapParams;    // xyz bounds, w = wrap
    float4 emitterPosition; // xyz, w = local space
    float4 softParams;    // softening, camera near, camera far, soft
    float4 lightCube[6];  // rgb; [0].w lit, [1].w half Lambert, [2].w normal map
    float4 colorLut[16];  // rgb + alpha over normalized life
    float4 scaleLut[16];  // x = size, y = size graph2, z = alpha graph2
};

struct ParticleVaryings {
    float4 position [[position]];
    float2 uv;
    float4 color;
    float hasMap;
    float3 output [[flat]];   // outputParams.xyz
    float3 normal;            // world: a lit particle's normal (the TBN's N with a normal map)
    float3 tangent;
    float3 binormal;
    float viewDepth;          // linear depth, for softening
};

// toneMap(color, exposure, mode) is the forward pass's own, from the common-tonemap chunk,
// which ParticleEmitter splices in after the metal_stdlib prologue.

// The simulation's PCG hash, for the per-particle random points between a graph and its
// graph2 (particleSimShaders.h).
static inline uint particlePcgHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
static inline float particleRnd(uint key) { return float(particlePcgHash(key) >> 8) * (1.0 / 16777216.0); }

// mat2(c, -s, s, c) * v — a positive angle turns clockwise.
static inline float2 particleRotate(float2 v, float c, float s)
{
    return float2(v.x * c + v.y * s, -v.x * s + v.y * c);
}

vertex ParticleVaryings particleVS(uint vid [[vertex_id]],
                                   uint iid [[instance_id]],
                                   constant Particle* particles [[buffer(7)]],
                                   constant uint* order [[buffer(8)]],
                                   constant float* meshVertices [[buffer(9)]],
                                   constant ParticleRenderParams& params [[buffer(11)]])
{
    ParticleVaryings out;
    out.position = float4(0.0, 0.0, 2.0, 1.0);  // default: clipped
    out.uv = float2(0.0);
    out.color = float4(0.0);
    out.hasMap = params.miscParams.z;
    out.output = params.outputParams.xyz;
    out.normal = float3(0.0, 0.0, 1.0);
    out.tangent = float3(1.0, 0.0, 0.0);
    out.binormal = float3(0.0, 1.0, 0.0);
    out.viewDepth = 0.0;

    if (iid >= uint(params.miscParams.y)) {
        return out;
    }
    // A sorted emitter draws its particles in the order the sort wrote.
    const uint particleIndex = params.outputParams.w > 0.5 ? order[iid] : iid;
    const Particle p = particles[particleIndex];
    const float age = p.posAge.w;
    const float lifetime = max(p.velLifetime.w, 1e-5);
    if (age <= 0.0 || age > lifetime || p.rotSeedSize.w > 0.5) {
        return out;   // unborn, dead or hidden
    }
    const float lifeT = saturate(age / lifetime);

    // LUT lookup with linear interpolation between the 16 samples.
    const float lutPos = lifeT * 15.0;
    const int lutIdx = int(lutPos);
    const int lutIdx2 = min(lutIdx + 1, 15);
    const float lutFrac = lutPos - float(lutIdx);
    float4 colorA = mix(params.colorLut[lutIdx], params.colorLut[lutIdx2], lutFrac);
    const float4 scaleSample = mix(params.scaleLut[lutIdx], params.scaleLut[lutIdx2], lutFrac);
    // A random point between each graph and its graph2, per particle life.
    const uint seed = uint(p.motion.w);
    const float size = mix(scaleSample.x, scaleSample.y, particleRnd(seed + 20u));
    colorA.a += (scaleSample.z - colorA.a) * particleRnd(seed + 21u);

    if (colorA.a <= 0.001 || size <= 0.0001) {
        return out;
    }

    // Wrap: a world-space particle wraps into a box around the emitter.
    float3 particlePos = p.posAge.xyz;
    if (params.wrapParams.w > 0.5) {
        const float3 bounds = params.wrapParams.xyz;
        const float3 relative = particlePos - params.emitterPosition.xyz;
        particlePos = relative - bounds * floor(relative / bounds) - bounds * 0.5 + params.emitterPosition.xyz;
    }

    const float2 cornerUV[4] = { float2(-1.0, -1.0), float2(1.0, -1.0),
                                 float2(-1.0, 1.0), float2(1.0, 1.0) };
    const bool screenSpace = params.motionParams.z > 0.5;
    const bool useMesh = params.faceBinorm.w > 0.5;
    const bool customFace = params.faceTangent.w > 0.5;
    const float2 corner = useMesh ? float2(0.0) : cornerUV[min(vid, 3u)];

    float4 viewPos = params.modelView * float4(particlePos, 1.0);
    const float3 viewVelocity = (params.modelView * float4(p.motion.xyz, 0.0)).xyz;
    float2 velocityV = viewVelocity.xy;
    if (screenSpace) {
        // The offset x is scaled by height / width below, so measure the
        // direction of motion in the same units.
        velocityV.x /= params.motionParams.w;
    }
    velocityV = length(velocityV) > 1e-6 ? normalize(velocityV) : velocityV;

    float angle = p.rotSeedSize.x + p.rotSeedSize.y * age;
    if (params.motionParams.x > 0.5) {
        angle = atan2(velocityV.x, velocityV.y);
    }
    const float ca = cos(angle);
    const float sa = sin(angle);
    const float2 offset = particleRotate(corner, ca, sa);

    // The camera's axes in world space (rows of the view's rotation).
    const float3 cameraRight = float3(params.view[0][0], params.view[1][0], params.view[2][0]);
    const float3 cameraUp = float3(params.view[0][1], params.view[1][1], params.view[2][1]);
    const float3 cameraBack = float3(params.view[0][2], params.view[1][2], params.view[2][2]);

    float2 meshUv = float2(0.0);
    float3 worldOffset = float3(0.0);     // unscaled, for a mesh or an oriented quad
    float4 clip;
    if (useMesh || customFace) {
        if (useMesh) {
            // Mesh particle: the mesh's vertex, turned about z then x by the angle.
            const uint base = vid * 14u;
            float3 local = float3(meshVertices[base], meshVertices[base + 1u], meshVertices[base + 2u]);
            local.xy = particleRotate(local.xy, ca, sa);
            local.yz = particleRotate(local.yz, ca, sa);
            worldOffset = local;
            meshUv = float2(meshVertices[base + 6u], meshVertices[base + 7u]);
        } else {
            // Custom face: the quad in the plane of the face vectors.
            worldOffset = params.faceTangent.xyz * offset.x + params.faceBinorm.xyz * offset.y;
        }
        const float3 viewOffset = (params.view * float4(worldOffset * size, 0.0)).xyz;
        if (params.motionParams.y > 0.0 && dot(viewOffset.xy, viewOffset.xy) > 1e-12) {
            // Stretch, for any vertex: the ones trailing the motion are
            // pulled back along it.
            const float3 previous = viewPos.xyz - viewVelocity * params.motionParams.y;
            const float interpolation = dot(-velocityV, normalize(viewOffset.xy)) * 0.5 + 0.5;
            viewPos.xyz = mix(viewPos.xyz, previous, interpolation);
        }
        viewPos.xyz += viewOffset;
        clip = params.projection * viewPos;
    } else {
        // Screen-aligned billboard (point-along, billboard, stretch), in view space. In SCREEN SPACE the model matrix already lands in
        // clip space (the view and projection are identity) and the quad is sized in viewport
        // heights.
        if (params.motionParams.y > 0.0) {
            const float3 previous = viewPos.xyz - viewVelocity * params.motionParams.y;
            const float interpolation = dot(-velocityV, normalize(offset)) * 0.5 + 0.5;
            viewPos.xyz = mix(viewPos.xyz, previous, interpolation);
        }
        float2 scaled = offset * size;
        if (screenSpace) {
            scaled.x *= params.motionParams.w;
            clip = float4(viewPos.xy + scaled, 0.0, 1.0);
        } else {
            viewPos.xy += scaled;
            clip = params.projection * viewPos;
        }
        worldOffset = cameraRight * offset.x + cameraUp * offset.y;
    }
    // GL-style projection (NDC z in [-1,1]) to the [0,1] depth both backends use.
    clip.z = 0.5 * (clip.z + clip.w);
    out.viewDepth = -viewPos.z;

    // A lit particle: the normal bulges out from
    // the quad's centre toward the vertex, and a normal map turns the camera-aligned frame
    // by the particle's angle.
    if (params.lightCube[0].w > 0.5) {
        out.normal = normalize(worldOffset + cameraBack);
        const float3 t = -cameraRight;
        const float3 b = -cameraUp;
        out.tangent = t * ca - b * sa;
        out.binormal = t * sa + b * ca;
        if (params.lightCube[2].w > 0.5) {
            out.normal = cameraBack;
        }
    }

    // Sprite-sheet frame from particle life (tile 0 = top-left).
    const float2 tiles = max(params.animParams.xy, float2(1.0));
    const float numFrames = max(params.animParams.z, 1.0);
    // animIndex selects WHICH animation in the sheet: each is numFrames tiles long
    // and they run in reading order, so a 4x4 sheet at 4 frames holds four of them.
    const float frame = floor(fmod(lifeT * numFrames * max(params.animParams.w, 0.0001), numFrames))
        + params.miscParams.w * numFrames;
    const float2 frameOrigin = float2(fmod(frame, tiles.x), floor(frame / tiles.x));
    const float2 tileUv = useMesh ? meshUv : float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    out.uv = (frameOrigin + tileUv) / tiles;

    out.position = clip;
    out.color = float4(colorA.rgb * params.miscParams.x, colorA.a);
    return out;
}

fragment half4 particleFS(ParticleVaryings in [[stage_in]],
                          texture2d<float> colorMap [[texture(0)]],
                          texture2d<float> normalMap [[texture(1)]],
                          depth2d<float> sceneDepth [[texture(25)]],
                          constant ParticleRenderParams& params [[buffer(11)]])
{
    constexpr sampler mapSampler(filter::linear, mip_filter::linear, address::clamp_to_edge);
    float4 tex = float4(1.0);
    if (in.hasMap > 0.5 && colorMap.get_width() > 0) {
        tex = colorMap.sample(mapSampler, in.uv);
        // The colour map is sRGB-authored.
        tex.rgb = pow(max(tex.rgb, float3(0.0)), float3(2.2));
    } else {
        // Procedural soft disc when no color map is assigned.
        const float d = length(fract(in.uv * 1.0) * 2.0 - 1.0);
        tex.a = saturate(1.0 - d);
        tex.a *= tex.a;
    }
    float alpha = tex.a * in.color.a;
    float3 rgb = in.color.rgb * tex.rgb;

    // Soft particles: fade where the particle nears the scene behind it.
    if (params.softParams.w > 0.5) {
        const float near = params.softParams.y;
        const float far = params.softParams.z;
        const float raw = sceneDepth.read(uint2(in.position.xy));
        const float depth = (near * far) / (far - raw * (far - near));
        alpha *= saturate(abs(in.viewDepth - depth) * params.softParams.x);
    }

    // Lighting: the light cube, by Lambert or half Lambert on the normal.
    if (params.lightCube[0].w > 0.5) {
        float3 normal = normalize(in.normal);
        if (params.lightCube[2].w > 0.5) {
            const float3 n = normalize(normalMap.sample(mapSampler, in.uv).xyz * 2.0 - 1.0);
            normal = normalize(in.tangent * n.x + in.binormal * n.y + in.normal * n.z);
        }
        float3 negNormal;
        float3 posNormal;
        if (params.lightCube[1].w > 0.5) {
            negNormal = normal * 0.5 + 0.5;
            posNormal = -normal * 0.5 + 0.5;
            negNormal *= negNormal;
            posNormal *= posNormal;
        } else {
            negNormal = max(normal, float3(0.0));
            posNormal = max(-normal, float3(0.0));
        }
        const float3 light = negNormal.x * params.lightCube[0].xyz + posNormal.x * params.lightCube[1].xyz +
                             negNormal.y * params.lightCube[2].xyz + posNormal.y * params.lightCube[3].xyz +
                             negNormal.z * params.lightCube[4].xyz + posNormal.z * params.lightCube[5].xyz;
        rgb *= light;
    }

    // Output: the colour is linear; a gamma target tone-maps it with the
    // scene's exposure and encodes it, a camera frame's linear HDR scene leaves both to compose.
    if (in.output.z < 0.5) {
        rgb = toneMap(rgb, in.output.x, in.output.y);
        rgb = pow(max(rgb, float3(0.0)) + 0.0000001, float3(1.0 / 2.2));  // gamma correction
    }
    return half4(half3(rgb), half(alpha));
}
