#!/usr/bin/env python3
"""Renderer diagnostic tests. No application, QEM, or hardware FPS claims.

Python tests exercise the actual generator and CMake opt-in using small source
fixtures. --cpp compiles the actual profiler header against a deterministic GL
protocol double; --sanitize additionally runs ASan/UBSan (GCC/Clang).
"""
from __future__ import annotations
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools/render_profile"
spec = importlib.util.spec_from_file_location("render_profile_generator", TOOLS / "instrument.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

# Deliberately fake GL/utility headers: assert that reads never wait on GPU.
STUBS = {
    'GL/glew.h': r"""#pragma once
#include <cassert>
#include <cstdint>
#include <map>
#include <iostream>
using GLuint=unsigned; using GLenum=unsigned; using GLint=int; using GLsizei=int; using GLuint64=uint64_t; using GLubyte=unsigned char;
#define GL_TIMESTAMP 0x8E28
#define GL_QUERY_COUNTER_BITS 0x8864
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_QUERY_RESULT 0x8866
#define GL_SAMPLES 0x80A9
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLES 4
#define GL_TRIANGLE_STRIP 5
#define GL_TRIANGLE_FAN 6
#define GL_LINES 1
inline bool GLEW_VERSION_3_3=true, GLEW_ARB_timer_query=true;
inline unsigned context_id=1, next_query=1, reads=0, polls=0, deletes=0, issued=0, allocations=0;
inline bool available=false;
inline int fake_bits=64;
inline uint64_t tick=100000;
struct Query { unsigned owner; uint64_t time; };
inline std::map<GLuint, Query> objects;
inline void glGetQueryiv(GLenum,GLenum,GLint* bits){*bits=fake_bits;}
inline const GLubyte* glGetString(GLenum){return reinterpret_cast<const GLubyte*>("fake GL (protocol test only)");}
inline void glGetIntegerv(GLenum,GLint* n){*n=4;}
inline void glGenQueries(GLsizei n,GLuint* result){++allocations; for(int i=0;i<n;++i){result[i]=next_query++; objects[result[i]]={context_id,0};}}
inline void glDeleteQueries(GLsizei n,const GLuint* ids){for(int i=0;i<n;++i){assert(objects.at(ids[i]).owner==context_id);objects.erase(ids[i]);++deletes;}}
inline void glQueryCounter(GLuint id,GLenum target){assert(target==GL_TIMESTAMP);assert(objects.at(id).owner==context_id);tick+=10000;objects[id].time = fake_bits==64 ? tick : tick&((uint64_t(1)<<fake_bits)-1);++issued;}
inline void glGetQueryObjectiv(GLuint id,GLenum parameter,GLint* ready){assert(parameter==GL_QUERY_RESULT_AVAILABLE);assert(objects.at(id).owner==context_id);*ready=available; ++polls;}
inline void glGetQueryObjectui64v(GLuint id,GLenum parameter,GLuint64* result){assert(available);assert(parameter==GL_QUERY_RESULT);assert(objects.at(id).owner==context_id);*result=objects.at(id).time;++reads;}
""",
    'boost/log/trivial.hpp': r"""#pragma once
#include <iostream>
#define BOOST_LOG_TRIVIAL(level) std::cerr
""",
    'boost/nowide/cstdlib.hpp': r"""#pragma once
#include <cstdlib>
namespace boost { namespace nowide { using std::getenv; } }
""",
    'boost/nowide/fstream.hpp': r"""#pragma once
#include <fstream>
namespace boost { namespace nowide { using std::ofstream; } }
""",
    'libslic3r/Utils.hpp': r"""#pragma once
#include <string>
namespace Slic3r { inline std::string data_dir(){return "/tmp";} inline std::string resources_dir(){return "fixture/resources";} }
""",
    'test.cpp': r"""#include "RenderProfile.hpp"
#include <cassert>
#include <thread>
namespace RP=Slic3r::GUI::RenderProfile;
static void one_frame(void* key, bool on, bool overflow=false){
    auto t=RP::entry_time();
    RP::Frame f(key, {0,800,600,on,false,false,true,1},t);
    f.ready(on);
    {
        RP::Scope pass("objects_opaque");
        RP::draw(GL_TRIANGLES,9000000,1,"gouraud");
        { RP::Scope upload("model_upload",false); RP::upload(1024); }
        {
            RP::Scope nested("selection_mask");
            RP::draw(GL_TRIANGLES,300,2,"selection_mask");
        }
        if(overflow) for(int i=0;i<160;++i){RP::Scope s("overflow");}
    }
    if(on){RP::Scope pass("shadow_geometry");RP::draw(GL_TRIANGLES,300000,1,"shadow_depth");}
    f.before_swap();
    {RP::Scope swap("swap",false);}
    f.finish();
    assert(RP::current()==nullptr);
}
int main(int argc,char** argv){
    const std::string test = argc>1 ? argv[1] : "normal";
    if(test=="disabled"){
        one_frame(reinterpret_cast<void*>(1),false);
        assert(allocations==0 && polls==0 && issued==0 && RP::registry().empty());
        return 0;
    }
    if(test=="unsupported"){GLEW_VERSION_3_3=GLEW_ARB_timer_query=false;}
    if(test=="zero_bits") fake_bits=0;
    if(test=="wrap"){fake_bits=32;tick=(uint64_t(1)<<32)-15000;}
    for(unsigned i=0;i<18;++i) one_frame(reinterpret_cast<void*>(1),i%2==0);
    assert(reads==0); // Never block or read an unavailable query, including ring overflow.
    if(test!="unsupported" && test!="zero_bits" && !RP::options().cpu_only) assert(allocations==RP::RING_SIZE);
    available=true;
    for(unsigned i=0;i<4;++i) one_frame(reinterpret_cast<void*>(1),true,i==3);
    RP::registry().at(reinterpret_cast<void*>(1))->poll(true);
    RP::registry().at(reinterpret_cast<void*>(1))->report(true);
    context_id=2;
    one_frame(reinterpret_cast<void*>(2),false);
    context_id=1;
    RP::release(reinterpret_cast<void*>(1),[]{return true;});
    context_id=2;
    RP::release(reinterpret_cast<void*>(2),[]{return true;});
    assert(objects.empty());
    assert(RP::registry().empty());
    if(test=="unsupported" || test=="zero_bits" || RP::options().cpu_only) assert(allocations==0 && reads==0 && polls==0);
    std::thread([]{RP::Job j(3000000);j.copied();j.simplified();j.result(100000);}).join();
    std::cout << "PASS " << test << ": issued="<<issued<<" reads="<<reads<<" polls="<<polls<<" deleted="<<deletes<<'\n';
}
""",
}

def fixtures():
    canvas = '''#include <GL/glew.h>
GLCanvas3D::~GLCanvas3D() { /* destructor */ }
void GLCanvas3D::render(bool only_init)
{
    const Size& cnv_size = get_canvas_size();
    if (m_picking_enabled) { _picking_pass(); }
    bool shadowMapReady = false;
    if (view) shadowMapReady = RenderShadowMap(camera);
    else
        m_shadowMap.valid = false;
    const auto setShadowEnabled = [](bool enabled) { shader_reset(); };
    if (m_picking_enabled)
        m_mouse.scene_position = _mouse_to_3d(m_mouse.position.cast<coord_t>());
    wxGetApp().imgui()->render();
    m_canvas->SwapBuffers();
}
bool GLCanvas3D::RenderShadowMap(const Camera& camera)
{
    m_shadowMap.valid = false;
    if (!requested) return false;
    GLShaderProgram* const shader = wxGetApp().get_shader("shadow_depth");
    const BoundingBoxf3 bounds = _max_bounding_box(false, true, true);
    EnsureShadowMapResources(size);
    shader->start_using();
    for (GLVolume* volume : m_volumes.volumes) { volume->render(); }
    shader->stop_using();
    m_shadowMap.valid = true;
    return true;
}
void GLCanvas3D::BindShadowUniforms(GLShaderProgram* shader) const
{
    shader->set_uniform("shadow_enabled", m_shadowMap.valid);
}
bool GLCanvas3D::EnsureShadowMapResources(unsigned size) { return true; }
void GLCanvas3D::_render_objects(GLVolumeCollection::ERenderType type, bool outline) {}
'''
    for name in ('_picking_pass', '_rectangular_selection_picking_pass', '_render_bed', '_render_platelist',
                 '_render_selection', '_render_overlays', '_render_background', '_render_current_gizmo',
                 '_render_sequential_clearance', 'RenderSelectionHighlightMask'):
        canvas += f'void GLCanvas3D::{name}() {{}}\n'
    model = '''#include <GL/glew.h>
void GLModel::render()
{
    glsafe(::glDrawElements(mode, range.second - range.first, index_type, (const void*)(range.first * Geometry::index_stride_bytes(data))));
}
void GLModel::render_instanced()
{
    glsafe(::glDrawElementsInstanced(mode, indices_count(), index_type, (const void*)0, instances_count));
}
bool GLModel::send_to_gpu()
{
    glsafe(::glBufferData(GL_ARRAY_BUFFER, data.vertices_size_bytes(), data.vertices.data(), GL_STATIC_DRAW));
    glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices_count * sizeof(unsigned char), x, GL_STATIC_DRAW));
    glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices_count * sizeof(unsigned short), x, GL_STATIC_DRAW));
    glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, data.indices_size_bytes(), x, GL_STATIC_DRAW));
    return true;
}
'''
    scene = '''#include <GL/glew.h>
// Fixture has m_lodSmallReady to enable LOD hooks.
void GLVolume::render()
{
    if (shader == nullptr)
        return;
    if (shadow) {
        if (shadow_model) { shadow_model->render(); }
    }
}
'''
    shader = '''#include <GL/glew.h>
void shader_loader() {
        s.close();
}
'''
    proxy = '''#include "GLModel.hpp"
class ShadowMeshProxy {
    GLModel* get(const std::shared_ptr<const TriangleMesh>& source) {
        const State state = m_task->state.load(std::memory_order_acquire);
        return nullptr;
    }
    void worker() {
            std::thread([source, task, slot] {
                    auto mesh = std::make_unique<indexed_triangle_set>(source->its);
                    its_quadric_edge_collapse(*mesh, TRIANGLE_BUDGET, nullptr, check_cancel);
                    task->result = std::move(mesh);
            }).detach();
    }
};
'''
    return dict(zip(('GLCanvas3D.cpp', 'GLModel.cpp', '3DScene.cpp', 'GLShader.cpp', 'ShadowMeshProxy.hpp'),
                    (canvas, model, scene, shader, proxy)))


def populate(root):
    gui = root / 'src/slic3r/GUI'
    gui.mkdir(parents=True)
    for name, text in fixtures().items():
        (gui / name).write_text(text)
    return gui


class GeneratorTests(unittest.TestCase):
    def test_lexical_balancing(self):
        text = 'void C::f() const { auto s=R"tag({})tag"; /* } */ const char c=\'}\'; } // {'
        self.assertEqual(len(generator.bodies(text, 'C::f')), 1)
        e = generator.Editor(text, 'fixture')
        e.scope('C::f', '"test"')
        self.assertIn('orca_profile_scope("test", true)', e.source)
        self.assertIn('R"tag({})tag"', e.source)

    def test_generate_is_idempotent_and_source_is_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gui = populate(root)
            before = {p.name: p.read_bytes() for p in gui.iterdir()}
            out = root / 'build/generated'
            manifest = generator.generate(gui, out)
            timestamps = {p.name: p.stat().st_mtime_ns for p in out.iterdir()}
            self.assertEqual(manifest, generator.generate(gui, out))
            self.assertEqual(timestamps, {p.name: p.stat().st_mtime_ns for p in out.iterdir()})
            self.assertEqual(before, {p.name: p.read_bytes() for p in gui.iterdir()})
            self.assertEqual(set(manifest['files']), set(before))
            self.assertIn('proxy_copy_and_actual_qem_wall_time', manifest['files']['ShadowMeshProxy.hpp']['hooks'])
            self.assertIn('ResolveSelectionHighlightMode', '\n'.join(manifest['files']['GLCanvas3D.cpp']['skipped']))
            generated = (out / 'GLCanvas3D.cpp').read_text()
            self.assertIn('before_swap();', generated)
            self.assertIn('"swap", false', generated)
            self.assertIn('"shadow_geometry"', generated)
            self.assertLess(generated.index('m_shadowMap.valid = false;'), generated.index('static_off()'))
            self.assertEqual((out / 'GLModel.cpp').read_text().count('::upload('), 4)
            self.assertIn('const bool shadow_enabled = false;', (out / 'GLShader.cpp').read_text())

    def test_bad_anchors_fail_before_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gui = populate(root)
            target = gui / 'GLModel.cpp'
            target.write_text(target.read_text().replace('data.indices_size_bytes()', 'changed_api()'))
            out = root / 'build'
            with self.assertRaisesRegex(ValueError, 'BufferData anchor missing'):
                generator.generate(gui, out)
            self.assertFalse(out.exists())
            with self.assertRaisesRegex(ValueError, 'outside'):
                generator.generate(gui, gui / 'generated')

    def test_duplicate_function_rejected(self):
        e = generator.Editor('void C::f() {} void C::f() {}', 'bad')
        with self.assertRaisesRegex(ValueError, 'found 2'):
            e.scope('C::f', '"test"')

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('c++'), 'CMake/C++ configuration unavailable')
    def test_cmake_opt_in_and_restoration(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            populate(root)
            (root / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.13)\nproject(ProfileFixture LANGUAGES CXX)\nadd_subdirectory(src/slic3r)\n')
            module = (TOOLS / 'RenderProfile.cmake').as_posix()
            (root / 'src/slic3r/CMakeLists.txt').write_text('''add_library(libslic3r_gui STATIC
    GUI/GLCanvas3D.cpp GUI/GLModel.cpp GUI/3DScene.cpp GUI/GLShader.cpp)
include("''' + module + '''")
get_target_property(result libslic3r_gui SOURCES)
file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${result}")
''')
            build = root / 'build'
            def configure(value):
                subprocess.run(['cmake', '-S', str(root), '-B', str(build),
                                '-DSLIC3R_RENDER_PROFILE=' + value], check=True, capture_output=True, text=True, timeout=30)
                return (build / 'sources.txt').read_text().split(';')
            original = configure('OFF')
            self.assertTrue(all(p.startswith('GUI/') for p in original))
            self.assertFalse((build / 'src/slic3r/render-profile').exists())
            generated = configure('ON')
            self.assertEqual(len(generated), 4)
            self.assertTrue(all('render-profile/' in p for p in generated))
            self.assertTrue((build / 'src/slic3r/render-profile/manifest.json').is_file())
            self.assertEqual(configure('OFF'), original)


def cpp_tests(sanitize):
    with tempfile.TemporaryDirectory(prefix='orca-render-profile-') as tmp:
        root = Path(tmp)
        for name, text in STUBS.items():
            p = root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
        executable = root / 'test'
        command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Wno-address', '-Werror',
                   '-pthread', '-I', str(root), '-I', str(TOOLS), str(root / 'test.cpp'), '-o', str(executable)]
        if sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=60)
        for mode in ('disabled', 'normal', 'unsupported', 'zero_bits', 'wrap', 'cpu_only'):
            env = {k: v for k, v in os.environ.items() if not k.startswith('ORCA_RENDER_PROFILE')}
            output = root / (mode + '.log')
            env.update(ORCA_RENDER_PROFILE='0' if mode == 'disabled' else '1', ORCA_RENDER_PROFILE_STRIDE='1',
                       ORCA_RENDER_PROFILE_OUT=str(output))
            if mode == 'cpu_only':
                env['ORCA_RENDER_PROFILE_CPU_ONLY'] = '1'
            subprocess.run([str(executable), mode], env=env, check=True, timeout=30)
            if mode == 'disabled':
                assert not output.exists()
                continue
            rows = [json.loads(line[len('[PCSS_PROFILE] '):]) for line in output.read_text().splitlines()]
            summaries = [r for r in rows if r['event'] == 'summary']
            assert summaries and sum(r['sampled_frames'] for r in summaries) == 23
            for row in summaries:
                assert row['seconds'] >= 0
                assert 'swap' not in row['gpu_ms']
                assert row['counters_per_sample']['shader.gouraud.triangles'] == 3000000
                assert row['counters_per_sample']['shader.selection_mask.triangles'] == 200
                assert row['counters_per_sample']['upload.objects_opaque.bytes'] == 1024
                assert row['shadow_requested'] == row['shadow_ready']
                if mode in ('unsupported', 'zero_bits', 'cpu_only'):
                    assert row['gpu_ms'] == {}
                for series in row['gpu_ms'].values():
                    assert 0 <= series['avg'] < 1.0  # The protocol clock is synthetic, not a performance measurement.
            if mode in ('normal', 'wrap'):
                assert any(r['counters_per_sample'].get('gpu_ring_full_skips', 0) > 0 for r in summaries)
                assert any(r['counters_per_sample'].get('gpu_marker_overflow', 0) > 0 for r in summaries)
            print('PASS JSON and attribution:', mode)
    print('C++ profiler protocol harness passed. Timings are synthetic; no hardware FPS measurement.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpp', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(GeneratorTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.cpp or args.sanitize:
        cpp_tests(args.sanitize)


if __name__ == '__main__':
    main()
