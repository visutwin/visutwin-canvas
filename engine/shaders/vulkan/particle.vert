#version 450
struct Particle { vec4 posAge; vec4 velLifetime; vec4 rotSeedSize; };
layout(set=6,binding=0,std430) readonly buffer Particles { Particle values[]; } particles;
layout(set=6,binding=3,std140) uniform RenderParams {
    mat4 modelView; mat4 projection; vec4 animParams; vec4 miscParams;
    vec4 colorLut[16]; vec4 scaleLut[16];
} params;
layout(location=0) out vec2 outUv;
layout(location=1) out vec4 outColor;
layout(location=2) out float outHasMap;
void main() {
    gl_Position=vec4(0,0,2,1); outUv=vec2(0); outColor=vec4(0); outHasMap=params.miscParams.z;
    uint id=gl_InstanceIndex; if(id>=uint(params.miscParams.y)) return;
    Particle p=particles.values[id]; float lifetime=max(p.velLifetime.w,1e-5);
    if(p.posAge.w<0.0||p.posAge.w>lifetime) return;
    float life=clamp(p.posAge.w/lifetime,0.0,1.0), lp=life*15.0;
    int a=int(lp), b=min(a+1,15); float f=fract(lp);
    vec4 color=mix(params.colorLut[a],params.colorLut[b],f);
    float size=mix(params.scaleLut[a].x,params.scaleLut[b].x,f);
    if(color.a<=0.001||size<=0.0001) return;
    vec2 corners[4]=vec2[](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(1,1));
    vec2 corner=corners[gl_VertexIndex]; float angle=p.rotSeedSize.x+p.rotSeedSize.y*p.posAge.w;
    mat2 rotation=mat2(cos(angle),sin(angle),-sin(angle),cos(angle));
    vec4 view=params.modelView*vec4(p.posAge.xyz,1);
    view.xy+=rotation*corner*(size*0.5);
    vec4 clip=params.projection*view; clip.z=0.5*(clip.z+clip.w); gl_Position=clip;
    vec2 tiles=max(params.animParams.xy,vec2(1));
    float frames=max(params.animParams.z,1.0);
    float frame=floor(mod(life*frames*max(params.animParams.w,0.0001),frames));
    vec2 tile=corner*0.5+0.5, origin=vec2(mod(frame,tiles.x),floor(frame/tiles.x));
    outUv=(origin+vec2(tile.x,1.0-tile.y))/tiles;
    outColor=vec4(color.rgb*params.miscParams.x,color.a);
}
