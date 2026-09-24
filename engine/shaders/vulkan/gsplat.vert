#version 450
layout(set=6,binding=0,std430) readonly buffer Splats { uint words[]; } splats;
layout(set=6,binding=1,std430) readonly buffer Order { uint values[]; } orderBuffer;
layout(set=6,binding=2,std430) readonly buffer SH { float values[]; } sh;
layout(set=6,binding=3,std140) uniform Params {
    mat4 modelView; mat4 projection; vec4 viewport;
    uint splatCount; uint shBands; uvec2 padding;
    vec4 fogColor;   // linear rgb
    vec4 fogParams;  // start, end, density, type (0 = none, 1 linear, 2 exp, 3 exp2)
    vec4 outputParams; // exposure, tone mapping mode, linear HDR target, unused
} params;

// The forward pass's tone mapping operators, without its lighting-block dispatch.
#define VT_TONEMAP_OPERATORS_ONLY
#include "chunks/common-tonemap.glsl"

// Upstream gsplatOutput's prepareOutputFromGamma; twin of gsplatPrepareOutput in
// gsplat-render.metal. A camera frame's scene pass is linear HDR with tone mapping
// left to compose; any other target is gamma, tonemapped and encoded here. Exposure
// scales before the curve and is skipped with it for NONE, as the Metal toneMap does.
vec3 gsplatPrepareOutput(vec3 gammaColor, float depth) {
    bool linearTarget = params.outputParams.z > 0.5;
    uint fogType = uint(params.fogParams.w + 0.5);
    bool fog = fogType != 0u;
    bool tonemap = !linearTarget && params.outputParams.y < 5.5;

    vec3 color = gammaColor;
    if (tonemap || linearTarget || fog) {
        color = pow(max(color, vec3(0.0)), vec3(2.2));
    }
    if (fog) {
        float density = max(params.fogParams.z, 0.0);
        float fogFactor;
        if (fogType == 1u) {
            float fogEnd = max(params.fogParams.y, params.fogParams.x + 1e-3);
            fogFactor = (fogEnd - depth) / (fogEnd - params.fogParams.x);
        } else if (fogType == 2u) {
            fogFactor = exp(-depth * density);
        } else {
            float d = depth * density;
            fogFactor = exp(-d * d);
        }
        color = mix(params.fogColor.xyz, color, clamp(fogFactor, 0.0, 1.0));
    }
    if (tonemap) {
        color = toneMapByMode(color * max(params.outputParams.x, 0.0), int(params.outputParams.y + 0.5));
    }
    if (tonemap || (!linearTarget && fog)) {
        color = pow(max(color, vec3(0.0)) + 0.0000001, vec3(1.0 / 2.2));
    }
    return color;
}
layout(location=0) out vec2 outUv;
layout(location=1) out vec4 outColor;
vec3 load3(uint base) { return vec3(uintBitsToFloat(splats.words[base]),uintBitsToFloat(splats.words[base+1]),uintBitsToFloat(splats.words[base+2])); }
vec4 unpackColor(uint c) { return vec4(c&255u,(c>>8)&255u,(c>>16)&255u,(c>>24)&255u)/255.0; }
vec3 shv(uint base, uint coefficient) {
    uint offset=base+coefficient*3u;
    return vec3(sh.values[offset],sh.values[offset+1u],sh.values[offset+2u]);
}
vec3 evaluateSH(uint base, uint bands, vec3 d) {
    float x=d.x,y=d.y,z=d.z;
    vec3 result=0.4886025119029199*(-shv(base,0)*y+shv(base,1)*z-shv(base,2)*x);
    if(bands>1u) {
        float xx=x*x,yy=y*y,zz=z*z;
        result+=shv(base,3)*(1.0925484305920792*x*y)
            +shv(base,4)*(-1.0925484305920792*y*z)
            +shv(base,5)*(0.31539156525252005*(2.0*zz-xx-yy))
            +shv(base,6)*(-1.0925484305920792*x*z)
            +shv(base,7)*(0.5462742152960396*(xx-yy));
    }
    if(bands>2u) {
        float xx=x*x,yy=y*y,zz=z*z;
        result+=shv(base,8)*(-0.5900435899266435*y*(3.0*xx-yy))
            +shv(base,9)*(2.890611442640554*x*y*z)
            +shv(base,10)*(-0.4570457994644658*y*(4.0*zz-xx-yy))
            +shv(base,11)*(0.3731763325901154*z*(2.0*zz-3.0*xx-3.0*yy))
            +shv(base,12)*(-0.4570457994644658*x*(4.0*zz-xx-yy))
            +shv(base,13)*(1.445305721320277*z*(xx-yy))
            +shv(base,14)*(-0.5900435899266435*x*(xx-3.0*yy));
    }
    return result;
}
void main() {
    gl_Position=vec4(0,0,2,1); outUv=vec2(0); outColor=vec4(0);
    uint index=orderBuffer.values[gl_InstanceIndex]; if(index>=params.splatCount)return;
    uint base=index*10u; vec3 center=load3(base); vec3 ca=load3(base+4u), cb=load3(base+7u);
    vec4 view=params.modelView*vec4(center,1), clip=params.projection*view; if(clip.w<=0.0)return;
    mat3 covariance=mat3(ca.x,ca.y,ca.z, ca.y,cb.x,cb.y, ca.z,cb.y,cb.z);
    float focal=params.viewport.x*params.projection[0][0], j=focal/view.z;
    mat3 J=mat3(j,0,-j*view.x/view.z, 0,j,-j*view.y/view.z, 0,0,0);
    mat3 W=transpose(mat3(params.modelView)); mat3 T=W*J;
    mat3 cov=transpose(T)*covariance*T;
    float d1=cov[0][0]+0.3, od=cov[0][1], d2=cov[1][1]+0.3;
    float mid=0.5*(d1+d2), radius=length(vec2(0.5*(d1-d2),od));
    float lambda1=mid+radius, lambda2=max(mid-radius,0.1);
    // Limit the kernel to the smaller viewport dimension (upstream gsplatCorner.js,
    // and the twin of the vmin clamp in gsplat-render.metal). Without it the
    // perspective Jacobian, which divides by view.z, blows the footprint up without
    // bound as a splat centre approaches the camera. That used to be invisible here
    // because such a splat's centre also left the depth range and the whole quad was
    // clipped away; clamping clip.z below keeps it, so the size clamp is what stops
    // it covering the screen. A camera inside a cloud went entirely flat without this.
    float vmin=min(1024.0,min(params.viewport.x,params.viewport.y));
    float l1=2.0*min(sqrt(2.0*lambda1),vmin), l2=2.0*min(sqrt(2.0*lambda2),vmin);
    if(max(l1,l2)<0.5)return;
    vec2 c=clip.ww*params.viewport.zw;
    // Cull against the frustum x/y planes, as Metal and upstream do.
    if(any(greaterThan(abs(clip.xy)-vec2(max(l1,l2))*c, clip.ww)))return;
    vec2 axis=normalize(vec2(od,lambda1-d1)+vec2(1e-8,0));
    vec2 corners[4]=vec2[](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(1,1));
    vec2 uv=corners[gl_VertexIndex];
    clip.xy+=(uv.x*l1*axis+uv.y*l2*vec2(axis.y,-axis.x))*c;
    // Remap GL's [-1,1] clip z onto the [0,1] convention, then keep the splat off the
    // near and far planes — the twin of the clamp in gsplat-render.metal, and upstream's
    // gsplatCenter.js. The whole quad shares its centre's depth, so an unclamped centre
    // crossing the near plane clips the entire splat away while its footprint still
    // covers visible pixels. clip.w > 0 here, the behind-camera case having returned.
    clip.z=clamp(0.5*(clip.z+clip.w), 0.0, clip.w); gl_Position=clip; outUv=uv;
    vec4 color=unpackColor(splats.words[base+3u]); vec3 displayColor=color.rgb;
    if(params.shBands>0u) {
        vec3 direction=normalize(transpose(mat3(params.modelView))*view.xyz);
        displayColor+=evaluateSH(index*45u,params.shBands,direction);
    }
    // Gamma-space colour -> what the target wants (clip.w is the view depth).
    outColor=vec4(gsplatPrepareOutput(max(displayColor,vec3(0)), clip.w),color.a);
}
