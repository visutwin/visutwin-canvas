#version 450
layout(set=1,binding=0) uniform sampler2D colorMap;
layout(location=0) in vec2 inUv;
layout(location=1) in vec4 inColor;
layout(location=2) in float inHasMap;
layout(location=0) out vec4 outColor;
void main() {
    vec4 texel=vec4(1);
    if(inHasMap>0.5) texel=texture(colorMap,inUv);
    else { float a=clamp(1.0-length(fract(inUv)*2.0-1.0),0.0,1.0); texel.a=a*a; }
    outColor=vec4(inColor.rgb*texel.rgb,texel.a*inColor.a);
}
