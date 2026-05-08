#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec3 payloadColor;

void main()
{
    const float t = clamp(gl_WorldRayDirectionEXT.y * 0.5 + 0.5, 0.0, 1.0);
    payloadColor  = mix(vec3(0.04, 0.06, 0.10), vec3(0.50, 0.70, 1.00), t);
}
