# VulkanRTTest

Vulkan KHR ray tracing demo on the Igloo Engine. Sibling of
[Tessera](https://github.com/CarlottaSeal/Tessera) (the TBR / tile-deferred demo);
both share the same engine.

## Scene

Crytek Sponza (270k verts / 262k tris / 25 materials), parsed inline at
startup, fed into a single BLAS. One TLAS instance with axis remap +
0.01 scale.

Shading per hit: per-material color (synthesized from FNV hash of the
material name since Sponza's MTL Kd is uniform gray and we don't load
the diffuse textures yet), Lambert + Blinn-Phong specular, hemispheric
ambient, hard shadow ray to a directional sun.

## Controls

- `SPACE` / `N` — leave attract mode
- `F5` — toggle RT path on/off
- WASD + mouse — fly camera (engine convention: x-fwd, y-left, z-up)
- ` ` ` (backtick) — dev console

## Layout

- `Code/Game/App.cpp` — RT path bring-up, OBJ parse, BLAS/TLAS/pipeline/SBT init
- `Code/Game/Game.cpp` — F5 toggle, RT branch in `Render()` (camera UBO, TraceRays, blit, HUD overlay)
- `Run/Data/Shaders/Vulkan/rt/` — raygen / closesthit / miss / shadowmiss
- Engine side: `SD/Engine/Code/Engine/Renderer/VulkanRTPath.{h,cpp}`

## Hardware

RTX 4080 Laptop (Ada, 3rd-gen RT cores). Needs
`VK_KHR_ray_tracing_pipeline` + `VK_KHR_acceleration_structure`.

## Sponza assets

Run `git submodule update --init` is NOT how this works yet — the
Sponza OBJ comes from
[jimmiebergmann/Sponza](https://github.com/jimmiebergmann/Sponza); place
`sponza.obj` + `sponza.mtl` under `Run/Data/Models/Sponza/`. Textures
are not loaded.
