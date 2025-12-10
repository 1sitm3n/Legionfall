#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inOffset;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inScale;

layout(location = 0) out vec3 fragColor;

layout(push_constant) uniform PushConstants {
    vec2 viewScale;
    vec2 viewOffset;
} pc;

void main() {
    vec2 worldPos = inPosition * inScale + inOffset;
    vec2 ndcPos = (worldPos + pc.viewOffset) * pc.viewScale;
    gl_Position = vec4(ndcPos, 0.0, 1.0);
    fragColor = inColor;
}