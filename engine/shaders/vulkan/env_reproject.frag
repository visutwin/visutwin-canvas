#version 450
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D sourceEquirect;
layout(set=0,binding=1) uniform samplerCube sourceCube;
layout(push_constant) uniform Push {
    vec4 rectTarget; // rect x,y,w,h in pixels
    vec4 flags;      // sourceCube, decodeSrgb, encodeRgbp, cubeFace
    vec4 misc;       // convolve, roughness, sourceProjection, targetProjection
} p;
// Projection ids mirror TextureProjection in platform/graphics/constants.h.
const float PROJ_OCTAHEDRAL = 3.0;
const float PI = 3.14159265359;
vec3 faceDirection(int face, vec2 st) {
    vec2 q = st * 2.0 - 1.0;
    if (face == 0) return normalize(vec3( 1.0, -q.y, -q.x));
    if (face == 1) return normalize(vec3(-1.0, -q.y,  q.x));
    if (face == 2) return normalize(vec3( q.x,  1.0,  q.y));
    if (face == 3) return normalize(vec3( q.x, -1.0, -q.y));
    if (face == 4) return normalize(vec3( q.x, -q.y,  1.0));
    return normalize(vec3(-q.x, -q.y, -1.0));
}
vec2 directionUv(vec3 d) {
    return vec2(atan(d.x, d.z) / (2.0 * PI) + 0.5,
        0.5 - asin(clamp(d.y, -1.0, 1.0)) / PI);
}
// Octahedral unwrap: the sphere folded onto a square, +Y centre, -Y at the corners.
// Matches the Metal pass and upstream's reproject chunk so atlases round-trip.
vec3 octahedralDirection(vec2 st) {
    vec2 f = st * 2.0 - 1.0;
    vec3 n = vec3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    float t = max(-n.y, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.z += (n.z >= 0.0) ? -t : t;
    return normalize(n);
}
vec2 directionUvOctahedral(vec3 d) {
    vec3 a = abs(d);
    float invL1 = 1.0 / max(a.x + a.y + a.z, 1e-8);
    vec2 t = vec2(d.x, d.z) * invL1;
    if (d.y < 0.0) {
        t = (1.0 - abs(t.yx)) * vec2(t.x >= 0.0 ? 1.0 : -1.0, t.y >= 0.0 ? 1.0 : -1.0);
    }
    return t * 0.5 + 0.5;
}
vec3 equirectDirection(vec2 st) {
    float phi = (st.x - 0.5) * 2.0 * PI;
    float theta = (0.5 - st.y) * PI;
    float c = cos(theta);
    return vec3(sin(phi) * c, sin(theta), cos(phi) * c);
}
vec4 sampleDirection(vec3 direction) {
    if (p.flags.x > 0.5) {
        return textureLod(sourceCube, direction, p.misc.y * 5.0);
    }
    vec2 uv = (p.misc.z >= PROJ_OCTAHEDRAL - 0.5)
        ? directionUvOctahedral(direction)
        : directionUv(direction);
    return texture(sourceEquirect, uv);
}
void main() {
    vec2 localUv = (gl_FragCoord.xy - p.rectTarget.xy) /
        max(p.rectTarget.zw, vec2(1.0));
    // A cube-face target overrides the projection; otherwise the target's own
    // projection decides which direction this texel stands for.
    vec3 direction;
    if (p.flags.w >= 0.0) {
        direction = faceDirection(int(p.flags.w + 0.5), localUv);
    } else if (p.misc.w >= PROJ_OCTAHEDRAL - 0.5) {
        direction = octahedralDirection(localUv);
    } else {
        direction = equirectDirection(localUv);
    }
    vec4 sampleValue = sampleDirection(direction);
    if (p.misc.x > 0.5) {
        vec3 up = abs(direction.y) < 0.99 ? vec3(0,1,0) : vec3(1,0,0);
        vec3 tangent = normalize(cross(up, direction));
        vec3 bitangent = cross(direction, tangent);
        vec3 sum = sampleValue.rgb;
        float weight = 1.0;
        for (int i = 0; i < 16; ++i) {
            float a = 6.28318530718 * (float(i) + 0.5) / 16.0;
            float r = p.misc.y * sqrt((float(i) + 0.5) / 16.0);
            vec3 d = normalize(direction + r *
                (cos(a) * tangent + sin(a) * bitangent));
            float w = max(dot(direction, d), 0.0);
            sum += sampleDirection(d).rgb * w;
            weight += w;
        }
        sampleValue.rgb = sum / weight;
    }
    vec3 color = p.flags.y > 0.5
        ? pow(max(sampleValue.rgb, vec3(0.0)), vec3(2.2))
        : sampleValue.rgb;
    if (p.flags.z > 0.5) {
        float peak = max(color.r, max(color.g, color.b));
        float a = clamp((8.0 - sqrt(max(peak, 0.0))) / 7.0, 0.0, 1.0);
        color = sqrt(max(color, vec3(0.0))) / max(8.0 - 7.0 * a, 1e-5);
        outColor = vec4(color, a);
    } else {
        outColor = vec4(color, sampleValue.a);
    }
}
