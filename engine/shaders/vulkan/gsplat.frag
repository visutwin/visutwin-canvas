#version 450
layout(location=0) in vec2 inUv;
layout(location=1) in vec4 inColor;
layout(location=0) out vec4 outColor;
void main() {
    float q=dot(inUv,inUv); if(q>1.0)discard;
    const float e4=0.018315638889;
    float alpha=((exp(-4.0*q)-e4)/(1.0-e4))*inColor.a;
    outColor=vec4(inColor.rgb*alpha,alpha);
}
