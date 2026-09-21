# PCSS verification

These tests distinguish shipping-renderer execution from source integration checks. They do not claim a complete interactive Orca GUI regression.

## CPU and source contracts

```sh
cmake -S tests/pcss -B build-pcss-tests
cmake --build build-pcss-tests --parallel
ctest --test-dir build-pcss-tests --output-on-failure
```

The CPU target uses the shipping `PCSSShadowMath.hpp`. The Python checks cover call order, actual visible G-code index ranges and caps, preference isolation, optional shader registration and Chinese strings. Source checks are **not** runtime layer-slider tests.

## Actual OpenGL renderer

On Ubuntu with an available display (or Xvfb):

```sh
sudo apt-get install libglew-dev libglfw3-dev libgl1-mesa-dev xvfb
cmake -S tests/pcss -B build-pcss-gl -DPCSS_TEST_OPENGL=ON
cmake --build build-pcss-gl --parallel
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ctest --test-dir build-pcss-gl --output-on-failure
LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=3.1 MESA_GLSL_VERSION_OVERRIDE=140 \
  xvfb-run -a ./build-pcss-gl/pcss_gl_tests
LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330 \
  xvfb-run -a ./build-pcss-gl/pcss_gl_tests --core
```

The GL target compiles the shipping `PCSSShadowRenderer.cpp`, not a rewritten test renderer. It compiles/links all new shader variants, renders real depth geometry, queries PCSS into an RGBA32F framebuffer and reads pixels back. Checks include gap/light-size/contact behavior; translation, nonuniform scale and rotation; sloped receivers; empty visible geometry; cached and invalidated updates; exception recovery; separate read/draw framebuffer restoration; a nonzero unpack PBO; plate blending and reserved texture-unit restoration.

Software OpenGL timings are not target-GPU performance claims. AddressSanitizer/UBSan can be enabled via normal CMake compiler/linker flags; driver leak checks may require a separate configuration.

## Required interactive application acceptance

On the actual supported Orca build and GPU:

1. Open Preferences, enable **Enable soft shadows (PCSS)**. Confirm both editor and preview repaint without slicing/restart; close/reopen the application and verify persistence.
2. In the editor, move/rotate/nonuniformly scale/mirror a solid object while dragging. Verify the shadow updates during the operation. Delete, undo and redo; verify no stale image.
3. Slice or load G-code without a model. Drag both layer-range endpoints, single-layer mode, and the sequential move slider. Hidden extrusion ranges must no longer cast shadows. Hide extrusion roles and reload the file.
4. Toggle between editor and preview. Check plate offsets, a nonrectangular bed, empty data, dark/light themes, selection outlines and tool overlays.
5. Disable the preference. Verify the original material/GUI path and that no shadow depth work is submitted. Check unsupported OpenGL/failed optional shader fallback.
6. Measure depth-update and receiver costs separately on a large model/G-code file; inspect slider responsiveness on the target GPU. Do not substitute a single aggregate FPS result for this check.

Painting/cut/variable-layer-height tools, transparent transmission, thumbnails, assembly view and picking retain their existing unshadowed paths. They are not silently advertised as newly supported workflows.
