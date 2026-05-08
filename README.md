# VulkanRTTest — Vulkan KHR Ray Tracing Demo (in progress)

Demo app on top of the Igloo Engine's Vulkan backend, exercising the
`VK_KHR_ray_tracing_pipeline` / `VK_KHR_acceleration_structure` extensions
to do hardware-accelerated ray tracing on RTX-class hardware.

Sibling demo to **Tessera** (which exercises the tile-deferred /
TBR-targeted path, for mobile). Same Igloo Engine, different target +
optimization story.

## Current state (2026-05-07)

**Engine layer (`SD/Engine/Code/Engine/Renderer/VulkanRTPath.{h,cpp}`):**
- `VulkanRenderer` extended with KHR ray tracing extension enablement:
  5 device extensions + 4 feature structs chained into
  `VkDeviceCreateInfo.pNext`. 9 RT function pointers loaded post-
  `vkCreateDevice`. `m_rtProperties` + `m_asProperties` queried.
- `VulkanRTPath` is now **fully implemented** (no stubs):
  - `CreateOutputImage` / `RecreateOutput` — R8G8B8A8 storage image,
    USAGE_STORAGE_BIT | USAGE_TRANSFER_SRC_BIT.
  - `BuildBLAS(verts, indices)` — allocates RT-flagged vertex/index
    buffers, queries build sizes, allocates AS storage + scratch,
    submits `vkCmdBuildAccelerationStructuresKHR` on a one-shot cmd.
  - `BuildTLAS(instances)` — same shape but with instance buffer +
    `VkAccelerationStructureGeometryInstancesDataKHR`.
  - `CreateRTPipeline(rgen, rchit, rmiss)` — 3 stages, 3 groups
    (general/triangles_hit/general), `maxPipelineRayRecursionDepth=1`.
    Descriptor set layout: binding 0 STORAGE_IMAGE, binding 1 AS_KHR,
    binding 2 UNIFORM_BUFFER. Camera UBO is RTPath-owned, host-mapped.
  - `CreateSBT` — handles fetched via `pfnGetRayTracingShaderGroupHandlesKHR`,
    packed with `shaderGroupBaseAlignment` between raygen/miss/hit
    regions. Stride = `alignUp(handleSize, handleAlignment)`.
  - `UpdateDescriptors(tlas)` — writes the TLAS handle into binding 1
    via `VkWriteDescriptorSetAccelerationStructureKHR` pNext.
  - `UpdateCamera(viewInv, projInv)` — per-frame `memcpy` into the
    persistently-mapped camera UBO.
  - `TraceRays(cmd, w, h)` — binds pipeline + descriptor set, calls
    `pfnCmdTraceRaysKHR`.
- Internal helpers: `BeginOneShotCmd` / `EndAndSubmitOneShotCmd` (own
  graphics-family `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` pool),
  `CreateAndAllocateBuffer` with `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`
  on shader-device-address buffers.

**Demo app:**
- App.cpp / Gamecommon.hpp now own `g_theRTPath`. `App::Startup` builds
  a unit cube BLAS, a 1-instance TLAS, the RT pipeline + SBT, and
  hooks descriptors. Verified bring-up on RTX 4080 Laptop — no
  validation errors, app stays alive.
- Game.cpp / App.cpp **still hold Tessera's deferred-rendering code** —
  the live frame loop is still going through `g_theDeferred`. RT path
  is initialized but not yet integrated into the per-frame render.
- Three RT shader sources at `Run/Data/Shaders/Vulkan/rt/`:
  - `raygen.rgen` — primary rays, reconstructs from camera matrices.
  - `closesthit.rchit` — barycentric coords as RGB.
  - `miss.rmiss` — sky gradient based on `gl_WorldRayDirectionEXT.y`.
- All three compiled to SPIR-V (`*.spv`) targeting Vulkan 1.3 / SPV 1.4.

## Next — final integration

The RT path is loaded; what's left is wiring it into the per-frame
render loop:

1. Strip the Tessera deferred render code out of `Game.cpp` /
   `Game.hpp` / `App.cpp` (chess pieces, F2/F3/F4 toggles, MT recording,
   HUD). Game.cpp's `Render()` should drop down to a much smaller
   function that just dispatches RT.
2. In `Game::Render`:
   - Compute `viewInverse` + `projInverse` from the world camera; call
     `g_theRTPath->UpdateCamera(...)`.
   - Get the current cmd buffer from `VulkanRenderer::GetCurrentCommandBuffer()`.
   - Issue `UNDEFINED -> GENERAL` barrier on the output image (first
     frame only) — or better, do the transition inside `RecreateOutput`.
   - Call `g_theRTPath->TraceRays(cmd, w, h)`.
   - Memory barrier (storage write -> transfer read).
   - `vkCmdBlitImage` from `GetOutputImage()` to the current swapchain
     image (transition swapchain image PRESENT_SRC -> TRANSFER_DST,
     blit, transition back).
3. The render-pass conflict: VulkanRenderer's BeginFrame may auto-start
   a render pass. RT writes happen outside a render pass. Either
   bypass the auto-start in BeginFrame for RT mode, or run the RT
   trace + blit *before* BeginCamera kicks the rasterized pass.

**Expected first-cut visual:** sky-blue background, single cube with
barycentric-RGB coloring, responsive to camera movement.

## Why this lives separately from Tessera

- Tessera's design story is **mobile / TBR / tile-local bandwidth**;
  RT cores are the opposite domain. Different target hardware, different
  optimization axes. Mixing into one repo dilutes both narratives.
- Same Igloo engine = code reuse without coupling demos. Adding the
  KHR_ray_tracing extension to Igloo benefits any future demo on the
  same engine.

## Hardware

Tested on RTX 4080 Laptop (Ada SM 8.9, 3rd-gen RT cores). Mali /
Immortalis-G720 in Tab S10 Ultra likely lacks full
`VK_KHR_ray_tracing_pipeline` support — this demo is desktop-NVIDIA-only
for now (would need extension fallback path for mobile).
