# VulkanRTTest

Real-time direct lighting with **ReSTIR DI** on Vulkan KHR ray tracing,
denoised by a small **kernel-predicting network (KPCN)** that was trained
on captures from this renderer and runs as hand-written Vulkan compute
shaders, the middle layers on the tensor cores. Crytek Sponza, 16 colored
point lights, ~130 FPS at 1728×864 on an RTX 4080 Laptop.

Built on the [Igloo Engine](https://github.com/CarlottaSeal/IglooEngine).
Sibling of [Tessera](https://github.com/CarlottaSeal/Tessera) (the
tile-deferred / TBR demo).

**Scope**: direct illumination only. Indirect / GI is out of scope —
unlit pixels get a small hemisphere ambient stand-in. For real
multi-bounce, see
[LuminaGI](https://github.com/CarlottaSeal/IglooEngine) (real-time
DX12, Lumen-style cards + SDF) or
[LuminaGI-CudaRef](https://github.com/CarlottaSeal/LuminaGI-CudaRef)
(CUDA ground-truth path tracer).

## Status (November 2026): paused

Development is paused because the dev machine's GPU is failing. Under
sustained load (training the denoiser, long benchmark runs) the RTX 4080
Laptop drops off the PCIe bus and needs a reboot to come back: three times
in 36 hours, at as little as ~93 W. Each time Windows logs a PCIe error
(WHEA 17) on the GPU's upstream root port seconds before, and 67 of 72
hardware errors logged over 60 days are PCIe errors reported by the GPU
itself; the vendor's short boot diagnostics still pass. The intermittent
denoiser stalls described below may be the same fault. Work resumes once
the machine is repaired.

Where it stands:

- **Done** (this branch): ReSTIR DI, the GPU neural denoiser with
  tensor-core middle layers, temporal stabilization.
- **Trained, not yet integrated**: a three-stage à-trous kernel head
  (dilations 1 / 2 / 4, 29×29 reach) reached 30.08 dB validation PSNR vs
  28.30 dB for the single 5×5 kernel. The engine and shader side is
  written; it still needs a run in the app before it is committed.
- **Open**: the root cause of the stalls (see *Known issues*).

The previous version of this project denoised with SVGF; that is on the
main branch.

## What's in here

**ReSTIR DI** (`Run/Data/Shaders/Vulkan/rt/closesthit.rchit`)
- Streaming RIS reservoir, 16 candidates per pixel
- Temporal (same pixel) + spatial (6 taps, 10 px disk) reuse, each
  reused sample reweighted to the current pixel's target function;
  M-normalized combine (the biased variant), gated on normal and depth
- Visibility-aware target function via inline ray queries:
  `pHat = NdL · intensity · V(x, light) / d²`. Reservoirs only keep
  visible samples, so no final shadow ray is needed
- ~23 shadow rays / pixel (16 RIS + 1 temporal + 6 spatial)

**Neural denoiser** (`kpcn_*.comp`, `Engine/.../VulkanRTPath.cpp`, `ml/`)
- Input: log1p of the albedo-demodulated noisy light + albedo. Four 3×3
  conv layers (32 channels), a 1×1 conv to 25 logits, softmax → a 5×5
  kernel per pixel applied to the linear noisy light. Untrained it is a
  box filter, which the exporter checks
- Training data from the in-app `denoise_capture` console command: 200
  random poses, noisy frame vs the mean of 512 frames (7.8 GB, not in git).
  L1 on log1p, AdamW, ~4 minutes
- GPU path: fp16 activations ping-ponged between two buffers; one
  descriptor set layout for every pass. Middle layers as an implicit GEMM
  with `VK_KHR_cooperative_matrix` (16×16×16 fp16, fp32 accumulate):
  bias preloaded into the accumulator, results stored straight to memory,
  ReLU deferred to the next pass's load. Checked against the fp32 shader
  on a 64-row readback (max abs diff 2e-3)
- CPU path (ONNX Runtime, `G`) kept for comparison, ~1 s/frame
- Temporal pass: reprojection through the reservoir's hit position and
  last frame's camera, eye-distance disocclusion test, YCoCg clip (luma
  1.25σ, chroma 2.5σ), per-pixel history length; with the camera still,
  unclipped same-pixel history up to 64 frames. Then albedo
  remodulation and a luminance-preserving Reinhard

**Scene** (`Code/Game/App.cpp`)
- Crytek Sponza (~270K tris, 25 mats), inline OBJ / MTL parse
- Bindless descriptor array, 64 textures (diffuse + normal maps)
- Mipmap chain generated on GPU via a `vkCmdBlitImage` chain; mip level
  in closesthit picked from hit distance (RT has no derivatives)
- 16 colored point lights from a 7-color saturated palette

## Performance

RTX 4080 Laptop, 1728×864, GPU timestamps:

| pass | time |
|---|---|
| TraceRays (ReSTIR DI) | 4.0–4.6 ms |
| conv_in | 0.57 ms |
| conv_mid ×3 (tensor cores) | 3 × 0.58 ms |
| apply | 0.41 ms |
| temporal | 0.27 ms |
| **frame** | **~7.6 ms (~130 FPS)** |

The denoiser went 13.7 → 7.0 → 3.1 → 3.0 ms: shared-memory tiles with
2×2 register blocking, then cooperative matrices, then the same tiling
for conv_in. The first cooperative-matrix version was slower than the
fp32 shader (2.25 vs 1.95 ms per layer: 47 KB of shared memory left two
workgroups per SM, and a 16-way bank conflict in the epilogue); the
fixes above took it to 0.58 ms. Each middle layer now moves ~190 MB,
close to the memory bandwidth limit.

## Known issues

- **Intermittent denoiser stalls.** For ~5 frames at a time the
  denoiser passes take 20–70× longer (20 FPS or less), mostly in the
  first ~30 s of load. Nsight Systems GPU metrics show DRAM writes pinned
  at ~90% of peak with SM issue at 2%, clocks unchanged, while the ray
  tracing pass in the same frame runs normally. Ruled out: input,
  other processes, paging, power, allocation layout, memory clock.
  Leaving the GPU a short idle gap every frame (`L`) made them rarer.
  Possibly the GPU fault above.
- ReSTIR temporal reuse reads the same pixel, without reprojection, so
  it stops helping while the camera moves.

## Controls

| key | action |
|---|---|
| `B` | denoiser on / off |
| `G` | denoise on GPU (Vulkan compute) / CPU (ONNX Runtime) |
| `M` | GPU denoiser mid layers: tensor cores / fp32 shader |
| `L` | wait for the GPU every frame (default off; can avoid intermittent denoiser stalls) |
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
`VK_EXT_descriptor_indexing`,
`VK_KHR_cooperative_matrix` (+ 16-bit storage and shaderFloat16).

Training: `ml/train_kpcn.py` and `ml/export_kpcn.py` (PyTorch, CUDA).

## Sponza assets

Sponza OBJ from [jimmiebergmann/Sponza](https://github.com/jimmiebergmann/Sponza).
Place `sponza.obj` + `sponza.mtl` + the `textures/` directory under
`Run/Data/Models/Sponza/`.

## References

- Bitterli, Wyman, Pharr, Shirley, Lefohn, Jarosz. *Spatiotemporal
  Reservoir Resampling for Real-Time Ray Tracing with Dynamic Direct
  Lighting*. SIGGRAPH 2020.
- Bako et al. *Kernel-Predicting Convolutional Networks for Denoising
  Monte Carlo Renderings*. SIGGRAPH 2017.
- Schied et al. *Spatiotemporal Variance-Guided Filtering*. HPG 2017
  (the previous denoiser, on the main branch).
