#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 16, rgba8) uniform image2D albedoImage;
layout(location = 0) rayPayloadInEXT vec3 payloadColor;

void main()
{
    const float t = clamp(gl_WorldRayDirectionEXT.y * 0.5 + 0.5, 0.0, 1.0);
    payloadColor  = mix(vec3(0.04, 0.06, 0.10), vec3(0.50, 0.70, 1.00), t);
    // Sky pixels still go through raygen's filteredLight * albedo composite —
    // write white so the sky color survives.
    imageStore(albedoImage, ivec2(gl_LaunchIDEXT.xy), vec4(1.0));
}
