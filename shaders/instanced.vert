#version 450

// Per-vertex attributes
layout(location = 0) in vec2 inPosition;

// Per-instance attributes
layout(location = 1) in vec2 inOffset;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inScale;

// Output to fragment shader
layout(location = 0) out vec3 fragColor;

// Push constants for view transformation
layout(push_constant) uniform PushConstants {
    vec2 viewScale;    // Converts world coords to NDC
    vec2 viewOffset;   // Camera position
} pc;

void main() {
    // Scale vertex, add instance offset (world position)
    vec2 worldPos = inPosition * inScale + inOffset;
    
    // Transform to NDC using orthographic projection
    vec2 ndcPos = (worldPos - pc.viewOffset) * pc.viewScale;
    
    gl_Position = vec4(ndcPos, 0.0, 1.0);
    fragColor = inColor;
}