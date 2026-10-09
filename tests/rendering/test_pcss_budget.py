#!/usr/bin/env python3
"""Bounded PCSS regression. Linux: python3 tests/rendering/test_pcss_budget.py --gpu.

The optional GPU tests need Mesa EGL/OpenGL shared libraries, not PyOpenGL.
A surfaceless software context validates shader behavior; it is NOT an FPS test.
"""
import argparse
import ctypes as C
import ctypes.util
from pathlib import Path
import math
import unittest

ROOT = Path(__file__).resolve().parents[2]


def area_weights(fraction, radius):
    return [max(min(i+1, fraction+radius)-max(i, fraction-radius), 0)/(2*radius)
            for i in (-1, 0, 1)]


class MathTests(unittest.TestCase):
    def test_filter_partitions_unity(self):
        for fraction in (0, .1, .49, .5, .9, .999999):
            for radius in (.5, .6, .75, 1):
                weights = area_weights(fraction, radius)
                self.assertAlmostEqual(sum(weights), 1)
                self.assertTrue(all(0 <= w <= 1 for w in weights))

    def test_contact_limit_is_bilinear(self):
        for f in (0, .1, .49, .5, .9, .999):
            expected = [max(.5-f, 0), 1-abs(f-.5), max(f-.5, 0)]
            for actual, weight in zip(area_weights(f, .5), expected):
                self.assertAlmostEqual(actual, weight)

    def test_orthographic_units_ignore_near_plane(self):
        for near, far in ((1, 300), (51, 351), (-500, 2000)):
            receiver = (160-near)/(far-near)
            blocker = (100-near)/(far-near)
            scale = .035 * (2/200) / (2/(far-near))
            self.assertAlmostEqual((receiver-blocker)*scale, 60*.035/200)

    def test_kernels_match(self):
        kernels = []
        for version in ('110', '140'):
            for name in ('flat', 'gouraud', 'gouraud_light'):
                source = (ROOT / f'resources/shaders/{version}/{name}.fs').read_text()
                start = source.index('// Bounded-cost PCSS.')
                end = source.index('void main()')
                kernels.append(source[start:end].replace('texture2D(', 'texture('))
                main = source[end:]
                if 'discard;' in main:
                    self.assertLess(main.index('shadow_receiver_gradient()'), main.index('discard;'))
                self.assertNotIn('shadow_poisson_offset', source)
        self.assertTrue(all(k == kernels[0] for k in kernels))

    def test_fetch_budget(self):
        source = (ROOT / 'resources/shaders/140/flat.fs').read_text()
        row = source[source.index('vec3 shadow_depth_row'):source.index('vec3 shadow_area_weights')]
        body = source[source.index('float pcss_shadow_factor'):source.index('void main()')]
        self.assertEqual(row.count('shadow_raw_depth('), 3)
        self.assertEqual(body.count('shadow_depth_row('), 3)
        self.assertNotIn('texture(', body)
        self.assertIn('vec2(0.5), vec2(1.0)', body)


class GL:
    """Small typed ctypes binding for the test's EGL/OpenGL operations only."""
    def __init__(self):
        path = ctypes.util.find_library('EGL')
        if not path:
            raise RuntimeError('libEGL is required for --gpu')
        self.egl = C.CDLL(path)
        self.egl.eglGetProcAddress.argtypes = [C.c_char_p]
        self.egl.eglGetProcAddress.restype = C.c_void_p
        self.display = None
        self.surface = None
        self.context = None
        def egl(name, restype, args):
            fn = getattr(self.egl, name)
            fn.restype, fn.argtypes = restype, args
            return fn
        self.initialize = egl('eglInitialize', C.c_uint, [C.c_void_p, C.POINTER(C.c_int), C.POINTER(C.c_int)])
        choose = egl('eglChooseConfig', C.c_uint, [C.c_void_p, C.POINTER(C.c_int), C.POINTER(C.c_void_p), C.c_int, C.POINTER(C.c_int)])
        bind_api = egl('eglBindAPI', C.c_uint, [C.c_uint])
        pbuffer = egl('eglCreatePbufferSurface', C.c_void_p, [C.c_void_p, C.c_void_p, C.POINTER(C.c_int)])
        create = egl('eglCreateContext', C.c_void_p, [C.c_void_p, C.c_void_p, C.c_void_p, C.POINTER(C.c_int)])
        self.make_current = egl('eglMakeCurrent', C.c_uint, [C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p])
        self.destroy_surface = egl('eglDestroySurface', C.c_uint, [C.c_void_p, C.c_void_p])
        self.destroy_context = egl('eglDestroyContext', C.c_uint, [C.c_void_p, C.c_void_p])
        self.terminate = egl('eglTerminate', C.c_uint, [C.c_void_p])
        platform = self.egl.eglGetProcAddress(b'eglGetPlatformDisplayEXT')
        if not platform:
            raise RuntimeError('Surfaceless EGL is unavailable')
        get_display = C.CFUNCTYPE(C.c_void_p, C.c_uint, C.c_void_p, C.POINTER(C.c_int))(platform)
        self.display = get_display(0x31DD, None, None)  # EGL_PLATFORM_SURFACELESS_MESA
        major, minor = C.c_int(), C.c_int()
        if not self.initialize(self.display, C.byref(major), C.byref(minor)):
            raise RuntimeError('eglInitialize failed')
        config, count = C.c_void_p(), C.c_int()
        attributes = (C.c_int*13)(0x3033, 1, 0x3040, 8, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038)
        if not choose(self.display, attributes, C.byref(config), 1, C.byref(count)) or count.value != 1:
            raise RuntimeError('No EGL OpenGL pbuffer config')
        if not bind_api(0x30A2):  # EGL_OPENGL_API
            raise RuntimeError('eglBindAPI failed')
        self.surface = pbuffer(self.display, config, (C.c_int*5)(0x3057, 512, 0x3056, 128, 0x3038))
        self.context = create(self.display, config, None, (C.c_int*1)(0x3038))
        if not self.context or not self.surface or not self.make_current(self.display, self.surface, self.surface, self.context):
            raise RuntimeError('Could not create/current an OpenGL context')
        U, I, F, P = C.c_uint, C.c_int, C.c_float, C.c_void_p
        self.functions = {}
        declarations = {
            'GetString': (C.c_char_p, [U]), 'GetError': (U, []),
            'CreateShader': (U, [U]), 'ShaderSource': (None, [U,I,C.POINTER(C.c_char_p),C.POINTER(I)]),
            'CompileShader': (None,[U]), 'GetShaderiv':(None,[U,U,C.POINTER(I)]),
            'GetShaderInfoLog':(None,[U,I,C.POINTER(I),P]), 'DeleteShader':(None,[U]),
            'CreateProgram':(U,[]), 'AttachShader':(None,[U,U]), 'LinkProgram':(None,[U]),
            'GetProgramiv':(None,[U,U,C.POINTER(I)]), 'GetProgramInfoLog':(None,[U,I,C.POINTER(I),P]),
            'UseProgram':(None,[U]), 'DeleteProgram':(None,[U]),
            'GetUniformLocation':(I,[U,C.c_char_p]), 'GetAttribLocation':(I,[U,C.c_char_p]),
            'Uniform1i':(None,[I,I]), 'Uniform1f':(None,[I,F]), 'Uniform2f':(None,[I,F,F]),
            'Uniform4f':(None,[I,F,F,F,F]), 'UniformMatrix4fv':(None,[I,I,C.c_ubyte,C.POINTER(F)]),
            'GenTextures':(None,[I,C.POINTER(U)]), 'BindTexture':(None,[U,U]),
            'ActiveTexture':(None,[U]), 'TexParameteri':(None,[U,U,I]),
            'TexParameterfv':(None,[U,U,C.POINTER(F)]), 'TexImage2D':(None,[U,I,I,I,I,I,U,U,P]),
            'GenFramebuffers':(None,[I,C.POINTER(U)]), 'BindFramebuffer':(None,[U,U]),
            'FramebufferTexture2D':(None,[U,U,U,U,I]), 'CheckFramebufferStatus':(U,[U]),
            'Viewport':(None,[I,I,I,I]), 'Disable':(None,[U]),
            'GenBuffers':(None,[I,C.POINTER(U)]), 'BindBuffer':(None,[U,U]),
            'BufferData':(None,[U,C.c_ssize_t,P,U]), 'EnableVertexAttribArray':(None,[U]),
            'VertexAttribPointer':(None,[U,I,U,C.c_ubyte,I,P]), 'DrawArrays':(None,[U,I,I]),
            'ReadPixels':(None,[I,I,I,I,U,U,P]),
        }
        for name, (result, args) in declarations.items():
            address = self.egl.eglGetProcAddress(('gl'+name).encode())
            if not address:
                raise RuntimeError('Missing gl'+name)
            self.functions[name] = C.CFUNCTYPE(result, *args)(address)

    def __getattr__(self, name):
        return self.functions[name]

    def close(self):
        self.make_current(self.display, None, None, None)
        self.destroy_context(self.display, self.context)
        self.destroy_surface(self.display, self.surface)
        self.terminate(self.display)

    def shader(self, source, kind):
        shader = self.CreateShader(kind)
        text = C.c_char_p(source.encode())
        self.ShaderSource(shader, 1, C.byref(text), None)
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
        self.AttachShader(program, vs); self.AttachShader(program, fs)
        self.LinkProgram(program)
        ok = C.c_int()
        self.GetProgramiv(program, 0x8B82, C.byref(ok))
        if not ok.value:
            log = C.create_string_buffer(16384)
            self.GetProgramInfoLog(program, len(log), None, log)
            raise AssertionError(log.value.decode())
        self.DeleteShader(vs); self.DeleteShader(fs)
        return program


def gpu_tests():
    gl = GL()
    try:
        print('GL:', gl.GetString(0x1F02).decode())
        print('Renderer:', gl.GetString(0x1F01).decode())
        # Compile every actual fragment, including both environment-map paths.
        for version in ('110', '140'):
            for name in ('flat', 'gouraud', 'gouraud_light'):
                fragment = (ROOT / f'resources/shaders/{version}/{name}.fs').read_text()
                for env in (False, True) if name=='gouraud' else (False,):
                    source = fragment
                    if env:
                        first, rest = source.split('\n', 1)
                        source = first+'\n#define ENABLE_ENVIRONMENT_MAP\n'+rest
                    shader = gl.shader(source, 0x8B30)
                    gl.DeleteShader(shader)
                    # When running in the full repository, also link the actual pair.
                    vs = ROOT / f'resources/shaders/{version}/{name}.vs'
                    if vs.exists():
                        program = gl.program(vs.read_text(), source)
                        gl.DeleteProgram(program)
                    print(f'Compiled GLSL {version}/{name}, environment={env}')
        size, width, height = 512, 512, 128
        depth = C.c_uint(); gl.GenTextures(1, C.byref(depth))
        gl.ActiveTexture(0x84C1); gl.BindTexture(0x0DE1, depth)
        gl.TexParameteri(0x0DE1, 0x2801, 0x2600); gl.TexParameteri(0x0DE1, 0x2800, 0x2600)
        gl.TexParameteri(0x0DE1, 0x884C, 0)
        gl.TexParameteri(0x0DE1, 0x2802, 0x812D); gl.TexParameteri(0x0DE1, 0x2803, 0x812D)
        gl.TexParameterfv(0x0DE1, 0x1004, (C.c_float*4)(1,1,1,1))
        output = C.c_uint(); gl.GenTextures(1, C.byref(output))
        gl.ActiveTexture(0x84C0); gl.BindTexture(0x0DE1, output)
        gl.TexImage2D(0x0DE1, 0, 0x8814, width, height, 0, 0x1908, 0x1406, None)
        gl.TexParameteri(0x0DE1, 0x2801, 0x2600); gl.TexParameteri(0x0DE1, 0x2800, 0x2600)
        fbo = C.c_uint(); gl.GenFramebuffers(1, C.byref(fbo)); gl.BindFramebuffer(0x8D40, fbo)
        gl.FramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, output, 0)
        assert gl.CheckFramebufferStatus(0x8D40)==0x8CD5
        gl.Viewport(0,0,width,height); gl.Disable(0x0BE2); gl.Disable(0x0B71)
        vbo=C.c_uint(); gl.GenBuffers(1,C.byref(vbo)); gl.BindBuffer(0x8892,vbo)
        vertices=(C.c_float*6)(-1,-1,3,-1,-1,3)
        gl.BufferData(0x8892,C.sizeof(vertices),vertices,0x88E4)
        ones=[1.0]*(size*size)
        blocked=[.4]*(size*size)
        edge=[.4 if x<size//2 else 1.0 for y in range(size) for x in range(size)]
        thin=[.4 if x==size//2 else 1.0 for y in range(size) for x in range(size)]
        plane=[.6+.15*((x+.5)/size-.5)+.2*((y+.5)/size-.5) for y in range(size) for x in range(size)]
        def render(p, depths, receiver=.6, gradient=(0,0), offset=(0,0), enabled=True, zspan=300, extent=200):
            gl.UseProgram(p)
            def loc(name): return gl.GetUniformLocation(p,name.encode())
            gl.Uniform1i(loc('shadow_map'),1); gl.Uniform1i(loc('shadow_enabled'),int(enabled))
            gl.Uniform4f(loc('uniform_color'),1,1,1,1)
            gl.Uniform2f(loc('shadow_map_texel_size'),1/size,1/size)
            gl.Uniform1f(loc('shadow_light_size'),.035); gl.Uniform1f(loc('shadow_bias'),.0015)
            matrix=(C.c_float*16)(2/extent,0,0,0,0,2/extent,0,0,0,0,-2/zspan,0,0,0,0,1)
            gl.UniformMatrix4fv(loc('shadow_matrix'),1,0,matrix)
            gl.Uniform1f(loc('test_depth'),receiver)
            gl.Uniform2f(loc('test_gradient'),*gradient); gl.Uniform2f(loc('test_offset'),*offset)
            gl.ActiveTexture(0x84C1); gl.BindTexture(0x0DE1,depth)
            data=(C.c_float*(size*size))(*depths)
            gl.TexImage2D(0x0DE1,0,0x81A6,size,size,0,0x1902,0x1406,data)
            a=gl.GetAttribLocation(p,b'v_position'); gl.EnableVertexAttribArray(a)
            gl.VertexAttribPointer(a,2,0x1406,0,0,None)
            gl.DrawArrays(0x0004,0,3)
            pixels=(C.c_float*(width*height*4))()
            gl.ReadPixels(0,0,width,height,0x1908,0x1406,pixels)
            assert gl.GetError()==0
            values=list(pixels)[::4]
            assert all(math.isfinite(x) and -1.e-5<=x<=1.00001 for x in values)
            return values
        def all_close(values, desired):
            assert max(abs(v-desired) for v in values)<1.e-5
        for version in ('110','140'):
            attr='attribute' if version=='110' else 'in'
            varying='varying' if version=='110' else 'out'
            vertex=f'''#version {version}
{attr} vec2 v_position;
{varying} vec4 shadow_position;
uniform float test_depth;
uniform vec2 test_gradient;
uniform vec2 test_offset;
void main() {{
    vec2 uv = 0.5 + v_position * 0.03125 + test_offset;
    float z = test_depth + dot(test_gradient, uv - 0.5);
    shadow_position = vec4(uv * 2.0 - 1.0, z * 2.0 - 1.0, 1.0);
    gl_Position = vec4(v_position, 0.0, 1.0);
}}
'''
            p=gl.program(vertex,(ROOT/f'resources/shaders/{version}/flat.fs').read_text())
            all_close(render(p,ones),1)
            all_close(render(p,blocked),0)
            all_close(render(p,blocked,enabled=False),1)
            all_close(render(p,plane,gradient=(.15,.2)),1)
            for receiver in (-.1,1.1): all_close(render(p,blocked,receiver=receiver),1)
            all_close(render(p,blocked,offset=(2,0)),1)
            image=render(p,edge)
            levels=len(set(round(v*4096) for v in image))
            assert levels>16,levels
            thin_image=render(p,thin)
            assert sum(thin_image[y*width+width//2] for y in range(height))/height<.999
            near=render(p,edge,receiver=.405)
            far=render(p,edge,receiver=.8)
            near_width=sum(.01<x<.99 for x in near[:width])
            far_width=sum(.01<x<.99 for x in far[:width])
            assert far_width>near_width,(near_width,far_width)
            # Preserve physical caster/receiver positions while changing near/far.
            physical_edge=[100 if x<size//2 else None for y in range(size) for x in range(size)]
            images=[]
            for n,f in ((1,300),(51,351),(-500,2000)):
                depths=[1 if d is None else (d-n)/(f-n) for d in physical_edge]
                images.append(render(p,depths,receiver=(101-n)/(f-n),zspan=f-n))
            assert max(abs(a-b) for a,b in zip(images[0],images[1]))<1.e-4
            assert max(abs(a-b) for a,b in zip(images[0],images[2]))<1.e-4
            print(f'GLSL {version}: lit/shadow/off/slope/XYZ bounds/thin/near-far passed; '
                  f'{levels} visibility levels; penumbra {near_width}->{far_width} pixels')
            gl.DeleteProgram(p)
    finally:
        gl.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu',action='store_true')
    args=parser.parse_args()
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(MathTests))
    if not result.wasSuccessful(): raise SystemExit(1)
    if args.gpu: gpu_tests()
    print('Passed. Full application integration and target-device frame times remain unmeasured.')


if __name__=='__main__': main()
