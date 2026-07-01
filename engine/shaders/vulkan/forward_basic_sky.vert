#version 450

// Skybox vertex stage.  Identical to forward_basic.vert except the clip-space
// depth is pinned to the far plane (z = w → depth 1.0 after the perspective
// divide).  The sky shell is authored at a huge radius so it always clears the
// near plane; pinning depth keeps it from being far-clipped and ensures it
// renders behind all scene geometry.  Selected by the pipeline only for
// materials flagged isSkybox, so normal geometry never pays for it.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV0;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec2 inUV1;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
} pc;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragWorldNormal;
layout(location = 2) out vec2 fragUV0;
layout(location = 3) out vec2 fragUV1;
layout(location = 4) out vec4 fragWorldTangent;
layout(location = 5) out float fragViewDepth;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position = (pc.viewProjection * worldPos).xyww;

    // Skybox never samples shadows, but the fragment stage declares this
    // varying — write it so the stage interfaces match.
    fragViewDepth = gl_Position.w;

    fragWorldPos = worldPos.xyz;

    mat3 normalMatrix = mat3(pc.model);
    fragWorldNormal = normalize(normalMatrix * inNormal);
    fragWorldTangent = vec4(normalize(normalMatrix * inTangent.xyz), inTangent.w);
    fragUV0 = inUV0;
    fragUV1 = inUV1;
}
