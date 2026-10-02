#version 450
// Twin of particleFS in shaders/metal/embedded/particle-render.metal.
layout(set=1,binding=0) uniform sampler2D colorMap;
layout(set=1,binding=1) uniform sampler2D normalMap;
// The scene depth grab (a separate image read through the nearest sampler), for softening.
layout(set=3,binding=11) uniform texture2D sceneDepthGrabImage;
layout(set=3,binding=13) uniform sampler nearestClampSampler;
layout(set=6,binding=3,std140) uniform RenderParams {
    mat4 modelView; mat4 projection; mat4 view;
    vec4 animParams; vec4 miscParams; vec4 motionParams; vec4 outputParams;
    vec4 faceTangent; vec4 faceBinorm; vec4 wrapParams; vec4 emitterPosition; vec4 softParams;
    vec4 lightCube[6];
    vec4 colorLut[16]; vec4 scaleLut[16];
} params;
layout(location=0) in vec2 inUv;
layout(location=1) in vec4 inColor;
layout(location=2) in float inHasMap;
layout(location=3) flat in vec3 inOutput; // exposure, tone mapping mode, linear HDR target
layout(location=4) in vec3 inNormal;
layout(location=5) in vec3 inTangent;
layout(location=6) in vec3 inBinormal;
layout(location=7) in float inViewDepth;
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
    float alpha=texel.a*inColor.a;
    vec3 rgb=inColor.rgb*texel.rgb;

    // Upstream particle_soft.
    if(params.softParams.w>0.5){
        float near=params.softParams.y, far=params.softParams.z;
        float raw=texelFetch(sampler2D(sceneDepthGrabImage,nearestClampSampler),ivec2(gl_FragCoord.xy),0).r;
        float depth=(near*far)/(far-raw*(far-near));
        alpha*=clamp(abs(inViewDepth-depth)*params.softParams.x,0.0,1.0);
    }

    // Upstream particle_lighting: the light cube, by Lambert or half Lambert.
    if(params.lightCube[0].w>0.5){
        vec3 normal=normalize(inNormal);
        if(params.lightCube[2].w>0.5){
            vec3 n=normalize(texture(normalMap,inUv).xyz*2.0-1.0);
            normal=normalize(inTangent*n.x+inBinormal*n.y+inNormal*n.z);
        }
        vec3 negNormal, posNormal;
        if(params.lightCube[1].w>0.5){
            negNormal=normal*0.5+0.5; posNormal=-normal*0.5+0.5;
            negNormal*=negNormal; posNormal*=posNormal;
        } else {
            negNormal=max(normal,vec3(0)); posNormal=max(-normal,vec3(0));
        }
        rgb*=negNormal.x*params.lightCube[0].xyz+posNormal.x*params.lightCube[1].xyz+
             negNormal.y*params.lightCube[2].xyz+posNormal.y*params.lightCube[3].xyz+
             negNormal.z*params.lightCube[4].xyz+posNormal.z*params.lightCube[5].xyz;
    }

    // Upstream particle_end: the colour is linear; a gamma target tone-maps it with the
    // scene's exposure and encodes it, a camera frame's linear HDR scene leaves both to compose.
    if(inOutput.z<0.5){
        rgb=toneMapExposed(rgb,inOutput.x,int(inOutput.y+0.5));
        rgb=pow(max(rgb,vec3(0))+0.0000001,vec3(1.0/2.2));
    }
    outColor=vec4(rgb,alpha);
}
