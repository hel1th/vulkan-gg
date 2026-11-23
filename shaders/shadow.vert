#version 450

layout(location = 0) in vec3 inPosition;

layout(set = 0, binding = 0, std140) uniform Scene {
    mat4 viewProjection;
    mat4 lightViewProjection;
} scene;

layout(set = 1, binding = 0, std140) uniform Model {
    mat4 transform;
} model;

void main() {
    gl_Position = scene.lightViewProjection * model.transform * vec4(inPosition, 1.0);
}
