#version 460
#extension GL_EXT_ray_tracing : require

// Payload — same location/type as the rgen.
layout(location = 0) rayPayloadInEXT vec3 payloadColor;
// Built-in barycentrics for the hit.
hitAttributeEXT vec2 attribs;

void main()
{
    // Visualize barycentrics as RGB for the first cut — confirms hit is alive.
    const vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    payloadColor    = bary;
}
