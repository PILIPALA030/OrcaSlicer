# PCSS v2: contact hardening, bounded shadow geometry, and a live preference

Baseline: `8f2be2697663b8ba59138090b146733536c9684c` on `feature_pcss_shadow`.

## Why the previous change was insufficient

The previous shared 3x3 filter capped its half-width at 0.5-1.0 shadow texels. It removed the original four-tap layering but saturated almost immediately, so distant shadows still looked like narrow PCF. It also left `GLCanvas3D::RenderShadowMap()` calling the ordinary `GLVolume::render()` path every frame. That path could select the full-resolution viewer mesh, run color/material splitting, and submit a multi-million-triangle mesh a second time. A small 512x512 depth texture does not bound vertex processing or triangle setup.

These are code-level causes consistent with the report. The user's original 3-million-triangle asset and target graphics hardware are not available in this validation environment.

## Actual PCSS receiver stages

All six GLSL 110/140 receivers now use the same implementation, retaining the existing uniforms and lighting/clipping/print-volume behavior:

1. Nine independent raw-depth blocker samples, including the center and balanced rings. The search footprint follows the directional light's possible separation and is bounded separately from the final filter.
2. Average blocker separation in orthographic light space. `radiusWorld = separationWorld * tan(angularRadius)`, converted to UV units using the existing light matrix. The implementation does not divide by normalized blocker depth.
3. Twelve disk-distributed PCF locations across the computed radius. Each location reconstructs bilinear visibility using four NEAREST depth comparisons, with receiver-plane correction at the actual fetched texel centers. It does not interpolate raw depths before comparison.

The half-width can now grow to **8 shadow texels**, rather than stopping at 1. Near-zero radii take a single bilinear-PCF contact path. This remains finite-sample PCSS, with a practical wide-kernel cap, not an exact area-light integrator. Nine blocker samples can still miss thin geometry between samples; increasing the cap alone is not a free quality improvement. No temporal randomness, history/denoising pass, new texture state requirement, or extra framebuffer is introduced.

Derivatives are evaluated before discard or non-uniform lighting branches. UV/Z bounds, lit samples outside the map, and a small world-scale receiver bias are retained. Sparse blocker results are not used as an unsafe all-shadow early-out. Although the separation-to-radius conversion is independent of the depth origin, changing the fitted near plane can change the search footprint and therefore the blockers selected in non-uniform scenes.

### Fragment cost, explicitly

| Path | Maximum raw-depth texture reads |
| --- | ---: |
| Shadow disabled / outside light volume | 0 |
| No blockers found | 9 |
| Near-zero-radius contact filter | 9 + 4 = 13 |
| General PCSS filter | 9 + 12 x 4 = 57 |

Wide PCSS is **more expensive per receiving fragment** than the old narrow nine-read filter. The performance improvement for large meshes is primarily in the shadow geometry path below, not a claim that 57 reads cost less than 9. Hardware comparison samplers or a lower-resolution shadow resolve are not implemented in this change.

## Independent shadow geometry

`GLVolume::render()` now recognizes the depth-only shader before ordinary color/material rendering. For a full-range imported model, it uses this policy:

- Original geometry if already at or below **100,000 triangles**.
- An existing completed Small/Middle LOD if it satisfies the same budget, independently of the camera's visible-LOD choice.
- Otherwise one cached `ShadowMeshProxy` per shared source mesh. A background QEM job targets at most 100,000 triangles; only the bounded result is expanded into position-only GL geometry on the main thread.

For a 3,000,000-triangle imported model, the prepared shadow draw is therefore at most 100,000 triangles (at most 1/30 of the original shadow triangle count). **This is not a 30x FPS claim.** The normal visible mesh, picking mesh, editable mesh, slicing data and visible-LOD selection are not changed. Instances of the same source share the proxy; each instance still needs a shadow draw with its own transform.

The budget applies to full-range imported model geometry. Partial index ranges and geometry without an original mesh pointer (for example some auxiliary geometry) keep their original path, because substituting unrelated proxy indices would be incorrect. This is a per-caster budget, not a cap on the total scene's triangles or draw calls.

### Background preparation and ownership

The proxy waits for existing visible-LOD jobs to finish before starting an additional mesh-sized copy/QEM job, and first gives rendering a chance to adopt a completed affordable LOD. A shared worker slot limits extra proxy QEM work to one job at a time. Input ownership is shared/immutable; workers own CPU data only, not a canvas or a GLModel. Release/acquire handoff publishes the result, and GPU resource creation/destruction remains on the UI/GL thread. The timer polls completion at 100 ms, requests a redraw, and does not busy-wait at render FPS.

While a large proxy is pending, that caster's shadow is temporarily omitted instead of falling back to another 3-million-triangle shadow draw. A failed or over-budget result stays omitted and logs a warning. QEM is an approximation and can alter silhouettes or self-shadowing; original-asset image validation remains necessary. Extra CPU work/memory during preparation is finite but not claimed negligible.

Disabling shadows cancels pending proxy work cooperatively; removing the last source-mesh holder destroys its proxy and requests cancellation without joining a worker on the UI thread. Workers retain only their safe CPU ownership. Completed proxies remain cached while their mesh is alive and can be reused when shadows are re-enabled.

This is **mesh-proxy caching**, not shadow-map caching. The existing depth pass still renders each frame, and follows the existing camera-dependent light direction. No stale depth map is reused during orbit or object transformations. Existing depth-pass clipping/coverage limitations are not fixed here, and scene-wide 512x512 shadow-map resolution remains unchanged.

## Preferences

General preferences now include **Enable PCSS soft shadows**, next to the camera controls. It uses the existing persistent `show_model_shadow` key and preserves the current saved value. The generic checkbox handler saves the config and explicitly dirties the Prepare and Preview canvases, without reloading meshes or restarting the application.

When off, the existing early return in `RenderShadowMap()` skips shadow FBO work and geometry draws; receiver shadow factors are disabled. Cached GPU resources are not necessarily freed on every toggle. The new English label/tooltip use the normal translation macros; translated catalogs may need an update.

## Regression commands and validation scope

```sh
python3 tests/rendering/test_pcss_budget.py
LIBGL_ALWAYS_SOFTWARE=1 python3 tests/rendering/test_pcss_budget.py --gpu
python3 tests/rendering/test_shadow_proxy.py --sanitize
```

The first command needs only Python's standard library. The EGL checks additionally need Linux Mesa OpenGL/EGL; the proxy harness needs a C++17 compiler, with ASan/UBSan for `--sanitize`. Run the proxy harness without Python/C++ assertion-disabling options.

Validation on 2026-10-09:

- Four mathematical/source tests passed: six-kernel equality, world-unit radius, balanced disk samples and explicit sampling/resampling budget.
- Eight actual shader program pairs compiled and linked: GLSL 110/140 flat, gouraud, gouraud_light and the gouraud environment-map variants. Local shader contents were checked against the corresponding remote Git blob hashes.
- On Mesa 25.0.7-2, llvmpipe (LLVM 19.1.7), synthetic fixtures passed for full light/shadow, disabled shadows, receiver-plane correction, out-of-range UV/Z, a thin contact blocker, and depth-range changes for a uniform blocker.
- At separations 0.3, 15 and 60 world units, the edge's 5%-95% visibility transition grew to **14, 36 and 134 output pixels**, respectively, in both GLSL versions. The fixture uses 16 output pixels per shadow texel. This verifies widening beyond the old local-PCF footprint; it is not a screenshot from the user's asset.
- The actual `ShadowMeshProxy.hpp` passed a C++17 ASan/UBSan lifecycle harness with controlled wx/GL/QEM stubs: pending LODs, a 3,000,000-indexed-triangle input and 100,000-triangle budget, reuse, serialized workers, disable/re-enable, deletion during work, exception/empty/over-budget results, and thread ownership. **The QEM implementation is stubbed in this harness:** it tests control flow/lifetime, not real simplification quality or speed.

A full application C++ build, native Preferences UI interaction, real QEM geometry regression, original screenshot replay, and AMD/NVIDIA/Intel target-device frame-rate validation have **not** been completed here. Do not turn these isolated checks into a hardware performance guarantee.

## Target-device acceptance

Rebuild the application, not just its shader resources. In Release builds, compare the baseline, v2-on and v2-off using the same 3-million-triangle model, viewport, VSync and completed preparation state. Inspect the log for the actual proxy triangle count. Measure median and p95 frame time separately for initial preparation, stationary redraw, pan/zoom, orbit and object dragging; distinguish shadow-depth time from receiver-shader time.

Check contact versus distant penumbrae, thin details, painted/mirrored/multiple instances, deletion during preparation, rapid toggles, restart persistence and Preview behavior. If the fragment path dominates on the target GPU, optimize or add an explicitly budgeted resolve path instead of claiming geometry reduction alone guarantees a small total-frame regression.
