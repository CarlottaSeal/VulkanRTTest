# ReSTIR DI in VulkanRTTest

Tracks the per-step build of Reservoir-based SpatioTemporal Importance
Resampling (Direct Illumination) on top of the existing Vulkan KHR
ray-tracing path. Reference: Bitterli et al. 2020 — *Spatiotemporal
reservoir resampling for real-time ray tracing with dynamic direct
lighting*.

## Goal

A scene with N (≈ 256) point lights, where naive 1-spp picking-one-light
per-pixel would be very noisy, becomes converging-quality real-time
through:
1. RIS (Resampled Importance Sampling) per pixel
2. Temporal reuse (prev-frame reservoir at same pixel)
3. Spatial reuse (combining neighbor pixels' reservoirs)

## Pipeline overview

```
per-frame:
  raygen
    primary ray → hit point P, normal N, shading data
    [step 1] generate M=8 candidate lights, RIS → currentReservoir
    [step 3] combine currentReservoir with prevReservoir (temporal)
    [step 4] combine with K neighbors' reservoirs (spatial)
    [step 2] shadow ray to chosen.lightIdx; if hit → reject
    final = chosen.contribution * reservoir.W
    write currentReservoir to ping-pong buffer (prev-for-next-frame)
```

For now we'll do everything inside the closest-hit shader for
simplicity. A proper implementation splits initial-RIS / spatial /
shading into separate passes — TODO once temporal + spatial are in.

## Reservoir layout

```
struct Reservoir {
    int   lightIdx;      // surviving sample
    float weightSum;     // sum of source weights (RIS W)
    int   M;             // sample count seen
    float W;             // 1 / pHat(chosen) * weightSum / M  (RIS estimator weight)
};
```

GPU storage: 16 bytes per pixel. Two textures (ping/pong) at swap
extent for temporal, or two storage buffers indexed by `pixelIdx`.

## Light source

- 256 point lights, randomly distributed inside Sponza's bounding box
  (engine units after axis remap).
- 32 bytes per light: `vec4(pos.xyz, intensity)` + `vec4(color.rgb, _pad)`.

## Steps

| # | Status | What |
|---|--------|------|
| 0 | DONE  | Lights SSBO at binding 12, UBO carries frameId + numLights, App generates 256 random lights distributed in Sponza. Closesthit still uses directional sun; lights buffer just sits there until step 1. |
| 1 | DONE  | RIS in closesthit: M=8 candidates, weighted-reservoir picks one, shadow ray to chosen. Verified — heavy noise + dim shadows because single-frame 1-spp RIS has huge variance and many close lights. This is the pre-ReSTIR baseline. |
| 2+3 | DONE  | Reservoir SSBO at binding 13, zero-init via vkCmdFillBuffer. Closesthit: initial RIS M=16, temporal merge with prev pixel's reservoir (M-clamp scales wsum proportionally; back-face reject; FTZ fix on frameId via 0x40000000 OR-bias to avoid GPU flushing the bit-reinterpreted uint to zero). |
| 4 | PARTIAL | Second reservoir buffer + biased spatial reuse landed but disabled — the no-MIS-reweight version produces block-shaped flicker because neighbor wsums baked in the neighbor's surface pHat. Re-enable after step 5 lands. |
| 6 | DONE  | Output TAA: rgba16f history image at binding 15. Raygen blends 0.1·new + 0.9·history per frame, Reinhard tone-maps the HDR result before the rgba8 swap blit (per-frame fireflies clamped to 4.0 before TAA so a single bright sample can't lock the history high). Confirmed working via fixed-write + monotonic-accumulation tests; user-visible smoothing of temporal flicker. Spatial salt-and-pepper noise remains — that's step 5's job. |
| 5 | NEXT  | Geometric similarity gate for spatial reuse: store world-space normal + depth in reservoir (16B → 32B), reject neighbor merge when normal/depth differs too much. Re-enables step 4. |
| 4 | TODO  | Spatial reuse: 5-tap neighborhood RIS-merge over current reservoirs |
| 5 | TODO  | Bias correction: similarity test (depth, normal) before accepting reuse |
| 6 | TODO  | Split into separate passes (rgen → spatial → shading) |

## Known issues / decisions

- Skipping geometry term in `pHat` (treating it as plain `light.intensity * NdotL / d²`) — fine for point lights, would need solid-angle-of-area-light for area lights later.
- Shadow ray tmax = `dist - 0.01` so the ray doesn't shoot past the light.
- Drop the directional sun once point lights are dense enough; keep hemispheric ambient as fill.

## Files

- `Code/Game/App.cpp` — light generation, calls `SetLights`
- `Code/Game/Game.cpp` — passes frameId / numLights into `UpdateCameraVectors`
- `Run/Data/Shaders/Vulkan/rt/closesthit.rchit` — RIS / reservoir / shading
- `Engine/Renderer/VulkanRTPath.{h,cpp}` — bindings 12 (lights) and 13 (reservoirs)
