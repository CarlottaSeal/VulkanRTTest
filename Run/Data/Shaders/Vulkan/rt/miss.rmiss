#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 16, rgba8) uniform image2D albedoImage;
layout(set = 0, binding = 18, r32ui) uniform uimage2D matIdImage;
layout(location = 0) rayPayloadInEXT vec3 payloadColor;

void main()
{
    payloadColor  = vec3(0.0);   // black sky — no environment contribution
    // Sky sentinel: alpha=0 marks "no geometry", raygen detects this and
    // writes the sky color directly without going through TAA or the filter
    // (TAA reprojection on a sky pixel uses stale hitWorld → ghost trails).
    imageStore(albedoImage, ivec2(gl_LaunchIDEXT.xy), vec4(1.0, 1.0, 1.0, 0.0));
    imageStore(matIdImage,  ivec2(gl_LaunchIDEXT.xy), uvec4(0, 0, 0, 0));   // 0 = sky sentinel
}
