#version 450
layout(set=1,binding=0) uniform sampler2D colorMap;
layout(location=0) in vec2 inUv;
layout(location=1) in vec4 inColor;
layout(location=2) in float inHasMap;
layout(location=3) flat in vec3 inOutput; // exposure, tone mapping mode, linear HDR target
layout(location=0) out vec4 outColor;

// The forward pass's tone mapping operators, without its lighting-block dispatch.
#define VT_TONEMAP_OPERATORS_ONLY
#include "chunks/common-tonemap.glsl"

void main() {
    vec4 texel=vec4(1);
    if(inHasMap>0.5){
        texel=texture(colorMap,inUv);
        // The colour map is sRGB-authored (upstream loads every one with srgb: true).
        texel.rgb=pow(max(texel.rgb,vec3(0)),vec3(2.2));
    }
    else { float a=clamp(1.0-length(fract(inUv)*2.0-1.0),0.0,1.0); texel.a=a*a; }
    // Upstream particle_end: the colour is linear; a gamma target tone-maps it with the
    // scene's exposure and encodes it, a camera frame's linear HDR scene leaves both to compose.
    vec3 rgb=inColor.rgb*texel.rgb;
    if(inOutput.z<0.5){
        rgb=toneMapExposed(rgb,inOutput.x,int(inOutput.y+0.5));
        rgb=pow(max(rgb,vec3(0))+0.0000001,vec3(1.0/2.2));
    }
    outColor=vec4(rgb,texel.a*inColor.a);
}
