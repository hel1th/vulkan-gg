#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(set = 0, binding = 0, std140) uniform Scene {
    mat4 viewProjection;
    mat4 lightViewProjection;
    vec4 camera;
    vec4 ambient;
    vec4 lightDirection;
    vec4 lightColor;
    ivec4 lightCounts;
} scene;

layout(set = 1, binding = 0, std140) uniform Model {
    mat4 transform;
    mat4 normalMatrix;
    vec4 specular;
} model;

layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;

void main() {
    vec4 worldPosition = model.transform * vec4(inPosition, 1.0);
    gl_Position = scene.viewProjection * worldPosition;
    fragPosition = worldPosition.xyz;
    fragNormal = mat3(model.normalMatrix) * inNormal;
    fragUV = inUV;
}
