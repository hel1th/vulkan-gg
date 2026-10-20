#version 450

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec3 fragNormal;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, std140) uniform Scene {
    mat4 viewProjection;
    vec4 camera;
    vec4 ambient;
    vec4 lightDirection;
    vec4 lightColor;
    ivec4 lightCounts;
} scene;

layout(set = 1, binding = 0, std140) uniform Model {
    mat4 transform;
    mat4 normalMatrix;
    vec4 albedo;
    vec4 specular;
} model;

struct PointLight {
    vec4 position;
    vec4 color;
};

struct SpotLight {
    PointLight light;
    vec4 direction;
    vec4 angles;
};

layout(set = 0, binding = 1, std430) readonly buffer PointLights {
    PointLight points[];
};

layout(set = 0, binding = 2, std430) readonly buffer SpotLights {
    SpotLight spots[];
};

vec3 unitVector(vec3 v) {
    float lengthSquared = dot(v, v);
    return lengthSquared > 0.0 ? v * inversesqrt(lengthSquared) : vec3(0.0);
}

vec3 blinnPhong(vec3 n, vec3 v, vec3 l, vec4 lightColor) {
    float diffuse = max(dot(n, l), 0.0);
    if (diffuse == 0.0) {
        return vec3(0.0);
    }
    vec3 halfway = unitVector(l + v);
    float specular = pow(max(dot(n, halfway), 0.0), model.specular.w);
    
    return (model.albedo.rgb * diffuse + model.specular.rgb * specular)
         * lightColor.rgb * lightColor.w;
}

void main() {
    vec3 n = normalize(fragNormal);
    vec3 v = unitVector(scene.camera.xyz - fragPosition * scene.camera.w);

    // Ambient and directional
    vec3 color = model.albedo.rgb * scene.ambient.rgb * scene.ambient.w;
    color += blinnPhong(n, v, unitVector(-scene.lightDirection.xyz), scene.lightColor);

    // Point lights
    for (int i = 0; i < scene.lightCounts.x; ++i) {
        vec3 toLight = points[i].position.xyz - fragPosition;
        float distanceSquared = max(dot(toLight, toLight), 0.0001);
        color += blinnPhong(n, v, unitVector(toLight), points[i].color) / distanceSquared;
    }

    // Spotlights
    for (int i = 0; i < scene.lightCounts.y; ++i) {
        vec3 toLight = spots[i].light.position.xyz - fragPosition;
        vec3 l = unitVector(toLight);
        float distanceSquared = max(dot(toLight, toLight), 0.0001);
        float angle = dot(-l, unitVector(spots[i].direction.xyz));
        float inner = cos(radians(spots[i].angles.x));
        float outer = cos(radians(spots[i].angles.y));
        float edge = inner > outer ? smoothstep(outer, inner, angle) : step(outer, angle);
        color += blinnPhong(n, v, l, spots[i].light.color) * edge / distanceSquared;
    }

    outColor = vec4(color, 1.0);
}
