#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_ray_query   : require
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 1)  uniform accelerationStructureEXT topLevelAS;
layout(set = 0, binding = 2)  uniform CameraUBO {
    vec4 eye_aspect;
    vec4 forward_fovTan;
    vec4 right_pad;
    vec4 up_pad;
    vec4 misc;
} cam;
layout(set = 0, binding = 3)  readonly buffer Positions    { float p[]; }  pbuf;
layout(set = 0, binding = 4)  readonly buffer Indices      { uint  i[]; }  ibuf;
layout(set = 0, binding = 5)  readonly buffer MatColors    { float c[]; }  matbuf;
layout(set = 0, binding = 6)  readonly buffer TriMatIds    { uint  m[]; }  trimat;
layout(set = 0, binding = 7)  uniform sampler2D textures[64];
layout(set = 0, binding = 8)  readonly buffer MatTexSlot   { int   s[]; }  matslot;
layout(set = 0, binding = 9)  readonly buffer UVCoords     { float uv[]; } uvbuf;
layout(set = 0, binding = 10) readonly buffer UVIdx        { uint  i[]; }  uvidx;
layout(set = 0, binding = 11) readonly buffer MatNormalSlot{ int   s[]; }  matnslot;
layout(set = 0, binding = 12) readonly buffer Lights       { vec4  d[]; }  lbuf;
// Reservoir slot = 12 ints: lightIdx, wsum, M, pad, normal.xyz, depth,
// hitWorld.xyz, pHat-at-acceptance.
layout(set = 0, binding = 13) buffer Reservoirs0           { int   d[]; }  resA;
layout(set = 0, binding = 14) buffer Reservoirs1           { int   d[]; }  resB;
layout(set = 0, binding = 16, rgba8) uniform image2D albedoImage;
layout(set = 0, binding = 17, rgba16f) uniform image2D momentsImage;
layout(set = 0, binding = 18, r32ui) uniform uimage2D matIdImage;
layout(set = 0, binding = 19, r32f)  uniform image2D  depthImage;

layout(location = 0) rayPayloadInEXT vec3 payloadColor;
hitAttributeEXT vec2 attribs;

vec3 fetchVert(uint idx) { return vec3(pbuf.p[idx*3+0], pbuf.p[idx*3+1], pbuf.p[idx*3+2]); }
vec2 fetchUV  (uint idx) { return vec2(uvbuf.uv[idx*2+0], uvbuf.uv[idx*2+1]); }
vec3 fetchMatColor(uint id) { return vec3(matbuf.c[id*3+0], matbuf.c[id*3+1], matbuf.c[id*3+2]); }

uint wangHash(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x = x ^ (x >> 4u);
    x *= 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return x;
}
float frand(inout uint s) { s = wangHash(s); return float(s) / 4294967296.0; }

struct Reservoir {
    int   lightIdx;
    float wsum;
    int   M;
    vec3  normal;
    float depth;
    vec3  hitWorld;
    float pHatStored;
};

void rUpdate(inout Reservoir r, int idx, float w, float pHat, inout uint rng) {
    r.wsum += w;
    r.M    += 1;
    if (frand(rng) < w / max(r.wsum, 1e-9)) {
        r.lightIdx   = idx;
        r.pHatStored = pHat;
    }
}
void rUpdateNoM(inout Reservoir r, int idx, float w, float pHat, inout uint rng) {
    r.wsum += w;
    if (frand(rng) < w / max(r.wsum, 1e-9)) {
        r.lightIdx   = idx;
        r.pHatStored = pHat;
    }
}

Reservoir readResAt(uint pixIdx, bool fromA) {
    Reservoir r;
    uint b = pixIdx * 12u;
    if (fromA) {
        r.lightIdx   = resA.d[b+0];
        r.wsum       = intBitsToFloat(resA.d[b+1]);
        r.M          = resA.d[b+2];
        r.normal     = vec3(intBitsToFloat(resA.d[b+4]),
                            intBitsToFloat(resA.d[b+5]),
                            intBitsToFloat(resA.d[b+6]));
        r.depth      = intBitsToFloat(resA.d[b+7]);
        r.hitWorld   = vec3(intBitsToFloat(resA.d[b+8]),
                            intBitsToFloat(resA.d[b+9]),
                            intBitsToFloat(resA.d[b+10]));
        r.pHatStored = intBitsToFloat(resA.d[b+11]);
    } else {
        r.lightIdx   = resB.d[b+0];
        r.wsum       = intBitsToFloat(resB.d[b+1]);
        r.M          = resB.d[b+2];
        r.normal     = vec3(intBitsToFloat(resB.d[b+4]),
                            intBitsToFloat(resB.d[b+5]),
                            intBitsToFloat(resB.d[b+6]));
        r.depth      = intBitsToFloat(resB.d[b+7]);
        r.hitWorld   = vec3(intBitsToFloat(resB.d[b+8]),
                            intBitsToFloat(resB.d[b+9]),
                            intBitsToFloat(resB.d[b+10]));
        r.pHatStored = intBitsToFloat(resB.d[b+11]);
    }
    return r;
}
void writeResAt(uint pixIdx, Reservoir r, bool toA) {
    uint b = pixIdx * 12u;
    if (toA) {
        resA.d[b+0]  = r.lightIdx;
        resA.d[b+1]  = floatBitsToInt(r.wsum);
        resA.d[b+2]  = r.M;
        resA.d[b+4]  = floatBitsToInt(r.normal.x);
        resA.d[b+5]  = floatBitsToInt(r.normal.y);
        resA.d[b+6]  = floatBitsToInt(r.normal.z);
        resA.d[b+7]  = floatBitsToInt(r.depth);
        resA.d[b+8]  = floatBitsToInt(r.hitWorld.x);
        resA.d[b+9]  = floatBitsToInt(r.hitWorld.y);
        resA.d[b+10] = floatBitsToInt(r.hitWorld.z);
        resA.d[b+11] = floatBitsToInt(r.pHatStored);
    } else {
        resB.d[b+0]  = r.lightIdx;
        resB.d[b+1]  = floatBitsToInt(r.wsum);
        resB.d[b+2]  = r.M;
        resB.d[b+4]  = floatBitsToInt(r.normal.x);
        resB.d[b+5]  = floatBitsToInt(r.normal.y);
        resB.d[b+6]  = floatBitsToInt(r.normal.z);
        resB.d[b+7]  = floatBitsToInt(r.depth);
        resB.d[b+8]  = floatBitsToInt(r.hitWorld.x);
        resB.d[b+9]  = floatBitsToInt(r.hitWorld.y);
        resB.d[b+10] = floatBitsToInt(r.hitWorld.z);
        resB.d[b+11] = floatBitsToInt(r.pHatStored);
    }
}

// pHat including visibility (Bitterli Alg. 3). Inline rayQuery is much
// cheaper than traceRayEXT here because we fire ~23 shadow rays per pixel.
float pHatVisible(int idx, vec3 hitPos, vec3 N) {
    if (idx < 0) return 0.0;
    vec3  lp        = lbuf.d[idx*2 + 0].xyz;
    float intensity = lbuf.d[idx*2 + 0].w;
    vec3  toL = lp - hitPos;
    float d   = length(toL);
    if (d < 1e-4 || intensity <= 0.0) return 0.0;
    vec3  L   = toL / d;
    float NdL = max(dot(N, L), 0.0);
    if (NdL <= 0.0) return 0.0;

    rayQueryEXT q;
    rayQueryInitializeEXT(q, topLevelAS,
        gl_RayFlagsTerminateOnFirstHitEXT |
        gl_RayFlagsOpaqueEXT |
        gl_RayFlagsSkipClosestHitShaderEXT,
        0xFF,
        hitPos + N * 0.001, 0.0, L, d - 0.01);
    while (rayQueryProceedEXT(q)) { }
    if (rayQueryGetIntersectionTypeEXT(q, true)
        != gl_RayQueryCommittedIntersectionNoneEXT) {
        return 0.0;
    }
    return NdL * intensity / (d * d);
}

bool similarSurface(vec3 nA, float dA, vec3 nB, float dB) {
    return dot(nA, nB) > 0.99
        && abs(dA - dB) <= max(dA, dB) * 0.01;
}

void main()
{
    const uint primId = uint(gl_PrimitiveID);
    const uint i0 = ibuf.i[primId*3+0];
    const uint i1 = ibuf.i[primId*3+1];
    const uint i2 = ibuf.i[primId*3+2];

    const vec3 p0 = fetchVert(i0);
    const vec3 p1 = fetchVert(i1);
    const vec3 p2 = fetchVert(i2);
    const vec2 uv0 = fetchUV(uvidx.i[primId*3+0]);
    const vec2 uv1 = fetchUV(uvidx.i[primId*3+1]);
    const vec2 uv2 = fetchUV(uvidx.i[primId*3+2]);

    vec3 normalObj = normalize(cross(p1 - p0, p2 - p0));

    const vec3 e1 = p1 - p0;
    const vec3 e2 = p2 - p0;
    const vec2 d1 = uv1 - uv0;
    const vec2 d2 = uv2 - uv0;
    const float det = d1.x * d2.y - d2.x * d1.y;
    const float invDet = (abs(det) > 1e-8) ? (1.0 / det) : 0.0;
    vec3 tangentObj   = normalize((e1 * d2.y - e2 * d1.y) * invDet);
    vec3 bitangentObj = normalize((e2 * d1.x - e1 * d2.x) * invDet);

    const mat3 objToWorld3 = mat3(gl_ObjectToWorldEXT);
    vec3 N = normalize(objToWorld3 * normalObj);
    vec3 T = normalize(objToWorld3 * tangentObj);
    vec3 B = normalize(objToWorld3 * bitangentObj);
    if (dot(N, gl_WorldRayDirectionEXT) > 0.0) { N = -N; T = -T; B = -B; }

    const vec3  hitWorld = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    const float hitDepth = gl_HitTEXT;
    const float bw = 1.0 - attribs.x - attribs.y;
    const vec2  uv = uv0*bw + uv1*attribs.x + uv2*attribs.y;

    const uint matId    = trimat.m[primId];
    const int  diffSlot = matslot.s[matId];
    const int  normSlot = matnslot.s[matId];

    // RT has no auto-derivative; pick mip from hit distance.
    const float texLod = clamp(log2(gl_HitTEXT / 16.0), 0.0, 3.0);

    vec3 baseColor = (diffSlot >= 0)
                     ? textureLod(textures[nonuniformEXT(diffSlot)], uv, texLod).rgb
                     : fetchMatColor(matId);

    vec3 shadingN = N;
    if (normSlot >= 0) {
        vec3 nmap = textureLod(textures[nonuniformEXT(normSlot)], uv, texLod).rgb * 2.0 - 1.0;
        shadingN = normalize(T * nmap.x + B * nmap.y + N * nmap.z);
    }

    const uint frameId   = floatBitsToUint(cam.misc.x) & 0x3FFFFFFFu;
    const uint numLights = floatBitsToUint(cam.misc.y) & 0x3FFFFFFFu;
    uint rng = wangHash(uint(gl_LaunchIDEXT.x) * 1973u
                      + uint(gl_LaunchIDEXT.y) * 9277u
                      + frameId * 26699u);

    const bool readA = (frameId & 1u) == 0u;

    Reservoir r;
    r.lightIdx   = -1;
    r.wsum       = 0.0;
    r.M          = 0;
    r.normal     = N;
    r.depth      = hitDepth;
    r.hitWorld   = hitWorld;
    r.pHatStored = 0.0;

    const int kCandidateCount = 16;
    for (int s = 0; s < kCandidateCount; ++s) {
        int   idx  = clamp(int(frand(rng) * float(numLights)), 0, int(numLights) - 1);
        float pHat = pHatVisible(idx, hitWorld, shadingN);
        if (pHat > 0.0) {
            rUpdate(r, idx, pHat * float(numLights), pHat, rng);
        } else {
            r.M += 1;   // M still counts the proposal for unbiased W
        }
    }

    const int kMaxM = 12;

    // Temporal reuse — same pixel, gated by surface similarity.
    const ivec2 launchID   = ivec2(gl_LaunchIDEXT.xy);
    const ivec2 launchSize = ivec2(gl_LaunchSizeEXT.xy);
    const uint  pixIdx     = uint(launchID.y) * uint(launchSize.x) + uint(launchID.x);
    Reservoir prev = readResAt(pixIdx, readA);
    if (prev.M > kMaxM) {
        prev.wsum *= float(kMaxM) / float(prev.M);
        prev.M     = kMaxM;
    }
    if (prev.M > 0 && prev.lightIdx >= 0 && uint(prev.lightIdx) < numLights
        && similarSurface(prev.normal, prev.depth, N, hitDepth)
        && prev.pHatStored > 0.0)
    {
        float pHat_curr = pHatVisible(prev.lightIdx, hitWorld, shadingN);
        if (pHat_curr > 0.0) {
            float weight = prev.wsum * (pHat_curr / prev.pHatStored);
            int   origM  = r.M;
            rUpdateNoM(r, prev.lightIdx, weight, pHat_curr, rng);
            r.M = origM + prev.M;
        }
    }

    // Spatial reuse — concentric-disk taps.
    const int   kSpatialTaps   = 6;
    const float kSpatialRadius = 10.0;
    for (int t = 0; t < kSpatialTaps; ++t) {
        vec2 u = vec2(frand(rng), frand(rng)) * 2.0 - 1.0;
        vec2 disk;
        if (u.x == 0.0 && u.y == 0.0) {
            disk = vec2(0.0);
        } else {
            float r2, theta;
            if (abs(u.x) > abs(u.y)) {
                r2 = u.x;
                theta = (3.14159265 / 4.0) * (u.y / u.x);
            } else {
                r2 = u.y;
                theta = 3.14159265 / 2.0 - (3.14159265 / 4.0) * (u.x / u.y);
            }
            disk = r2 * vec2(cos(theta), sin(theta));
        }
        ivec2 nl = launchID + ivec2(disk * kSpatialRadius);
        if (nl == launchID) continue;
        if (nl.x < 0 || nl.y < 0 || nl.x >= launchSize.x || nl.y >= launchSize.y) continue;
        uint nPixIdx = uint(nl.y) * uint(launchSize.x) + uint(nl.x);

        Reservoir nr = readResAt(nPixIdx, readA);
        if (nr.M > kMaxM) {
            nr.wsum *= float(kMaxM) / float(nr.M);
            nr.M     = kMaxM;
        }
        if (nr.M <= 0 || nr.lightIdx < 0 || uint(nr.lightIdx) >= numLights) continue;
        if (!similarSurface(nr.normal, nr.depth, N, hitDepth)) continue;
        if (nr.pHatStored <= 0.0) continue;

        float pHat_curr = pHatVisible(nr.lightIdx, hitWorld, shadingN);
        if (pHat_curr <= 0.0) continue;

        float weight = nr.wsum * (pHat_curr / nr.pHatStored);
        int origM = r.M;
        rUpdateNoM(r, nr.lightIdx, weight, pHat_curr, rng);
        r.M = origM + nr.M;
    }

    // Chosen sample is guaranteed visible (visibility is in pHat), so no
    // separate final shadow ray.
    vec3 lightContrib = vec3(0.0);
    if (r.lightIdx >= 0 && r.M > 0 && r.pHatStored > 0.0) {
        int   idx       = r.lightIdx;
        vec3  lp        = lbuf.d[idx*2 + 0].xyz;
        float intensity = lbuf.d[idx*2 + 0].w;
        vec3  lcol      = lbuf.d[idx*2 + 1].rgb;
        vec3  toL = lp - hitWorld;
        float d   = length(toL);
        vec3  L   = toL / max(d, 1e-6);
        float NdL = max(dot(shadingN, L), 0.0);

        float W = r.wsum / (float(r.M) * r.pHatStored);
        lightContrib = NdL * intensity * lcol / (d * d) * W;
    }

    r.normal   = N;
    r.depth    = hitDepth;
    r.hitWorld = hitWorld;
    writeResAt(pixIdx, r, !readA);

    const vec3 finalColor = lightContrib;

    // Welford EWMA — storing M2 directly would cancel against M1² in fp16.
    // momentsImage layout: r=M1, g=variance, b=historyLen, a=variance copy.
    {
        const float lum = dot(finalColor, vec3(0.299, 0.587, 0.114));
        const vec4  prevMoments = imageLoad(momentsImage, ivec2(gl_LaunchIDEXT.xy));
        const float prevM1   = prevMoments.r;
        const float prevVar  = prevMoments.g;
        const float prevHist = prevMoments.b;
        const bool  prevSurfaceMatch = (prev.M > 0
                                     && similarSurface(prev.normal, prev.depth, N, hitDepth));
        const bool  momentsValid = prevSurfaceMatch
                                && prevHist >= 1.0
                                && prevHist <= 64.0;
        float histLen, M1, variance;
        if (momentsValid) {
            histLen = min(prevHist + 1.0, 32.0);
            float a = max(1.0 / histLen, 0.05);
            float delta = lum - prevM1;
            M1       = prevM1 + a * delta;
            variance = (1.0 - a) * (prevVar + a * delta * delta);
        } else {
            histLen  = 1.0;
            M1       = lum;
            variance = 0.0;
        }
        imageStore(momentsImage, ivec2(gl_LaunchIDEXT.xy),
                   vec4(M1, variance, histLen, variance));
    }

    // alpha=1 = real surface (miss writes 0 for sky).
    imageStore(albedoImage, ivec2(gl_LaunchIDEXT.xy), vec4(baseColor, 1.0));
    imageStore(matIdImage,  ivec2(gl_LaunchIDEXT.xy), uvec4(matId + 1u, 0, 0, 0));
    imageStore(depthImage,  ivec2(gl_LaunchIDEXT.xy), vec4(1.0, 0, 0, 0));

    payloadColor = finalColor;
}
