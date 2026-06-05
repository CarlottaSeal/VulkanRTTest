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
| 4 | PARTIAL | Second reservoir buffer + biased spatial reuse re-enabled with similarity gate (step 5). Without proper MIS reweighting the patches are smaller but still visible. |
| 6 | DONE  | Output TAA: rgba16f history image at binding 15. Raygen blends 0.1·new + 0.9·history per frame, Reinhard tone-maps the HDR result before the rgba8 swap blit. Per-frame fireflies clamped to 4.0 before TAA. FTZ fix on frameId via 0x40000000 OR-bias / 0x3FFFFFFF mask — without this the bit-reinterpreted small uint gets flushed to zero by the GPU, killing the per-frame RNG variance and the (frameId == 0) branch. |
| 5 | DONE  | Reservoir grew to 32B with world-space normal + depth (`gl_HitTEXT`). Temporal + spatial merges gated by `dot(N1,N2) > 0.97` (~14°) and `\|d1-d2\|/max < 2%`. Spatial taps reduced from 5 to 3 close-range neighbors. |
| 7 (C) | DONE | Unbiased MIS for spatial reuse: reservoir grew to 48B holding hitWorld; spatial weight = pHat_curr / pHat_origin × wsum_neighbor. Temporal merge stays on the static-pixel approximation (pHat_curr ≈ pHat_origin). |
| 8 (B) | DONE | Single-pass 5×5 edge-aware A-Trous in raygen post-TAA. Weights: pow(dot(N1,N2), 8) × exp(-Δd/d × 8). Reads neighbor normals/depth from the PING reservoir buffer (closesthit's read side, fully committed prior frame). |

| 9 | DONE  | Motion-vector reprojection: prev-frame camera cached in UBO; raygen projects current hitWorld through prev camera basis to find prev-frame screen coord. Surface match check (`dot(N) > 0.95` + `Δhit/dist < 5%`) gates whether to use the reprojected history pixel; if rejected, alpha = 1.0 (no history). Eliminates the bulk of camera-rotation ghosting. |
| 10 | DONE | Albedo demodulation: closesthit writes baseColor to a separate G-buffer (binding 16, rgba8); payload carries un-modulated lighting only. Raygen multiplies albedo back after the spatial filter, so texture detail isn't blurred. |

| 11 | DONE  | Multi-pass A-Trous via compute (stride 1/2/4/8/16, ~31-pixel reach). Two RGBA16F ping-pong images. atrous.comp + composite.comp; new compute pipelines + descriptor pools. Edge stops: normal pow(dot,8) + depth exp(-Δd/d × 8) + luminance exp(-ΔL × 4). Reservoir reads use the PONG side (current frame's data, written by closesthit) since the RT→compute barrier guarantees all closesthit writes are visible. |
| 12 | DONE  | Reprojection gate tightened to cos > 0.97 + hit-distance ratio < 2%. Sky pixels short-circuit the TAA path entirely (raygen detects via albedo.a sentinel). |
| 13 | NEXT  | SVGF — variance estimation + variance-driven adaptive filter + variance-aware history rejection (auto noise/ghost balance). |

## Remaining work

The 1-spp ReSTIR DI + multi-pass A-Trous gets close but still leaves some ghost on rotation and a fine-grained residual noise. Closing those needs **SVGF**:
- Per-pixel luminance variance estimate (temporal + spatial joint refinement).
- Variance-adaptive filter strength: high-variance areas get more aggressive blur, low-variance areas stay sharp.
- Variance-driven history rejection so the noise / ghost trade-off becomes automatic instead of a fixed alpha.
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
