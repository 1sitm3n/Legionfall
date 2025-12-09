#version 450

// Per-vertex
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec3 inColor;

// Per-instance
layout(location = 2) in vec2 inOffset;
layout(location = 3) in vec3 inInstanceColor;

layout(location = 0) out vec3 outColor;

void main() {
    vec2 worldPos = inPos + inOffset;
    gl_Position = vec4(worldPos, 0.0, 1.0);
    outColor = inColor * inInstanceColor;
}
