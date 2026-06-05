# ReSTIR GI in VulkanRTTest

Tracks the per-step build of one-bounce diffuse GI via ReSTIR on top of
the shipped DI + SVGF path. Reference: Ouyang et al. 2021 — *ReSTIR GI:
Path Resampling for Real-Time Path Tracing*. Pairs with `restir.md`.

## Goal

After ReSTIR DI converged the direct lighting, the dark corners and
unlit surfaces still look flat — only the hemispheric ambient stand-in
fills them. ReSTIR GI replaces that stand-in with one-bounce indirect:
shoot a cosine-weighted ray from each visible point, shade the second
hit with a cheap 1-spp NEE, and reuse the resulting "sample paths"
spatiotemporally so the noise is denoiser-tractable.

## Scope (intentional)

- **One bounce, diffuse only.** Multi-bounce / glossy is [[project_luminagi_tdd]]
  / [[project_luminagi_cudaref]]'s territory.
- **Sample point shaded with 1-spp NEE** (random light + 1 shadow ray).
  No nested ReSTIR DI at the sample point.
- **Additive to DI**, runs through the same TAA → A-Trous → composite
  chain. Step G6 splits into a separate denoiser only if the combined
  chain visibly smears DI.

## Pipeline overview

```
per-frame, in closesthit (after DI shading):
  wi = cosineHemi(shadingN)
  trace 1-bounce inline rayQuery → xs, ns, albedo_s   (or miss → sky)
  Lo = shadeSamplePoint(xs, ns, albedo_s)             (or skyHemi(wi))
  build current GI reservoir from (xs, ns, Lo)
  temporal merge with prev pixel's GI reservoir       (surface-gated)
  spatial merge with K neighbors' GI reservoirs       (Jacobian)
  giLight = Lo * cosTheta_v * W / π
  payloadColor = diLight + giLight + ambient
```

## GI reservoir layout — 48B (12 ints, matches DI slot)

```
struct GIReservoir {
    vec3  xs;          // sample-point world pos
    float wsum;
    vec3  ns;          // sample-point normal
    int   M;
    vec3  Lo;          // outgoing radiance at xs toward xv
    float pHatStored;  // target fn at acceptance — for unbiased reuse
};
```

Visible point `xv` / `nv` are re-derived from the primary hit each
frame, so they don't live in the reservoir. Marshalled as `int[12]`
ping-pong SSBOs at bindings 20/21, same `floatBitsToInt` /
`intBitsToFloat` pattern as the DI reservoirs at 13/14.

## Steps

| #  | Status | What |
|----|--------|------|
| G0 | TODO   | Engine: bindings 20/21 in DSL + pool, descriptor writes, alloc + zero-init in `RecreateOutput`, destroy in `Shutdown`. Shader declares the bindings (no logic yet). Validation clean, image unchanged. |
| G1 | TODO   | closesthit: cosine-hemisphere sample around `shadingN`, inline rayQuery 1-bounce. Debug visualize by overwriting `payloadColor = ns_s * 0.5 + 0.5` — should look like a shaded view "behind walls". |
| G2 | TODO   | Shade sample point with 1-light NEE + hemi ambient; sky-miss path returns `skyHemi(wi)`. Debug `payloadColor = Lo` — much darker than DI, colored where bounces are colored. |
| G3 | TODO   | Build single-frame GI reservoir (M=1), write to ping/pong, integrate `giLight = Lo * cos * W / π` into `payloadColor`. Baseline 1-spp ReSTIR GI estimator. Expect heavy noise. |
| G4 | TODO   | Temporal merge same-pixel, gated by the DI reservoir's `prev.normal/prev.depth` (already loaded — free check). M-clamp = 10. Mild rotation ghosting expected (no reprojection in v0). |
| G5 | TODO   | Spatial merge — 3 taps, radius 8 px, Jacobian `(cosC/cosO)*(dO²/dC²)` clamped `[0.01, 100]`. Flat regions smooth, edges stay sharp. |
| G6 | OPT    | Split GI denoise (own history image, own atrous pass, composite adds `albedo * giFiltered`). Gated on measured DI smearing — skip if combined chain holds up. |
| G7 | TODO   | README + restir.md scope sections + memory: "DI-only" → "DI + 1-bounce diffuse GI". Re-measure FPS, log in README. |

## Known issues / decisions

- **Sample-point shading uses material color only**, no UV/texture
  fetch at the secondary hit. SVGF's spatial filter hides the missing
  detail; texture fetch would double the closesthit cost for negligible
  visual win.
- **`pHat = luminance(Lo) * cosTheta_v`** — chrominance deliberately
  out of the target so reservoirs don't bias toward red samples. The
  atrous chrominance edge stop already handles hue variance downstream.
- **No motion-vector reprojection for GI temporal in v0** — same-pixel
  only, surface-gated. Reprojection is gated on whether rotation
  ghosting is bad enough to bother.
- **Surface-similarity gate reused from DI** — `dot(N,N') > 0.99` and
  `|Δd|/d < 1%`. Tight on purpose; loose gates darken Jacobian-clamped
  edges.
- **`Lo` carries sample-point albedo** (from `albedo_s/π` inside
  `shadeSamplePoint`); the visible-point albedo is multiplied later by
  composite. So the integration step emits `Lo * cos * W / π` un-modulated
  — if you also multiply by the visible-point albedo here you get a
  double-modulate.
- **Memory cost**: 48B × W × H × 2 = ~122 MB at 1600×800. DI uses the
  same already, total ~244 MB for reservoirs.

## Files

- `Engine/Code/Engine/Renderer/VulkanRTPath.{h,cpp}` — bindings 20/21,
  alloc, descriptor writes, resize hook, destroy.
- `Run/Data/Shaders/Vulkan/rt/closesthit.rchit` — GI block, helpers,
  marshalling.
- `README.md`, `docs/restir.md`, memory — scope language (step G7).

## Deferred

- Glossy GI (GGX importance sampling + view-dependent Jacobian).
- Multi-bounce path reuse (GRIS).
- Per-pixel GI variance / separate GI atrous (G6 — gated on need).
- Motion-vector reprojection for GI temporal (gated on observed ghosting).
