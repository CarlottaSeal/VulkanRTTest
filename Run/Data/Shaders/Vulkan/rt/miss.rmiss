#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 16, rgba8) uniform image2D albedoImage;
layout(set = 0, binding = 18, r32ui) uniform uimage2D matIdImage;
layout(set = 0, binding = 19, r32f)  uniform image2D  depthImage;
layout(location = 0) rayPayloadInEXT vec3 payloadColor;

void main()
{
    payloadColor = vec3(0.0);
    // alpha=0 / matId=0 are sky sentinels for downstream passes.
    imageStore(albedoImage, ivec2(gl_LaunchIDEXT.xy), vec4(1.0, 1.0, 1.0, 0.0));
    imageStore(matIdImage,  ivec2(gl_LaunchIDEXT.xy), uvec4(0, 0, 0, 0));
    imageStore(depthImage,  ivec2(gl_LaunchIDEXT.xy), vec4(1.0, 0, 0, 0));
}
