# Performance-bounded PCSS shadow filtering

Baseline: `27a56130dab462685ef3ca8dec364ea887852333` on `feature_pcss_shadow`.

## Diagnosis

The original receivers in `resources/shaders/{110,140}/{flat,gouraud,gouraud_light}.fs` used four fixed Poisson offsets for blocker search and four binary depth comparisons for PCF. Although eight offsets were declared, each loop only visited the first four. Thus PCF could produce only 0, 0.25, 0.5, 0.75 or 1.0. Averaging a few displaced hard-shadow silhouettes explains layered or apparently separated edges rather than a continuous soft edge. The first four offsets also form an unbalanced subset and do not include the center, making thin blockers easier to miss.

`GLCanvas3D::RenderShadowMap()` fits an orthographic light camera to scene bounds and uses a 512x512 map with NEAREST depth sampling. When the camera zooms in, a shadow texel can cover many screen pixels; this magnifies the quantization. A wider sparse filter is not a substitute for reconstruction.

The original `(receiverDepth - blockerDepth) / blockerDepth` expression divides normalized orthographic depth by a value whose origin depends on the arbitrary near plane. Together with the ad-hoc `*32` scale, it is not a consistent directional-light penumbra calculation. A fixed normalized bias is also scene-scale dependent, and offset samples need receiver-plane correction on sloping receivers. Finally, only UV bounds were checked, not the receiver's light-space Z bounds.

These are code-level findings consistent with the reported screenshots. The exact application scene and target GPU have not been replayed here.

## Implemented fix

All six fragment shaders now use the same bounded-cost kernel, with `texture2D` for GLSL 110 and `texture` for GLSL 140. The regression test checks that the kernels remain identical.

A complete, texel-aligned 3x3 neighborhood is loaded once: at most nine NEAREST raw-depth reads. Those depths are reused for blocker classification, average blocker depth, and the final PCF. The center is included. There is no second texture-sampling loop.

The filter integrates a square footprint over the neighboring texel cells. The final result weights the **depth-comparison results**, not interpolated raw depths. Weights vary continuously with the receiver's fractional texel position, removing the old restriction to five visibility levels. Full-light and full-shadow neighborhoods return before the remaining penumbra arithmetic; these exits happen AFTER the nine reads and do not reduce that sampling budget.

For the current directional light and orthographic projection:

```
gap_world = (receiver_depth - average_blocker_depth) * (far - near)
radius_world = gap_world * tan(light_angular_radius)
radius_uv = radius_world / light_frustum_width
```

The existing `shadow_light_size` value is interpreted as the angular tangent (currently 0.035). The row lengths of the existing `shadow_matrix` recover `2/width`, `2/height` and `2/(far-near)`, so no new C++ uniforms or additional per-vertex varyings are needed. This formula is specific to the current orthographic directional light; it must not be reused unchanged for a perspective spot light.

Receiver-plane depth derivatives are evaluated before fragment discard and non-uniform lighting branches. Every fetched texel is compared against the receiver plane at that texel center. A small world-scale base bias (0.02 mm before the existing cap and a numerical floor) replaces the larger scene-dependent constant in the receiver. Bounds checks cover XYZ, and samples outside the map are treated as lit without depending on the texture wrap mode.

The existing print-volume detection, slope coloring, clipping behavior, lighting composition and uniform interfaces are retained. The change does not modify C++ scene rendering, the light direction, geometry selection, LOD policy, framebuffer allocation or the vertex shaders.

## Explicit quality/performance tradeoff

This is a **limited-radius, local PCSS approximation**, not an unrestricted wide-area PCSS implementation. Its blocker search is limited to the same local neighborhood, and the filter half-width is clamped to **0.5 to 1.0 shadow-map texels**. At the lower limit it reproduces bilinear PCF reconstruction; at the upper limit the complete footprint still fits the fetched 3x3 neighborhood. Far shadows therefore stop widening once this cap is reached. The square filter is not a circular area-light integration.

Do not simply raise the radius clamp: that would request a footprint not represented by the nine samples and invalidate the filter. Wider penumbrae require a separate, explicitly budgeted quality path.

| Work per receiving fragment | Original | This change |
| --- | --- | --- |
| No blocker found | 4 raw-depth reads | Up to 9 raw-depth reads |
| Blocker found / partial shadow | 4 search + 4 filter reads | Up to 9 shared raw-depth reads |
| Shadow-map resolution | 512x512 | 512x512, unchanged |
| Extra render targets / full-screen passes | None added by this fix | None |
| Additional model shadow draws | Existing shadow pass | Unchanged |

Nine versus eight reads on the old blocker-found path is NOT an FPS estimate. The old no-blocker path used only four reads, and this change adds derivative and weighting arithmetic. Keeping the fetch count bounded and avoiding larger maps, extra passes and random-noise denoising controls cost, but **a small total-frame regression has not been verified on AMD integrated graphics or the user's scene**.

The existing depth pass still redraws geometry each frame and calls `volume->render()`. Geometry-heavy scenes may be dominated by that pass rather than PCF. Shadow-map caching or a dedicated shadow LOD path would be separate C++ work and are not claimed as implemented here. Cached depth would need invalidation for geometry/transforms, visibility, clipping, LOD readiness and light direction; the existing light follows camera rotation, so rotation cannot blindly reuse a stale shadow map.

Likewise, the scene-wide 512x512 coverage remains a resolution limit for extreme zoom and multiple distant plates. This fix improves filtering; it does not claim to recover absent geometric detail. Camera-fitted receiver bounds and conservative off-camera caster selection are separate work.

## Automated validation

From the repository root:

```sh
python3 tests/rendering/test_pcss_budget.py
LIBGL_ALWAYS_SOFTWARE=1 python3 tests/rendering/test_pcss_budget.py --gpu
```

The first command uses only Python's standard library. The optional rendering checks need Linux Mesa EGL/OpenGL shared libraries; they do not need PyOpenGL. Run without Python's `-O` option, which removes assertions.

Validation performed on 2026-10-09 using Mesa 25.0.7-2, OpenGL 4.5 compatibility, llvmpipe (LLVM 19.1.7):

- Five mathematical/source-consistency tests passed: normalized nonnegative area weights, bilinear contact limit, orthographic unit invariance, six-kernel equality, and the nine-read budget.
- Both GLSL versions compiled and linked with their actual original vertex shaders for flat, gouraud and gouraud_light, including the environment-map variant: eight program combinations.
- Synthetic fixtures passed for full light, full shadow, disabled shadows, a sloping self-receiver, out-of-range depth/UV, a one-texel blocker and near/far-plane changes.
- The magnified edge fixture produced 34 quantized visibility levels instead of being restricted to five. In that fixture, the transition widened from 16 to 32 output pixels when separation increased; these are fixture screen pixels, not shadow-map texels.

These are shader correctness checks, **not a full application build, screenshot replay, hardware-driver certification, or frame-rate benchmark**.

## Target-device acceptance

Compare the baseline and this change in Release builds, with the same scene, viewport, VSync setting, shadow setting and completed LOD generation. Repeatedly reproduce the tall thin object and close-up camera angles in the report, then test multiple instances, transforms, mirrored objects, preview clipping, multiple plates and repeated shadow toggles.

Record median and p95 total frame time, plus shadow-depth-pass and receiver-shading time where GPU timing tools are available. Test stationary redraws, pan/zoom, orbit and object dragging separately. For a concrete acceptance gate, use a project-agreed limit such as no more than 5% additional median/p95 total frame time relative to the original PCSS branch; that is a proposed gate, NOT an achieved measurement. Compare shadow-disabled rendering separately to identify the cost already introduced by the original shadow-depth pass.

If the gate fails, profile before increasing resolution or sample count. A smaller filtering improvement must not be misreported as a guarantee about the cost of rendering a large mesh twice.
