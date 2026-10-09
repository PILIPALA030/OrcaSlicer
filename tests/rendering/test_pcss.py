#!/usr/bin/env python3
"""PCSS checks. Run from any directory; add --gpu under xvfb-run for GL tests.

The C++ smoke compiles the actual renderer with lightweight app adapters and
real Eigen/GLEW/OpenGL, not the full slicer. Software GL is not an FPS benchmark.
See doc/pcss_shadows.md for dependencies and target-device validation.
"""
from pathlib import Path
import argparse
import ctypes
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / 'resources/shaders'
NAMES = ('flat', 'gouraud', 'gouraud_light', 'shadow_depth')


def kernel(source):
    block = source[source.index('// Keep this block identical'):source.index('void main()')]
    return block.replace('texture2D(', 'texture(').replace('shadow2D(shadow_map_pcf, coordinates).r',
                                                         'texture(shadow_map_pcf, coordinates)')


class Contracts(unittest.TestCase):
    def test_six_shader_copies_match(self):
        blocks = [kernel((SHADERS / v / (name + '.fs')).read_text())
                  for v in ('110', '140') for name in NAMES[:3]]
        self.assertTrue(all(block == blocks[0] for block in blocks))

    def test_receiver_derivatives_precede_discard(self):
        for version in ('110', '140'):
            source = (SHADERS / version / 'gouraud.fs').read_text().split('void main()')[1]
            self.assertLess(source.index('shadow_receiver_gradient()'), source.index('discard;'))
            self.assertLess(source.index('shadow_receiver_gradient()'), source.index('top_diffuse >'))

    def test_orthographic_separation_is_near_far_invariant(self):
        for near, far in ((1, 300), (51, 351), (-500, 2000)):
            receiver = (160-near)/(far-near)
            blocker = (100-near)/(far-near)
            uv_radius = (receiver-blocker) * (far-near) * 0.035 / 200
            self.assertAlmostEqual(uv_radius, 60*0.035/200)

    def test_guard_band_covers_filter_and_snap(self):
        side = 80 * 512/(512-12)
        texel = side/512
        self.assertGreater((side-80)/2 - 0.5*texel, (4+1)*texel)

    def test_geometry_revision_and_cache_contract(self):
        model = (ROOT / 'src/slic3r/GUI/GLModel.cpp').read_text()
        self.assertEqual(model.count('next_geometry_revision.fetch_add'), 4)
        self.assertIn('std::atomic<std::uint64_t>', model)
        source = (ROOT / 'src/slic3r/GUI/GLCanvas3DShadow.cpp').read_text()
        self.assertLess(source.index('if (unchanged)'), source.index('const ShadowPassState saved_state'))
        self.assertNotIn('volume->render()', source)
        self.assertIn('a.geometryRevision == b.geometryRevision', source)
        self.assertIn('clipping_plane == m_shadowMap.cachedClippingPlane', source)
        self.assertIn('SHADOW_MAP_SIZE = 512', source)

    def test_samplers_initialized_before_first_draw(self):
        manager = (ROOT / 'src/slic3r/GUI/GLShadersManager.cpp').read_text()
        self.assertIn('set_uniform("shadow_map", 1)', manager)
        self.assertIn('set_uniform("shadow_map_pcf", 2)', manager)
        for version in ('110', '140'):
            source = (SHADERS / version / 'flat.fs').read_text()
            self.assertIn('sampler2DShadow shadow_map_pcf', source)
            self.assertIn('i < 5', source)
            self.assertIn('i < 8', source)
            self.assertNotIn('max(blockerDepth', source)


def compile_glsl():
    validator = shutil.which('glslangValidator')
    if not validator:
        raise RuntimeError('Install glslang-tools to run --gpu.')
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        for version in ('110', '140'):
            for name in NAMES:
                for environment in (False, True) if name == 'gouraud' else (False,):
                    files = []
                    for old, new in (('vs', 'vert'), ('fs', 'frag')):
                        source = (SHADERS / version / (name + '.' + old)).read_text()
                        if environment:
                            head, rest = source.split('\n', 1)
                            source = head + '\n#define ENABLE_ENVIRONMENT_MAP\n' + rest
                        path = work / (name + '.' + new)
                        path.write_text(source)
                        files.append(str(path))
                    validation = subprocess.run([validator, '-l', *files], capture_output=True, text=True)
                    if validation.returncode:
                        raise RuntimeError(validation.stdout + validation.stderr)
                    print(f'GLSL {version} linked: {name}, environment={environment}', flush=True)


def cpp_smoke():
    tests = ROOT / 'tests/rendering'
    header = (ROOT / 'src/slic3r/GUI/GLCanvas3D.hpp').read_text()
    a = header.index('    struct ShadowCasterState')
    b = header.index('    ShadowMapResources m_shadowMap;', a) + len('    ShadowMapResources m_shadowMap;')
    adapter = (tests / 'pcss_smoke_adapter.hpp').read_text().replace(
        '// PCSS_RESOURCE_FIELDS -- injected verbatim from GLCanvas3D.hpp by the test', header[a:b])
    renderer = (ROOT / 'src/slic3r/GUI/GLCanvas3DShadow.cpp').read_text()
    renderer = '\n'.join(line for line in renderer.splitlines()
                         if not line.startswith('#include "') and line != '#include <boost/log/trivial.hpp>')
    source = adapter + '\n' + renderer + '\n' + (tests / 'pcss_smoke_main.cpp').read_text()
    with tempfile.TemporaryDirectory() as directory:
        cpp = Path(directory) / 'pcss_smoke.cpp'
        exe = Path(directory) / 'pcss_smoke'
        cpp.write_text(source)
        subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-I/usr/include/eigen3',
                        str(cpp), '-o', str(exe), '-lGLEW', '-lglfw', '-lGL'], check=True)
        subprocess.run([str(exe)], check=True)
    print('Renderer C++ adapter smoke passed (not a full application build).', flush=True)


def gpu_visibility():
    import glfw
    import numpy as np
    from OpenGL import GL as gl
    from OpenGL.GL import shaders
    if not glfw.init():
        raise RuntimeError('An OpenGL display is required (use xvfb-run).')
    glfw.window_hint(glfw.VISIBLE, glfw.FALSE)
    window = glfw.create_window(512, 128, 'PCSS regression', None, None)
    if not window:
        raise RuntimeError('Could not create the OpenGL test context.')
    glfw.make_context_current(window)
    print('Software-test GL renderer:', gl.glGetString(gl.GL_RENDERER).decode(), flush=True)
    width, height, size = 512, 128, 512
    depth = gl.glGenTextures(1)
    for unit in (1, 2):
        gl.glActiveTexture(gl.GL_TEXTURE0 + unit)
        gl.glBindTexture(gl.GL_TEXTURE_2D, depth)
    gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MIN_FILTER, gl.GL_NEAREST)
    gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MAG_FILTER, gl.GL_NEAREST)
    gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_COMPARE_MODE, gl.GL_NONE)
    gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_WRAP_S, gl.GL_CLAMP_TO_BORDER)
    gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_WRAP_T, gl.GL_CLAMP_TO_BORDER)
    gl.glTexParameterfv(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_BORDER_COLOR, [1., 1., 1., 1.])
    raw, comparison = [int(x) for x in gl.glGenSamplers(2)]
    for sampler, compare in ((raw, False), (comparison, True)):
        gl.glSamplerParameteri(sampler, gl.GL_TEXTURE_MIN_FILTER, gl.GL_LINEAR if compare else gl.GL_NEAREST)
        gl.glSamplerParameteri(sampler, gl.GL_TEXTURE_MAG_FILTER, gl.GL_LINEAR if compare else gl.GL_NEAREST)
        gl.glSamplerParameteri(sampler, gl.GL_TEXTURE_COMPARE_MODE, gl.GL_COMPARE_REF_TO_TEXTURE if compare else gl.GL_NONE)
        gl.glSamplerParameteri(sampler, gl.GL_TEXTURE_COMPARE_FUNC, gl.GL_LEQUAL)
        gl.glSamplerParameteri(sampler, gl.GL_TEXTURE_WRAP_S, gl.GL_CLAMP_TO_BORDER)
        gl.glSamplerParameteri(sampler, gl.GL_TEXTURE_WRAP_T, gl.GL_CLAMP_TO_BORDER)
        gl.glSamplerParameterfv(sampler, gl.GL_TEXTURE_BORDER_COLOR, [1., 1., 1., 1.])
    gl.glBindSampler(1, raw)
    gl.glBindSampler(2, comparison)
    gl.glActiveTexture(gl.GL_TEXTURE0)
    output = gl.glGenTextures(1)
    gl.glBindTexture(gl.GL_TEXTURE_2D, output)
    gl.glTexImage2D(gl.GL_TEXTURE_2D, 0, gl.GL_RGBA32F, width, height, 0, gl.GL_RGBA, gl.GL_FLOAT, None)
    gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MIN_FILTER, gl.GL_NEAREST)
    gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MAG_FILTER, gl.GL_NEAREST)
    fbo = gl.glGenFramebuffers(1)
    gl.glBindFramebuffer(gl.GL_FRAMEBUFFER, fbo)
    gl.glFramebufferTexture2D(gl.GL_FRAMEBUFFER, gl.GL_COLOR_ATTACHMENT0, gl.GL_TEXTURE_2D, output, 0)
    assert gl.glCheckFramebufferStatus(gl.GL_FRAMEBUFFER) == gl.GL_FRAMEBUFFER_COMPLETE
    gl.glViewport(0, 0, width, height)
    gl.glDisable(gl.GL_BLEND)
    gl.glDisable(gl.GL_DEPTH_TEST)
    vao = gl.glGenVertexArrays(1)
    gl.glBindVertexArray(vao)
    vbo = gl.glGenBuffers(1)
    gl.glBindBuffer(gl.GL_ARRAY_BUFFER, vbo)
    gl.glBufferData(gl.GL_ARRAY_BUFFER, np.array([-1,-1, 3,-1, -1,3], dtype=np.float32), gl.GL_STATIC_DRAW)

    def program(version):
        attribute = 'attribute' if version == '110' else 'in'
        varying = 'varying' if version == '110' else 'out'
        vertex = f'''#version {version}
{attribute} vec2 v_position;
{varying} vec4 shadow_position;
uniform float test_depth;
uniform vec2 test_gradient;
uniform vec2 test_uv_offset;
void main() {{
    vec2 uv = (v_position + 1.0) * 0.5 + test_uv_offset;
    float z = test_depth + dot(test_gradient, uv - 0.5);
    shadow_position = vec4(uv * 2.0 - 1.0, z * 2.0 - 1.0, 1.0);
    gl_Position = vec4(v_position, 0.0, 1.0);
}}'''
        # Link before validating, to assign distinct sampler units first.
        vs = shaders.compileShader(vertex, gl.GL_VERTEX_SHADER)
        fs = shaders.compileShader((SHADERS / version / 'flat.fs').read_text(), gl.GL_FRAGMENT_SHADER)
        p = gl.glCreateProgram()
        gl.glAttachShader(p, vs); gl.glAttachShader(p, fs); gl.glLinkProgram(p)
        assert gl.glGetProgramiv(p, gl.GL_LINK_STATUS), gl.glGetProgramInfoLog(p)
        gl.glDeleteShader(vs); gl.glDeleteShader(fs)
        gl.glUseProgram(p)
        for name, value in (('shadow_map', 1), ('shadow_map_pcf', 2), ('shadow_enabled', 1)):
            gl.glUniform1i(gl.glGetUniformLocation(p, name), value)
        gl.glUniform4f(gl.glGetUniformLocation(p, 'uniform_color'), 1, 1, 1, 1)
        gl.glUniform2f(gl.glGetUniformLocation(p, 'shadow_map_texel_size'), 1/size, 1/size)
        gl.glUniform2f(gl.glGetUniformLocation(p, 'shadow_penumbra_scale'), .05, .05)
        gl.glUniform1f(gl.glGetUniformLocation(p, 'shadow_bias'), .00001)
        loc = gl.glGetAttribLocation(p, 'v_position')
        gl.glEnableVertexAttribArray(loc)
        gl.glVertexAttribPointer(loc, 2, gl.GL_FLOAT, False, 0, ctypes.c_void_p(0))
        return p

    def render(p, depths, hardware=True, receiver=.6, gradient=(0, 0), offset=(0, 0)):
        gl.glUseProgram(p)
        gl.glActiveTexture(gl.GL_TEXTURE1)
        gl.glBindTexture(gl.GL_TEXTURE_2D, depth)
        gl.glTexImage2D(gl.GL_TEXTURE_2D, 0, gl.GL_DEPTH_COMPONENT24, size, size, 0,
                        gl.GL_DEPTH_COMPONENT, gl.GL_FLOAT, np.ascontiguousarray(depths, dtype=np.float32))
        gl.glBindSampler(1, raw if hardware else 0)
        gl.glBindSampler(2, comparison if hardware else 0)
        gl.glUniform1i(gl.glGetUniformLocation(p, 'shadow_hardware_pcf'), int(hardware))
        gl.glUniform1f(gl.glGetUniformLocation(p, 'test_depth'), receiver)
        gl.glUniform2f(gl.glGetUniformLocation(p, 'test_gradient'), *gradient)
        gl.glUniform2f(gl.glGetUniformLocation(p, 'test_uv_offset'), *offset)
        gl.glValidateProgram(p)
        assert gl.glGetProgramiv(p, gl.GL_VALIDATE_STATUS), gl.glGetProgramInfoLog(p)
        gl.glDrawArrays(gl.GL_TRIANGLES, 0, 3)
        image = np.frombuffer(gl.glReadPixels(0, 0, width, height, gl.GL_RGBA, gl.GL_FLOAT), dtype=np.float32)
        image = image.reshape(height, width, 4)[:, :, 0].copy()
        assert np.isfinite(image).all()
        assert image.min() >= -1.e-5 and image.max() <= 1.00001
        assert gl.glGetError() == gl.GL_NO_ERROR
        return image

    ones = np.ones((size, size), np.float32)
    blocked = ones * .4
    edge = ones.copy(); edge[:, :size//2] = .4
    centers = (np.arange(size) + .5)/size
    plane = .6 + .15*(centers[None, :]-.5) + .2*(centers[:, None]-.5)
    try:
        for version in ('110', '140'):
            p = program(version)
            for hardware in (True, False):
                assert np.allclose(render(p, ones, hardware), 1)
                assert np.allclose(render(p, blocked, hardware)[:, 8:-8], 0)
                assert np.allclose(render(p, plane, hardware, gradient=(.15,.2)), 1)
                assert np.allclose(render(p, blocked, hardware, receiver=-.1), 1)
                assert np.allclose(render(p, blocked, hardware, receiver=1.1), 1)
                assert np.allclose(render(p, blocked, hardware, offset=(2, 0)), 1)
                visibility = render(p, edge, hardware)
                levels = np.unique(np.rint(visibility*4096)).size
                assert levels > 32, f'Visibility still quantized: {levels} levels'
                thin = ones.copy(); thin[:, size//2] = .4
                assert render(p, thin, hardware)[:, size//2].mean() < .999
                print(f'GPU GLSL {version}, hardware_pcf={hardware}: lit/shadow/slope/bounds/thin caster; {levels} levels', flush=True)
            near = render(p, edge, True, receiver=.41).mean(axis=0)
            far = render(p, edge, True, receiver=.8).mean(axis=0)
            near_width = ((near > .02) & (near < .98)).sum()
            far_width = ((far > .02) & (far < .98)).sum()
            assert far_width > near_width, (near_width, far_width)
            print(f'Contact hardening GLSL {version}: {near_width} -> {far_width} transition pixels', flush=True)
            gl.glDeleteProgram(p)
    finally:
        glfw.destroy_window(window)
        glfw.terminate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu', action='store_true')
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Contracts))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.gpu:
        compile_glsl()
        cpp_smoke()
        gpu_visibility()
    print('PCSS checks passed. Target-device FPS and full application build are still required.', flush=True)


if __name__ == '__main__':
    main()
