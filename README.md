# VulkanRTTest

Real-time direct lighting with **ReSTIR DI** + **SVGF** denoising on
Vulkan KHR ray tracing. Implements Bitterli et al. 2020 Algorithms 1
+ 2 + 3 end to end on Crytek Sponza.

Built on the [Igloo Engine](https://github.com/CarlottaSeal/IglooEngine).
Sibling of [Tessera](https://github.com/CarlottaSeal/Tessera) (the
tile-deferred / TBR demo).

## What's in here

**ReSTIR DI** (`Run/Data/Shaders/Vulkan/rt/closesthit.rchit`)
- Streaming-RIS reservoir, 16 candidates per pixel
- Temporal + spatial reuse with unbiased MIS reweighting
- Visibility-aware target density via inline ray queries —
  `pHat = NdL · intensity · V(x, light) / d²`. Reservoirs only retain
  visible samples, so ghost trails self-correct without manual
  invalidation.
- ~23 shadow rays / pixel total (16 RIS + 1 temporal + 6 spatial)

**SVGF denoiser** (`raygen.rgen` + `atrous.comp`)
- Welford-form running mean + variance (avoids fp16 catastrophic
  cancellation that the textbook M2 - M1² formulation hits in flat
  regions)
- 3×3 gaussian variance prefilter
- 5-pass A-Trous wavelet, strides 1 / 2 / 4 / 8 / 16, B3-spline
  kernel weights, edge stops on normal / depth / luminance /
  **chrominance** (chrominance gate is non-standard — needed because
  ReSTIR DI's per-pixel point-light pick produces high-frequency hue
  variation that SVGF's diffuse-light-after-albedo-demod assumption
  doesn't cover)
- Luminance-preserving Reinhard tone map (per-channel Reinhard
  saturated colored pixels toward white)

**Scene** (`Code/Game/App.cpp`)
- Crytek Sponza (~270K tris, 25 mats), inline OBJ parse
- Bindless descriptor array, 64 textures (diffuse + normal maps)
- Mipmap chain generated on GPU via `vkCmdBlitImage` chain
- Ray-cone-derived LOD in closesthit (`textureLod` with
  hand-computed mip from hit distance — RT has no auto-derivative)
- 16 colored point lights from a 7-color saturated palette

## Performance

RTX 4080 Laptop, 1600×800, no vsync:

| | numbers |
|---|---|
| FPS | ~140 GPU-bound |
| Rays / frame | ~30M (1.28M pixels × 23 shadow rays) |
| Ray throughput | ~4.2 G rays / s |
| % of RTX 30-class peak | 30–50% |

## GPU optimizations (vs the simplest correct version)

- `traceRayEXT` → `rayQueryEXT` for shadow rays (skips `rmiss`
  pipeline switch on each shadow trace)
- Cooperative shared-memory tile in the atrous stride-1 pass —
  reservoir-SSBO traffic dropped from ~1600 to 144 reads / workgroup
- Bindless texture array with `nonuniformEXT` indexing
- Variance prefilter ahead of pass 1 so atrous σ doesn't flicker

## Controls

| key | action |
|---|---|
| `V` | toggle RT path on / off (default: deferred path) |
| WASD + mouse | fly camera (Igloo: x-forward, y-left, z-up) |
| `ESC` | quit |
| ` ` ` (backtick) | dev console |

## Build

Open `VulkanRTTest.sln` in Visual Studio 2022, Release x64.
Requires the Igloo engine repo at `..\Engine\` (sibling directory).

Vulkan extensions used:
`VK_KHR_acceleration_structure`,
`VK_KHR_ray_tracing_pipeline`,
`VK_KHR_ray_query`,
`VK_KHR_deferred_host_operations`,
`VK_KHR_buffer_device_address`,
`VK_EXT_descriptor_indexing`.

## Sponza assets

Sponza OBJ from [jimmiebergmann/Sponza](https://github.com/jimmiebergmann/Sponza).
Place `sponza.obj` + `sponza.mtl` + the `textures/` directory under
`Run/Data/Models/Sponza/`.

## References

- Bitterli, Wyman, Pharr, Shirley, Lefohn, Jarosz. *Spatiotemporal
  Reservoir Resampling for Real-Time Ray Tracing with Dynamic Direct
  Lighting*. SIGGRAPH 2020.
- Schied et al. *Spatiotemporal Variance-Guided Filtering*. HPG 2017.
- HummaWhite/Vulkan-ReSTIR-PT — useful reference for unbiased MIS
  spatial reuse formulation.
