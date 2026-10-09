# Bounded-cost PCSS shadows

## Diagnosis

The initial implementation at 27a56130 averaged four binary NEAREST depth
comparisons at fixed, asymmetric offsets. Its visibility could only be 0,
0.25, 0.5, 0.75 or 1. A wider kernel creates displaced hard silhouettes and
bands rather than a smooth penumbra. The blocker search omitted the center,
so thin casters could be missed between its four offsets.

The shadow camera is orthographic and the main light is directional. The old
`(receiverDepth - blockerDepth) / blockerDepth` formula used normalized depths
measured from an arbitrary near plane. The resulting softness changed with
the scene bounds / depth range. Fitting a scene bounding sphere also wasted
much of the 512-square map's XY resolution on the bed and empty build volume.

## Changes

Keep one 512 x 512 DEPTH_COMPONENT24 map and the existing unwritten RGBA8
compatibility attachment. Fit orthographic XY to caster bounds with a
six-texel guard band and a snapped center. Receiver bounds contribute to
near/far, not XY. This is still a single map: many distant plates can reduce
precision.

For a directional light with `tan(angularRadius) = 0.035`, use:

```
radiusWorld = (receiverDepth - averageBlockerDepth) * (far - near) * tan(angularRadius)
radiusUV    = radiusWorld / orthographicExtent
```

Normalized orthographic depths are linear. Do not divide by blocker depth
measured from an arbitrary near plane. Receiver-plane sample correction and
a 0.02 mm base bias reduce self-shadowing. Hardware PCF additionally needs a
one-texel plane bias to cover its neighbor comparisons. Derivatives are
evaluated before discard/non-uniform lighting branches. Reject projected
XYZ outside the frustum and treat texture borders as lit.

Five blocker taps include the center and four balanced surrounding offsets.
Eight balanced filter taps use native comparison filtering when OpenGL 3.3
or ARB_sampler_objects is available. Bind the same depth texture with a raw
NEAREST sampler and a LINEAR/LEQUAL comparison sampler on distinct units.
The fallback uses four manually bilinear-filtered comparison taps, each
comparing four NEAREST depths before interpolating visibility. GL_LINEAR on
raw depth is not PCF. Sampler units are initialized at program creation,
including for draws with shadows disabled.

Cap search/filter radius at four shadow texels for hardware PCF and two for
the legacy fallback. Sub-half-texel penumbrae need just one bilinear PCF
footprint. This deliberately limits very wide penumbrae to keep the small
sample budget usable. A fixed per-screen-pixel rotation breaks up coherent
bands without a time/frame seed, noise texture or history buffer. Some
spatial grain and motion shimmer can remain.

Cache the depth pass using geometry identity/revision, world transform,
index range, caster membership, light view-projection and clip/Z planes.
Globally unique GLModel revisions change on geometry initialization/reset,
including same-size rebuilds and allocator address reuse. Acquire the
existing LOD-ready hand-off before reading worker-generated geometry.
Camera translation/zoom alone need not redraw the depth map; orbit changes
the existing eye-relative light and correctly invalidates it.

Draw existing GLModel buffers directly with a position-only shader. Choose
Small LOD below 96 shadow pixels, otherwise available Middle LOD, with the
original geometry as fallback. Do not alter visible-scene LOD, build meshes,
or wait for workers in the pass. Avoid GLVolume material parsing and
per-color segmentation reconstruction. Restore FBOs, viewport, program,
texture/sampler bindings and raster/depth/blend/cull/scissor/stencil state
on success and allocation failure.

## Performance budget and limitations

| Fragment path | Shader texture operations, upper bounds |
|---|---:|
| Disabled or outside light frustum | 0 |
| No blocker found | 5 |
| Hardware PCF, sub-texel penumbra | 5 + 1 |
| Hardware PCF, wider penumbra | 5 + 8 |
| Legacy PCF, sub-texel penumbra | 5 + 4 |
| Legacy PCF, wider penumbra | 5 + 16 |

The baseline used 4 + 4 binary fetches. Native LINEAR shadow sampling filters
neighbor comparisons; one such instruction is not identical in cost to one
raw-depth fetch. These counts are NOT memory transaction counts or an FPS
prediction. Cache hits remove depth geometry submission, not receiver
shading. Orbit/object editing still regenerate the map. Without ready LODs,
very large original meshes remain expensive.

No 2K/4K maps, duplicate depth maps, full-screen blur, TAA, readbacks, fences
or glFinish are added to the application. Test-only readbacks stay in the
regression harness. Wide penumbrae, multiple depth layers, very thin parts,
LOD self-shadowing mismatch, grazing angles and distant plates still need
visual checks. Five-tap blocker search is an approximation; the center tap
does not guarantee detection of every sub-texel blocker over the full support.

## Automated checks

```sh
python3 tests/rendering/test_pcss.py
# Ubuntu 24.04 test dependencies; this does not build the slicer:
sudo apt-get install glslang-tools g++ libeigen3-dev libglew-dev libglfw3-dev \
  libgl1-mesa-dri libglx-mesa0 xvfb xauth python3-numpy python3-opengl python3-glfw
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a /usr/bin/python3 tests/rendering/test_pcss.py --gpu
```

Checks cover GLSL 110/140 links, including environment-map variants,
identical kernel copies, near/far-invariant units, derivative placement and
sampler separation. Synthetic OpenGL execution tests lit/shadow pixels,
sloped receivers, clipping/borders, thin blockers, continuous visibility
levels and contact hardening, for both hardware and manual sampling.

The C++ smoke compiles the actual GLCanvas3DShadow.cpp and actual resource
structs with lightweight application adapters and real Eigen/GLEW/OpenGL.
It checks cache invalidation, delayed LOD hand-off and ARB/EXT state/texture
restoration, including initial allocation. It does NOT compile the entire
application or prove production GUI/mesh integration. A full Release build
with the platform's usual dependencies remains required before shipping.

## Target-device acceptance

Compare baseline, patched, and shadows-disabled Release builds with the
same model, camera path, viewport, power mode and VSync. Warm shaders/LODs,
then record median and p95 frame time for static redraw/hover, pan/zoom,
sustained orbit and object transforms. Measure shadow depth and receiver
GPU time separately using asynchronous timer queries or a graphics profiler,
not glFinish in the application.

Check the reported tall-box scene at close/grazing views, mirrored and
nonuniformly scaled models, close/lifted occluders, thin parts, multiple
plates, SLA clipping, selection glow, thumbnails and shadow toggling. Test
both ready and not-yet-ready LODs on large meshes. A suggested gate is <=10%
median frame-time regression versus the old enabled-shadow branch, plus an
agreed absolute millisecond budget on the slowest supported GPU. This is a
validation target, not a measured claim or guarantee. Software OpenGL timing
is not representative of the user's AMD/Intel/NVIDIA/Apple hardware.

## References

Khronos OpenGL Wiki, Sampler Object and Sampler (GLSL): sampler modes,
comparison filtering and sampler-type/texture-unit restrictions. NVIDIA
GPU Gems 3, Chapter 8, section 8.6: blocker search, penumbra estimation and
filtering stages of PCSS.
