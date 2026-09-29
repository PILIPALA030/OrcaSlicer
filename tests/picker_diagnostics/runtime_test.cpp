#include "PickerDiagnosticsGL.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace PD = Slic3r::GUI::PickerDiag;
static void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
int main(int argc,char**) {
    const bool expected_enabled=argc==1;
    require(PD::enabled()==expected_enabled,"enable flag");
    int token=0, executed=0;
    {
        PD::Frame frame(&token,false,false); frame.accept();
        PD_PASS("picking");
        PD_TAG(Geometry,&token);
        PD_SCOPE("test_outer");
        const auto before=PD::context();
        {
            PD_SCOPE("test_inner");
            int& reference=PD_CALL("reference",(++executed,token));
            reference=7;
            require(token==7 && executed==1,"single evaluation and reference semantics");
            auto owner=PD_CALL("move_only",std::make_unique<int>(9));
            require(*owner==9,"move-only return");
            try { PD_CALL("exception",throw std::runtime_error("expected")); }
            catch(const std::runtime_error&) { ++executed; }
        }
        require(PD::context().scope==before.scope,"scope restoration");
        require(executed==2,"exception propagation");
        PD::on_context(&token);
        PD::buffer_data(GL_ARRAY_BUFFER,252000000,nullptr,GL_STATIC_DRAW);
        unsigned char pixel[4]{};
        PD::read_pixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        PD::draw_elements(GL_TRIANGLES,9000000,GL_UNSIGNED_INT,nullptr);
        require(MockGL::buffers==1 && MockGL::reads==1 && MockGL::draws==1,"GL call preservation");
        MockGL::error=GL_OUT_OF_MEMORY;
        require(PD::observed_error("test")==GL_OUT_OF_MEMORY,"preserve consumed error");
        require(glGetError()==GL_NO_ERROR,"consume exactly once");
        if(expected_enabled) {
            const auto uid=PD::context().geometry_uid;
            require(uid!=0,"geometry uid");
            PD::retire_geometry(&token);
            { PD_TAG(Geometry,&token); require(PD::context().geometry_uid!=uid,"resource generation"); }
            { PD_GPU_SCOPE("mock_gpu"); }
            PD::on_context(&token);
            require(MockGL::result_reads==0,"pending result must not be fetched");
            MockGL::available=true; PD::on_context(&token);
            require(MockGL::result_reads==2,"resolve completed timestamp pair");
            MockGL::available=false;
            for(int i=0;i<70;++i) { PD_GPU_SCOPE("pool_fill"); }
            require(MockGL::times.size()==128,"bounded timestamp query pool");
            double view[16]{},projection[16]{}; int viewport[4]{0,0,1920,1080};
            PD::camera_state(&token,view,projection,viewport); PD::cache_note(&token,"capture_success");
            view[0]=1; PD::camera_state(&token,view,projection,viewport); PD::cache_note(&token,"present_attempt");
            std::vector<std::thread> workers;
            for(int i=0;i<8;++i) workers.emplace_back([] { PD_SCOPE("test_worker"); PD::task_delta(1); PD_EVENT("WORKER_TEST","value=1"); PD::task_delta(-1); });
            for(auto& worker:workers) worker.join();
            { PD_SCOPE("deliberately_slow_mock_readback"); std::this_thread::sleep_for(std::chrono::milliseconds(3200)); }
            PD::release_context(&token);
        } else require(MockGL::string_queries==0,"disabled context snapshot must not query GL");
        frame.swapped();
    }
    require(PD::context().frame==0 && PD::context().stage_start_us==0,"frame cleanup must restore idle");
    PD::shutdown();
    std::cout << "picker diagnostics tests passed (enabled=" << expected_enabled << ")\n";
    return 0;
}
