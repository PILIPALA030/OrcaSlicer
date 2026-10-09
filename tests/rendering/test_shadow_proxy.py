#!/usr/bin/env python3
"""Compile/run the actual ShadowMeshProxy header against controlled wx/GL/QEM stubs.

This tests ownership, handoff, cancellation, queuing and budget enforcement;
it does NOT test QEM approximation quality, the full GUI build, or GPU FPS.
Requires a C++17 compiler. Optional --sanitize enables ASan and UBSan.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FILES = {
    'GLModel.hpp': r'''
#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <algorithm>
inline const auto main_thread = std::this_thread::get_id();
inline std::atomic<int> jobs{0}, active_jobs{0}, peak_jobs{0}, bad_thread{0};
inline std::atomic<bool> hold_jobs{false};
inline std::atomic<int> outcome{0};
namespace Slic3r {
struct indexed_triangle_set {
    std::vector<std::array<unsigned, 3>> indices;
    std::vector<std::array<float, 3>> vertices;
    indexed_triangle_set() = default;
    indexed_triangle_set(const indexed_triangle_set& other) : indices(other.indices), vertices(other.vertices) {
        if (std::this_thread::get_id() == main_thread) ++bad_thread;
    }
};
struct TriangleMesh { indexed_triangle_set its; };
namespace GUI {
class GLModel {
public:
    struct Geometry {
        enum class EPrimitiveType { Triangles };
        enum class EVertexLayout { P3 };
        struct Format { EPrimitiveType type; EVertexLayout layout; } format;
        size_t vertices = 0, indices = 0;
        void reserve_vertices(size_t) {}
        void reserve_indices(size_t) {}
        void add_vertex(const std::array<float,3>&) { ++vertices; }
        void add_triangle(unsigned a, unsigned b, unsigned c) {
            assert(a < vertices && b < vertices && c < vertices);
            indices += 3;
        }
    };
    size_t count = 0;
    void init_from(Geometry&& geometry) {
        if (std::this_thread::get_id() != main_thread) ++bad_thread;
        count = geometry.indices;
    }
    ~GLModel() { if (std::this_thread::get_id() != main_thread) ++bad_thread; }
};
}}
''',
    'libslic3r/QuadricEdgeCollapse.hpp': r'''
#pragma once
#include "GLModel.hpp"
namespace Slic3r {
inline void its_quadric_edge_collapse(indexed_triangle_set& mesh, uint32_t budget,
                                     float*, std::function<void()> cancel) {
    if (std::this_thread::get_id() == main_thread) ++bad_thread;
    ++jobs;
    int now = ++active_jobs;
    peak_jobs.store(std::max(peak_jobs.load(), now));
    struct Guard { ~Guard() { --active_jobs; } } guard;
    while (hold_jobs.load()) {
        cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cancel();
    if (outcome == 1) throw std::runtime_error("injected QEM failure");
    mesh.indices.resize(outcome == 2 ? budget+1 : outcome == 3 ? 0 : budget);
}
}
''',
    'wx/timer.h': r'''
#pragma once
#include <vector>
#include <algorithm>
class wxTimer {
    bool running = false;
    static std::vector<wxTimer*>& timers() { static std::vector<wxTimer*> value; return value; }
public:
    wxTimer() { timers().push_back(this); }
    virtual ~wxTimer() { auto& t=timers(); t.erase(std::remove(t.begin(),t.end(),this),t.end()); }
    virtual void Notify() {}
    bool IsRunning() const { return running; }
    void Start(int) { running = true; }
    void Stop() { running = false; }
    static void Tick() { for (auto* t : timers()) if (t->running) t->Notify(); }
};
''',
    'boost/log/trivial.hpp': r'''
#pragma once
#include <iostream>
#define BOOST_LOG_TRIVIAL(level) std::cout
''',
    'test.cpp': r'''
#include "ShadowMeshProxy.hpp"
#include <iostream>
using Slic3r::GUI::ShadowMeshProxy;
using Slic3r::GUI::GLModel;
using Slic3r::TriangleMesh;
using namespace std::chrono_literals;
template<class F> void wait_until(F predicate) {
    for (int i=0; i<5000; ++i) {
        wxTimer::Tick();
        if (predicate()) return;
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("asynchronous test timed out");
}
int main() {
    auto mutable_source = std::make_shared<TriangleMesh>();
    mutable_source->its.vertices = {{{0,0,0}},{{1,0,0}},{{0,1,0}}};
    mutable_source->its.indices.resize(3000000, {0,1,2});
    std::shared_ptr<const TriangleMesh> source = mutable_source;
    bool enabled = true, lod_ready = false;
    int redraws = 0;
    auto redraw = [&] { assert(std::this_thread::get_id()==main_thread); ++redraws; };
    {
        ShadowMeshProxy proxy([&]{return enabled;}, [&]{return lod_ready;}, redraw);
        assert(proxy.get(source)==nullptr);
        wxTimer::Tick();
        assert(jobs==0); // Must wait for the already-running visible LOD jobs.
        lod_ready = true;
        wxTimer::Tick();
        assert(redraws>0);
        GLModel* model = nullptr;
        wait_until([&]{model=proxy.get(source); return model!=nullptr;});
        assert(model->count==3*ShadowMeshProxy::TRIANGLE_BUDGET);
        int count=jobs;
        assert(proxy.get(source)==model && jobs==count); // Cached CPU/GPU proxy.
        assert(source->its.indices.size()==3000000); // Editable source unchanged.
        std::cout << "\nPASS: LOD wait, 3000000 -> 100000 triangle budget, cache reuse and immutable source\n";
    }
    wait_until([]{return active_jobs==0;});
    {
        hold_jobs = true;
        ShadowMeshProxy first([&]{return enabled;}, []{return true;}, redraw);
        ShadowMeshProxy second([&]{return enabled;}, []{return true;}, redraw);
        first.get(source);
        wait_until([]{return active_jobs==1;});
        int count = jobs;
        for(int i=0;i<10;++i) { assert(second.get(source)==nullptr); wxTimer::Tick(); }
        assert(jobs==count && peak_jobs==1);
        hold_jobs = false;
        wait_until([&]{return first.get(source)!=nullptr;});
        wait_until([&]{return second.get(source)!=nullptr;});
        assert(peak_jobs==1);
        std::cout << "\nPASS: serialized workers and queued completion\n";
    }
    wait_until([]{return active_jobs==0;});
    {
        hold_jobs = true;
        ShadowMeshProxy proxy([&]{return enabled;}, []{return true;}, redraw);
        proxy.get(source);
        wait_until([]{return active_jobs==1;});
        enabled = false;
        wxTimer::Tick();
        wait_until([]{return active_jobs==0;});
        enabled = true;
        hold_jobs = false;
        wait_until([&]{return proxy.get(source)!=nullptr;});
        std::cout << "\nPASS: disable cancels work; re-enable restarts safely\n";
    }
    wait_until([]{return active_jobs==0;});
    {
        hold_jobs = true;
        auto proxy=std::make_unique<ShadowMeshProxy>([]{return true;},[]{return true;},redraw);
        proxy->get(source);
        wait_until([]{return active_jobs==1;});
        proxy.reset(); // Worker owns CPU state only; do not join on the UI thread.
        wait_until([]{return active_jobs==0;});
        hold_jobs = false;
        std::cout << "PASS: delete while worker is active\n";
    }
    for(int error=1;error<=3;++error) {
        outcome = error;
        ShadowMeshProxy proxy([]{return true;},[]{return true;},redraw);
        hold_jobs = true;
        int previous_jobs = jobs;
        wait_until([&]{proxy.get(source); return jobs > previous_jobs && active_jobs == 1;});
        int before=redraws;
        hold_jobs = false;
        wait_until([&]{return redraws>before && active_jobs==0;});
        int count=jobs;
        for(int i=0;i<10;++i) assert(proxy.get(source)==nullptr);
        assert(jobs==count); // No accidental retry storm or full-resolution fallback.
        std::cout << "\nPASS: terminal failure/budget rejection case " << error << "\n";
    }
    wait_until([]{return active_jobs==0;});
    // State publication precedes thread exit; give detached closures time to release CPU data.
    std::this_thread::sleep_for(50ms);
    assert(bad_thread==0);
    std::cout << "PASS: mesh copying/QEM stay off UI; GLModel stays on UI\n";
}
''',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcss-proxy-') as directory:
        root = Path(directory)
        for name, text in FILES.items():
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
        (root / 'ShadowMeshProxy.hpp').write_bytes((ROOT / 'src/slic3r/GUI/ShadowMeshProxy.hpp').read_bytes())
        command = [os.environ.get('CXX','c++'), '-std=c++17','-O1','-g','-pthread','-I',str(root)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        executable = root / 'test'
        subprocess.run(command+[str(root/'test.cpp'),'-o',str(executable)],check=True,timeout=60)
        subprocess.run([str(executable)],check=True,timeout=30)
    print('Proxy lifecycle harness passed (controlled QEM/wx/GL stubs; not a full application build).')


if __name__ == '__main__':
    main()
