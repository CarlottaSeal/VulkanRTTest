#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 1)  uniform accelerationStructureEXT topLevelAS;
layout(set = 0, binding = 3)  readonly buffer Positions { float p[]; } pbuf;
layout(set = 0, binding = 4)  readonly buffer Indices   { uint  i[]; } ibuf;
layout(set = 0, binding = 5)  readonly buffer MatColors { float c[]; } matbuf;
layout(set = 0, binding = 6)  readonly buffer TriMatIds { uint  m[]; } trimat;
layout(set = 0, binding = 7)  uniform sampler2D textures[32];
layout(set = 0, binding = 8)  readonly buffer MatTexSlot{ int   s[]; } matslot;
layout(set = 0, binding = 9)  readonly buffer UVCoords  { float uv[]; } uvbuf;
layout(set = 0, binding = 10) readonly buffer UVIdx     { uint  i[]; } uvidx;

layout(location = 0) rayPayloadInEXT vec3 payloadColor;
layout(location = 1) rayPayloadEXT uint shadowed;
hitAttributeEXT vec2 attribs;

vec3 fetchVert(uint idx)
{
    return vec3(pbuf.p[idx * 3 + 0], pbuf.p[idx * 3 + 1], pbuf.p[idx * 3 + 2]);
}
vec2 fetchUV(uint idx)
{
    return vec2(uvbuf.uv[idx * 2 + 0], uvbuf.uv[idx * 2 + 1]);
}
vec3 fetchMatColor(uint id)
{
    return vec3(matbuf.c[id * 3 + 0], matbuf.c[id * 3 + 1], matbuf.c[id * 3 + 2]);
}

void main()
{
    const uint primId = uint(gl_PrimitiveID);
    const uint i0 = ibuf.i[primId * 3 + 0];
    const uint i1 = ibuf.i[primId * 3 + 1];
    const uint i2 = ibuf.i[primId * 3 + 2];

    const vec3 p0 = fetchVert(i0);
    const vec3 p1 = fetchVert(i1);
    const vec3 p2 = fetchVert(i2);

    vec3 normalObj   = normalize(cross(p1 - p0, p2 - p0));
    vec3 normalWorld = normalize(mat3(gl_ObjectToWorldEXT) * normalObj);
    if (dot(normalWorld, gl_WorldRayDirectionEXT) > 0.0)
        normalWorld = -normalWorld;

    const vec3 hitWorld = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;

    // Barycentric UV interpolation, then texture sample (or fall back to
    // per-material color when matTexSlot < 0).
    const vec2 uv0 = fetchUV(uvidx.i[primId * 3 + 0]);
    const vec2 uv1 = fetchUV(uvidx.i[primId * 3 + 1]);
    const vec2 uv2 = fetchUV(uvidx.i[primId * 3 + 2]);
    const float bw = 1.0 - attribs.x - attribs.y;
    const vec2  uv = uv0 * bw + uv1 * attribs.x + uv2 * attribs.y;

    const uint matId = trimat.m[primId];
    const int  slot  = matslot.s[matId];
    vec3 baseColor;
    if (slot >= 0) {
        baseColor = texture(textures[nonuniformEXT(slot)], uv).rgb;
    } else {
        baseColor = fetchMatColor(matId);
    }

    const vec3 lightDir   = normalize(vec3(-0.4, 0.3, 1.0));
    const vec3 lightColor = vec3(1.0, 0.95, 0.85);

    const vec3  skyColor    = vec3(0.50, 0.65, 0.85);
    const vec3  groundColor = vec3(0.30, 0.25, 0.22);
    const float upDot       = clamp(normalWorld.z * 0.5 + 0.5, 0.0, 1.0);
    const vec3  ambient     = mix(groundColor, skyColor, upDot) * 0.55;

    shadowed = 1u;
    traceRayEXT(
        topLevelAS,
        gl_RayFlagsTerminateOnFirstHitEXT |
        gl_RayFlagsOpaqueEXT |
        gl_RayFlagsSkipClosestHitShaderEXT,
        0xFF,
        0, 0, 1,
        hitWorld + normalWorld * 0.001,
        0.0,
        lightDir,
        10000.0,
        1
    );

    const float NdotL    = max(dot(normalWorld, lightDir), 0.0);
    const float visible  = (shadowed == 0u) ? 1.0 : 0.0;
    const vec3  diffuse  = baseColor * NdotL * visible * lightColor;

    const vec3  viewDir  = normalize(-gl_WorldRayDirectionEXT);
    const vec3  halfVec  = normalize(viewDir + lightDir);
    const float NdotH    = max(dot(normalWorld, halfVec), 0.0);
    const float spec     = pow(NdotH, 16.0);
    const vec3  specular = vec3(0.4, 0.38, 0.35) * spec * visible;

    payloadColor = ambient * baseColor + diffuse + specular;
}
