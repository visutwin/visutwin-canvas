#version 450
// Twin of particleVS in shaders/metal/embedded/particle-render.metal.
struct Particle { vec4 posAge; vec4 velLifetime; vec4 rotSeedSize; vec4 motion; };
layout(set=6,binding=0,std430) readonly buffer Particles { Particle values[]; } particles;
// A sorted emitter's draw order, and a mesh emitter's vertices (14 floats each); the pool
// when the emitter has neither.
layout(set=6,binding=1,std430) readonly buffer Order { uint values[]; } drawOrder;
layout(set=6,binding=2,std430) readonly buffer MeshVertices { float values[]; } meshVertices;
layout(set=6,binding=3,std140) uniform RenderParams {
    mat4 modelView; mat4 projection; mat4 view;
    vec4 animParams; vec4 miscParams; vec4 motionParams; vec4 outputParams;
    vec4 faceTangent; vec4 faceBinorm; vec4 wrapParams; vec4 emitterPosition; vec4 softParams;
    vec4 lightCube[6];
    vec4 colorLut[16]; vec4 scaleLut[16];
} params;
layout(location=0) out vec2 outUv;
layout(location=1) out vec4 outColor;
layout(location=2) out float outHasMap;
layout(location=3) flat out vec3 outOutput; // outputParams.xyz
layout(location=4) out vec3 outNormal;
layout(location=5) out vec3 outTangent;
layout(location=6) out vec3 outBinormal;
layout(location=7) out float outViewDepth;

uint pcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rnd(uint key) { return float(pcgHash(key) >> 8) * (1.0 / 16777216.0); }
// Upstream's rotate(): a positive angle turns clockwise.
vec2 rotate2(vec2 v, float c, float s) { return vec2(v.x * c + v.y * s, -v.x * s + v.y * c); }

void main() {
    gl_Position=vec4(0,0,2,1); outUv=vec2(0); outColor=vec4(0); outHasMap=params.miscParams.z; outOutput=params.outputParams.xyz;
    outNormal=vec3(0,0,1); outTangent=vec3(1,0,0); outBinormal=vec3(0,1,0); outViewDepth=0.0;
    uint iid=gl_InstanceIndex; if(iid>=uint(params.miscParams.y)) return;
    uint id=params.outputParams.w>0.5?drawOrder.values[iid]:iid;
    Particle p=particles.values[id]; float lifetime=max(p.velLifetime.w,1e-5);
    // Unborn, dead or hidden (upstream particle.js).
    if(p.posAge.w<=0.0||p.posAge.w>lifetime||p.rotSeedSize.w>0.5) return;
    float life=clamp(p.posAge.w/lifetime,0.0,1.0), lp=life*15.0;
    int a=int(lp), b=min(a+1,15); float f=fract(lp);
    vec4 color=mix(params.colorLut[a],params.colorLut[b],f);
    vec4 scaleSample=mix(params.scaleLut[a],params.scaleLut[b],f);
    // A random point between each graph and its graph2, per particle life.
    uint seed=uint(p.motion.w);
    float size=mix(scaleSample.x,scaleSample.y,rnd(seed+20u));
    color.a+=(scaleSample.z-color.a)*rnd(seed+21u);
    if(color.a<=0.001||size<=0.0001) return;

    // Upstream particle_wrap: a world-space particle wraps into a box around the emitter.
    vec3 particlePos=p.posAge.xyz;
    if(params.wrapParams.w>0.5){
        vec3 bounds=params.wrapParams.xyz, rel=particlePos-params.emitterPosition.xyz;
        particlePos=rel-bounds*floor(rel/bounds)-bounds*0.5+params.emitterPosition.xyz;
    }

    vec2 corners[4]=vec2[](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(1,1));
    bool screenSpace=params.motionParams.z>0.5;
    bool useMesh=params.faceBinorm.w>0.5;
    bool customFace=params.faceTangent.w>0.5;
    vec2 corner=useMesh?vec2(0):corners[min(gl_VertexIndex,3)];
    vec4 view=params.modelView*vec4(particlePos,1);
    vec3 viewVelocity=(params.modelView*vec4(p.motion.xyz,0)).xyz;
    vec2 velocityV=viewVelocity.xy;
    if(screenSpace) velocityV.x/=params.motionParams.w;
    velocityV=length(velocityV)>1e-6?normalize(velocityV):velocityV;
    float angle=p.rotSeedSize.x+p.rotSeedSize.y*p.posAge.w;
    if(params.motionParams.x>0.5) angle=atan(velocityV.x,velocityV.y);
    float ca=cos(angle), sa=sin(angle);
    vec2 offset=rotate2(corner,ca,sa);
    vec3 cameraRight=vec3(params.view[0][0],params.view[1][0],params.view[2][0]);
    vec3 cameraUp=vec3(params.view[0][1],params.view[1][1],params.view[2][1]);
    vec3 cameraBack=vec3(params.view[0][2],params.view[1][2],params.view[2][2]);

    vec2 meshUv=vec2(0);
    vec3 worldOffset=vec3(0);
    vec4 clip;
    if(useMesh||customFace){
        if(useMesh){
            // Upstream particle_mesh: the mesh's vertex, turned about z then x by the angle.
            uint base=uint(gl_VertexIndex)*14u;
            vec3 local=vec3(meshVertices.values[base],meshVertices.values[base+1u],meshVertices.values[base+2u]);
            local.xy=rotate2(local.xy,ca,sa);
            local.yz=rotate2(local.yz,ca,sa);
            worldOffset=local;
            meshUv=vec2(meshVertices.values[base+6u],meshVertices.values[base+7u]);
        } else {
            // Upstream particle_customFace: the quad in the plane of the face vectors.
            worldOffset=params.faceTangent.xyz*offset.x+params.faceBinorm.xyz*offset.y;
        }
        vec3 viewOffset=(params.view*vec4(worldOffset*size,0)).xyz;
        if(params.motionParams.y>0.0&&dot(viewOffset.xy,viewOffset.xy)>1e-12){
            vec3 previous=view.xyz-viewVelocity*params.motionParams.y;
            float interpolation=dot(-velocityV,normalize(viewOffset.xy))*0.5+0.5;
            view.xyz=mix(view.xyz,previous,interpolation);
        }
        view.xyz+=viewOffset;
        clip=params.projection*view;
    } else {
        // Upstream's billboard in view space; in SCREEN SPACE the model matrix lands in clip
        // space and the quad is sized in viewport heights, its x scaled by height / width (#9570).
        if(params.motionParams.y>0.0){
            vec3 previous=view.xyz-viewVelocity*params.motionParams.y;
            float interpolation=dot(-velocityV,normalize(offset))*0.5+0.5;
            view.xyz=mix(view.xyz,previous,interpolation);
        }
        vec2 scaled=offset*size;
        if(screenSpace){ scaled.x*=params.motionParams.w; clip=vec4(view.xy+scaled,0,1); }
        else { view.xy+=scaled; clip=params.projection*view; }
        worldOffset=cameraRight*offset.x+cameraUp*offset.y;
    }
    clip.z=0.5*(clip.z+clip.w); gl_Position=clip;
    outViewDepth=-view.z;

    // A lit particle (upstream particle_normal / particle_TBN).
    if(params.lightCube[0].w>0.5){
        outNormal=normalize(worldOffset+cameraBack);
        vec3 t=-cameraRight, bn=-cameraUp;
        outTangent=t*ca-bn*sa;
        outBinormal=t*sa+bn*ca;
        if(params.lightCube[2].w>0.5) outNormal=cameraBack;
    }

    vec2 tiles=max(params.animParams.xy,vec2(1));
    float frames=max(params.animParams.z,1.0);
    // miscParams.w is animIndex: which animation in the sheet to play.
    float frame=floor(mod(life*frames*max(params.animParams.w,0.0001),frames))
        +params.miscParams.w*frames;
    vec2 origin=vec2(mod(frame,tiles.x),floor(frame/tiles.x));
    vec2 tileUv=useMesh?meshUv:vec2(corner.x*0.5+0.5,0.5-corner.y*0.5);
    outUv=(origin+tileUv)/tiles;
    outColor=vec4(color.rgb*params.miscParams.x,color.a);
}
