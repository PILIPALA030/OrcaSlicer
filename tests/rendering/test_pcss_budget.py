#!/usr/bin/env python3
"""PCSS v2 regression: python3 tests/rendering/test_pcss_budget.py [--gpu].

Math/source checks use the standard library. --gpu needs Linux Mesa EGL/GL,
not PyOpenGL. Software rendering is a correctness check, NOT an FPS benchmark.
"""
import argparse
import ctypes as C
import ctypes.util
import math
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('flat', 'gouraud', 'gouraud_light')


def kernel(source):
    return source[source.index('// PCSS v2:'):source.index('void main()')].replace('texture2D(', 'texture(')


class MathTests(unittest.TestCase):
    def test_six_kernels_match(self):
        sources = [(ROOT / f'resources/shaders/{v}/{n}.fs').read_text() for v in ('110', '140') for n in NAMES]
        self.assertTrue(all(kernel(s) == kernel(sources[0]) for s in sources))
        for source in sources:
            main = source[source.index('void main()'):]
            if 'discard;' in main:
                self.assertLess(main.index('shadow_receiver_gradient()'), main.index('discard;'))
            self.assertNotIn('shadow_area_weights', source)

    def test_directional_radius_in_world_units(self):
        for near, far in ((1, 300), (51, 351), (-500, 2000)):
            receiver, blocker = (160-near)/(far-near), (100-near)/(far-near)
            scale = .035 * (2/200) / (2/(far-near))
            self.assertAlmostEqual((receiver-blocker)*scale, 60*.035/200)
        radii = [min(gap*.035*512/200, 8) for gap in (0, 10, 20, 60)]
        self.assertEqual(radii[0], 0)
        self.assertGreater(radii[-1], 5)
        self.assertTrue(all(a < b for a, b in zip(radii, radii[1:])))

    def test_samples_are_balanced_and_in_disk(self):
        source = kernel((ROOT / 'resources/shaders/140/flat.fs').read_text())
        block = source[source.index('vec2 shadow_filter_offset'):source.index('float shadow_compare')]
        points = [tuple(map(float, p)) for p in re.findall(r'return vec2\(([-.0-9]+), ([-.0-9]+)\)', block)]
        self.assertEqual(len(points), 12)
        self.assertTrue(all(x*x+y*y <= 1.00001 for x, y in points))
        self.assertAlmostEqual(sum(p[0] for p in points), 0)
        self.assertAlmostEqual(sum(p[1] for p in points), 0)
        self.assertIn('if (i == 0) return vec2(0.0);', source)

    def test_sampling_budget_and_actual_resampling(self):
        source = kernel((ROOT / 'resources/shaders/140/flat.fs').read_text())
        self.assertIn('i < 9', source)
        self.assertIn('i < 12', source)
        bilinear = source[source.index('float shadow_bilinear_pcf'):source.index('float pcss_shadow_factor')]
        self.assertEqual(bilinear.count('shadow_compare('), 4)
        self.assertIn('shadow_filter_offset(i) * radius', source)
        self.assertIn('8.0 * shadow_map_texel_size', source)
        self.assertNotIn('blocker_count >', source)  # Sparse search is not an umbra proof.
        self.assertNotIn('gl_FragCoord', source)  # No frame-dependent stochastic noise.


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class GL:
    """Typed ctypes binding for the isolated EGL test context."""
    def __init__(self):
        library = ctypes.util.find_library('EGL')
        if not library:
            raise RuntimeError('libEGL is required for --gpu')
        self.egl = C.CDLL(library)
        self.egl.eglGetProcAddress.argtypes = [C.c_char_p]
        self.egl.eglGetProcAddress.restype = C.c_void_p
        self.display = self.surface = self.context = None
        def egl(name, result, args):
            fn = getattr(self.egl, name)
            fn.restype, fn.argtypes = result, args
            return fn
        I, U, P = C.c_int, C.c_uint, C.c_void_p
        initialize = egl('eglInitialize', U, [P, C.POINTER(I), C.POINTER(I)])
        choose = egl('eglChooseConfig', U, [P, C.POINTER(I), C.POINTER(P), I, C.POINTER(I)])
        bind = egl('eglBindAPI', U, [U])
        pbuffer = egl('eglCreatePbufferSurface', P, [P, P, C.POINTER(I)])
        create = egl('eglCreateContext', P, [P, P, P, C.POINTER(I)])
        self.make_current = egl('eglMakeCurrent', U, [P, P, P, P])
        self.destroy_context = egl('eglDestroyContext', U, [P, P])
        self.destroy_surface = egl('eglDestroySurface', U, [P, P])
        self.terminate = egl('eglTerminate', U, [P])
        platform = self.egl.eglGetProcAddress(b'eglGetPlatformDisplayEXT')
        require(platform, 'Surfaceless EGL is unavailable')
        get_display = C.CFUNCTYPE(P, U, P, C.POINTER(I))(platform)
        self.display = get_display(0x31DD, None, None)
        major, minor = I(), I()
        require(initialize(self.display, C.byref(major), C.byref(minor)), 'eglInitialize failed')
        config, count = P(), I()
        attributes = (I*13)(0x3033, 1, 0x3040, 8, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038)
        require(choose(self.display, attributes, C.byref(config), 1, C.byref(count)) and count.value == 1,
                'No OpenGL pbuffer config')
        require(bind(0x30A2), 'eglBindAPI failed')
        self.surface = pbuffer(self.display, config, (I*5)(0x3057, 16, 0x3056, 16, 0x3038))
        self.context = create(self.display, config, None, (I*1)(0x3038))
        require(self.context and self.surface and self.make_current(self.display, self.surface, self.surface, self.context),
                'Could not make OpenGL context current')
        F = C.c_float
        declarations = {
            'GetString': (C.c_char_p, [U]), 'GetError': (U, []),
            'CreateShader': (U, [U]), 'ShaderSource': (None, [U,I,C.POINTER(C.c_char_p),C.POINTER(I)]),
            'CompileShader': (None,[U]), 'GetShaderiv': (None,[U,U,C.POINTER(I)]),
            'GetShaderInfoLog': (None,[U,I,C.POINTER(I),P]), 'DeleteShader': (None,[U]),
            'CreateProgram': (U,[]), 'AttachShader': (None,[U,U]), 'LinkProgram': (None,[U]),
            'GetProgramiv': (None,[U,U,C.POINTER(I)]), 'GetProgramInfoLog': (None,[U,I,C.POINTER(I),P]),
            'UseProgram': (None,[U]), 'DeleteProgram': (None,[U]),
            'GetUniformLocation': (I,[U,C.c_char_p]), 'GetAttribLocation': (I,[U,C.c_char_p]),
            'Uniform1i': (None,[I,I]), 'Uniform1f': (None,[I,F]), 'Uniform2f': (None,[I,F,F]),
            'Uniform4f': (None,[I,F,F,F,F]), 'UniformMatrix4fv': (None,[I,I,C.c_ubyte,C.POINTER(F)]),
            'GenTextures': (None,[I,C.POINTER(U)]), 'BindTexture': (None,[U,U]),
            'ActiveTexture': (None,[U]), 'TexParameteri': (None,[U,U,I]),
            'TexImage2D': (None,[U,I,I,I,I,I,U,U,P]),
            'GenFramebuffers': (None,[I,C.POINTER(U)]), 'BindFramebuffer': (None,[U,U]),
            'FramebufferTexture2D': (None,[U,U,U,U,I]), 'CheckFramebufferStatus': (U,[U]),
            'Viewport': (None,[I,I,I,I]), 'Disable': (None,[U]),
            'GenBuffers': (None,[I,C.POINTER(U)]), 'BindBuffer': (None,[U,U]),
            'BufferData': (None,[U,C.c_ssize_t,P,U]), 'EnableVertexAttribArray': (None,[U]),
            'VertexAttribPointer': (None,[U,I,U,C.c_ubyte,I,P]), 'DrawArrays': (None,[U,I,I]),
            'ReadPixels': (None,[I,I,I,I,U,U,P]),
        }
        self.functions = {}
        for name, (result, args) in declarations.items():
            address = self.egl.eglGetProcAddress(('gl'+name).encode())
            require(address, 'Missing gl'+name)
            self.functions[name] = C.CFUNCTYPE(result, *args)(address)

    def __getattr__(self, name):
        return self.functions[name]

    def close(self):
        self.make_current(self.display, None, None, None)
        self.destroy_context(self.display, self.context)
        self.destroy_surface(self.display, self.surface)
        self.terminate(self.display)

    def shader(self, text, kind):
        shader = self.CreateShader(kind)
        source = C.c_char_p(text.encode())
        self.ShaderSource(shader, 1, C.byref(source), None)
        self.CompileShader(shader)
        ok = C.c_int()
        self.GetShaderiv(shader, 0x8B81, C.byref(ok))
        if not ok.value:
            log = C.create_string_buffer(16384)
            self.GetShaderInfoLog(shader, len(log), None, log)
            raise AssertionError(log.value.decode())
        return shader

    def program(self, vertex, fragment):
        vs, fs = self.shader(vertex, 0x8B31), self.shader(fragment, 0x8B30)
        program = self.CreateProgram()
        self.AttachShader(program, vs)
        self.AttachShader(program, fs)
        self.LinkProgram(program)
        ok = C.c_int()
        self.GetProgramiv(program, 0x8B82, C.byref(ok))
        if not ok.value:
            log = C.create_string_buffer(16384)
            self.GetProgramInfoLog(program, len(log), None, log)
            raise AssertionError(log.value.decode())
        self.DeleteShader(vs)
        self.DeleteShader(fs)
        return program


def gpu_tests():
    gl = GL()
    try:
        print('GL:', gl.GetString(0x1F02).decode())
        print('Renderer:', gl.GetString(0x1F01).decode())
        for version in ('110', '140'):
            for name in NAMES:
                folder = ROOT / f'resources/shaders/{version}'
                vertex = (folder / (name+'.vs')).read_text()
                fragment = (folder / (name+'.fs')).read_text()
                for env in (False, True) if name == 'gouraud' else (False,):
                    source = fragment
                    if env:
                        first, rest = source.split('\n', 1)
                        source = first+'\n#define ENABLE_ENVIRONMENT_MAP\n'+rest
                    gl.DeleteProgram(gl.program(vertex, source))
                    print(f'Linked actual GLSL {version}/{name}, environment={env}')
        size, width, height = 512, 1024, 16
        depth = C.c_uint()
        gl.GenTextures(1, C.byref(depth))
        gl.ActiveTexture(0x84C1)
        gl.BindTexture(0x0DE1, depth)
        for key, value in ((0x2801,0x2600),(0x2800,0x2600),(0x884C,0),(0x2802,0x812F),(0x2803,0x812F)):
            gl.TexParameteri(0x0DE1, key, value)
        output, fbo, vbo = C.c_uint(), C.c_uint(), C.c_uint()
        gl.GenTextures(1, C.byref(output))
        gl.ActiveTexture(0x84C0)
        gl.BindTexture(0x0DE1, output)
        gl.TexImage2D(0x0DE1,0,0x8814,width,height,0,0x1908,0x1406,None)
        gl.TexParameteri(0x0DE1,0x2801,0x2600)
        gl.TexParameteri(0x0DE1,0x2800,0x2600)
        gl.GenFramebuffers(1, C.byref(fbo))
        gl.BindFramebuffer(0x8D40, fbo)
        gl.FramebufferTexture2D(0x8D40,0x8CE0,0x0DE1,output,0)
        require(gl.CheckFramebufferStatus(0x8D40) == 0x8CD5, 'Incomplete test framebuffer')
        gl.Viewport(0,0,width,height)
        gl.Disable(0x0BE2)
        gl.Disable(0x0B71)
        gl.GenBuffers(1, C.byref(vbo))
        gl.BindBuffer(0x8892,vbo)
        vertices = (C.c_float*6)(-1,-1,3,-1,-1,3)
        gl.BufferData(0x8892,C.sizeof(vertices),vertices,0x88E4)
        ones, blocked = [1.]*(size*size), [.4]*(size*size)
        edge = [.4 if x < size//2 else 1. for y in range(size) for x in range(size)]
        thin = [.4 if x == size//2 else 1. for y in range(size) for x in range(size)]
        plane = [.6+.15*((x+.5)/size-.5)+.2*((y+.5)/size-.5) for y in range(size) for x in range(size)]
        for version in ('110','140'):
            vertex = f'''#version {version}
{('attribute' if version=='110' else 'in')} vec2 v_position;
{('varying' if version=='110' else 'out')} vec4 shadow_position;
uniform float test_depth;
uniform vec2 test_gradient;
uniform vec2 test_offset;
void main() {{
    vec2 uv = vec2(0.5) + v_position * vec2(32.0/512.0, 8.0/512.0) + test_offset;
    float z = test_depth + dot(test_gradient, uv - vec2(0.5));
    shadow_position = vec4(uv*2.0-1.0, z*2.0-1.0, 1.0);
    gl_Position = vec4(v_position, 0.0, 1.0);
}}
'''
            program = gl.program(vertex, (ROOT / f'resources/shaders/{version}/flat.fs').read_text())
            attribute = gl.GetAttribLocation(program,b'v_position')
            gl.EnableVertexAttribArray(attribute)
            gl.VertexAttribPointer(attribute,2,0x1406,0,0,None)
            def render(values, receiver=.6, gradient=(0,0), offset=(0,0), enabled=True, zspan=300):
                gl.UseProgram(program)
                def loc(name): return gl.GetUniformLocation(program,name.encode())
                gl.Uniform1i(loc('shadow_map'),1)
                gl.Uniform1i(loc('shadow_enabled'),int(enabled))
                gl.Uniform4f(loc('uniform_color'),1,1,1,1)
                gl.Uniform2f(loc('shadow_map_texel_size'),1/size,1/size)
                gl.Uniform1f(loc('shadow_light_size'),.035)
                gl.Uniform1f(loc('shadow_bias'),.0015)
                gl.Uniform1f(loc('test_depth'),receiver)
                gl.Uniform2f(loc('test_gradient'),*gradient)
                gl.Uniform2f(loc('test_offset'),*offset)
                matrix = (C.c_float*16)(.01,0,0,0,0,.01,0,0,0,0,-2/zspan,0,0,0,0,1)
                gl.UniformMatrix4fv(loc('shadow_matrix'),1,0,matrix)
                gl.ActiveTexture(0x84C1)
                gl.BindTexture(0x0DE1,depth)
                data = (C.c_float*(size*size))(*values)
                gl.TexImage2D(0x0DE1,0,0x81A6,size,size,0,0x1902,0x1406,data)
                gl.DrawArrays(0x0004,0,3)
                pixels = (C.c_float*(width*4))()
                gl.ReadPixels(0,height//2,width,1,0x1908,0x1406,pixels)
                require(gl.GetError() == 0, 'GL error during fixture')
                result = list(pixels)[::4]
                require(all(math.isfinite(x) and -.00001 <= x <= 1.00001 for x in result), 'Invalid visibility')
                return result
            require(min(render(ones)) > .999, 'Full light')
            require(max(render(blocked)) < .001, 'Full shadow')
            require(min(render(blocked,enabled=False)) > .999, 'Disabled shadows')
            require(min(render(plane,gradient=(.15,.2))) > .999, 'Receiver-plane correction')
            for receiver in (-.1,1.1):
                require(min(render(blocked,receiver=receiver)) > .999, 'Light-space Z bounds')
            require(min(render(blocked,offset=(.75,0))) > .999, 'UV bounds')
            require(min(render(thin,receiver=.401)) < .1, 'Thin contact blocker missing')
            widths = []
            for receiver in (.401,.45,.6):
                line = render(edge,receiver=receiver)
                widths.append(sum(.05 < x < .95 for x in line))
            require(widths[0] < widths[1] < widths[2], 'Penumbra did not grow with separation')
            require(widths[2] > 3*widths[0] and widths[2] > 32, 'Still restricted to a local PCF edge')
            first = render(edge)
            moved = [(.4*300+50)/450 if x < size//2 else 1. for y in range(size) for x in range(size)]
            second = render(moved,receiver=(.6*300+50)/450,zspan=450)
            require(max(abs(a-b) for a,b in zip(first,second)) < .001, 'World-space radius changed with depth range')
            print(f'GLSL {version}: synthetic fixtures passed; 5%-95% edge widths={widths} pixels (16 pixels/texel)')
            gl.DeleteProgram(program)
    finally:
        gl.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu', action='store_true')
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(MathTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.gpu:
        gpu_tests()


if __name__ == '__main__':
    main()
