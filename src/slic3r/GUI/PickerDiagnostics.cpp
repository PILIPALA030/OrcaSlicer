#include "PickerDiagnostics.hpp"
#include <algorithm>
#include <array>
#include <new>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#include <sys/resource.h>
#endif

namespace Slic3r { namespace GUI { namespace PickerDiag {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t MaxSlots = 128, MaxQueue = 8192, MaxGeometries = 16384;
constexpr uint64_t RotateBytes = 128ull * 1024 * 1024;
std::atomic<uint64_t> ids{1};
std::atomic<int> active_tasks{0}, peak_tasks{0};
thread_local Context tls_context;
thread_local Counters tls_counters;
thread_local size_t tls_slot = MaxSlots;
thread_local bool tls_registered = false;

bool env_flag(const char* name, bool fallback) noexcept {
    const char* value = std::getenv(name);
    return value ? std::strcmp(value,"0") != 0 : fallback;
}
struct Slot {
    std::atomic<uint64_t> version{0}, frame{0}, scope{0}, query{0}, geometry_uid{0}, since{0};
    std::atomic<const void*> canvas{nullptr}, volume{nullptr}, geometry{nullptr};
    std::atomic<const char*> pass{"other"}, stage{"idle"};
};
struct Cache { uint64_t camera=0, captured_camera=0, capture_frame=0, draw_frame=0, scene_invalidations=0, pick_invalidations=0; };
struct Geometry { uint64_t uid = 0, vertex_requested = 0, index_requested = 0; };
struct State {
    bool active = false, detail = false, gpu = false, errors = false;
    std::atomic<bool> accepting{false}, monitor_stop{false};
    std::atomic<size_t> slot_count{0};
    std::array<Slot, MaxSlots> slots;
    std::atomic<uint64_t> dropped{0};
    std::mutex queue_mutex, wait_mutex, geometry_mutex, cache_mutex;
    std::unordered_map<const void*,Cache> caches;
    std::condition_variable writer_cv, monitor_cv;
    std::deque<std::string> queue;
    std::unordered_map<const void*,Geometry> geometries;
    std::thread writer, monitor;
    std::filesystem::path path;
    FILE* file = nullptr;
    uint64_t file_bytes = 0;
    State() noexcept;
    void start();
    void stop() noexcept;
    void write(const std::string& line) noexcept;
    void pump() noexcept;
    void watch() noexcept;
};
State* exit_state = nullptr;
State& state() noexcept {
    // The small state object is process-lifetime: detached simplification workers
    // may still log during shutdown. stop() joins only our two owned threads.
    static State* s = []() noexcept {
        State* p = new (std::nothrow) State;
        if (!p) std::abort();
        exit_state = p;
        std::atexit([] { if (exit_state) exit_state->stop(); });
        try { p->start(); } catch (...) { p->stop(); p->active = false; }
        return p;
    }();
    return *s;
}
size_t slot_index() noexcept {
    if (!tls_registered) {
        tls_slot = state().slot_count.fetch_add(1, std::memory_order_relaxed);
        tls_registered = true;
    }
    return tls_slot;
}
uint64_t geometry_id(const void* ptr) noexcept {
    if (!ptr || !enabled()) return 0;
    try {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.geometry_mutex);
        auto found = s.geometries.find(ptr);
        if (found != s.geometries.end()) return found->second.uid;
        if (s.geometries.size() >= MaxGeometries) return 0;
        return s.geometries.emplace(ptr, Geometry{next_id(),0,0}).first->second.uid;
    } catch (...) { return 0; }
}
FILE* open_file(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}
State::State() noexcept {
    if (!env_flag("ORCA_PICKER_DIAG",true)) return;
    detail = env_flag("ORCA_PICKER_DIAG_DETAIL",false);
    gpu = env_flag("ORCA_PICKER_DIAG_GPU",false);
    errors = env_flag("ORCA_PICKER_DIAG_GL_ERRORS",false);
    try {
        std::error_code ec;
        auto dir = std::filesystem::temp_directory_path(ec);
#ifdef _WIN32
        if (const wchar_t* value = _wgetenv(L"ORCA_PICKER_DIAG_DIR")) dir = value;
        const auto pid = GetCurrentProcessId();
#else
        if (const char* value = std::getenv("ORCA_PICKER_DIAG_DIR")) dir = value;
        const auto pid = getpid();
#endif
        if (dir.empty()) return;
        ec.clear();
        std::filesystem::create_directories(dir,ec);
        if (ec) return;
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        path = dir / ("orca-picker-" + std::to_string(pid) + "-" + std::to_string(stamp) + ".log");
        file = open_file(path);
        if (!file) return;
        active = true;
        accepting = true;
        std::ostringstream line;
        line << "event=SESSION schema=1 base=8578d18ae1d09f2099e3de25a1ddf7d9e9e06120"
             << " unix_ms=" << stamp << " pid=" << pid
             << " detail=" << detail << " gpu_timing=" << gpu << " gl_error_probes=" << errors
             << " queue_capacity=" << MaxQueue << " watchdog_slots=" << MaxSlots
             << " logical_bytes_are_requested_not_residency=1";
#ifdef NDEBUG
        line << " ndebug=1";
#else
        line << " ndebug=0";
#endif
        line << " compiler=";
#ifdef _MSC_VER
        line << "msvc-" << _MSC_VER;
#else
        line << std::quoted(__VERSION__);
#endif
        line << '\n';
        write(line.str());
        std::fflush(file);
        std::fprintf(stderr,"Picker diagnostics: %s\n",path.u8string().c_str());
    } catch (...) { if (file) std::fclose(file); file = nullptr; active = false; accepting = false; }
}
void State::start() {
    if (!active) return;
    writer = std::thread([this] { pump(); });
    monitor = std::thread([this] { watch(); });
}
void State::stop() noexcept {
    monitor_stop = true;
    monitor_cv.notify_all();
    if (monitor.joinable()) monitor.join();
    accepting = false;
    writer_cv.notify_all();
    if (writer.joinable()) writer.join();
    if (file) { std::fflush(file); std::fclose(file); file = nullptr; }
}
void State::write(const std::string& line) noexcept {
    if (!file) return;
    if (file_bytes >= RotateBytes) {
        std::fclose(file); file = nullptr;
        try {
            auto previous = path; previous += ".1";
            std::error_code ec;
            std::filesystem::remove(previous,ec);
            ec.clear(); std::filesystem::rename(path,previous,ec);
            if (!ec) file = open_file(path);
        } catch (...) {}
        file_bytes = 0;
    }
    if (!file || std::fwrite(line.data(),1,line.size(),file) != line.size()) {
        accepting = false;
        return;
    }
    file_bytes += line.size();
}
void State::pump() noexcept {
    while (true) {
        std::deque<std::string> batch;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            writer_cv.wait_for(lock,std::chrono::milliseconds(250),[this] { return !accepting || !queue.empty(); });
            queue.swap(batch);
        }
        for (const auto& line : batch) write(line);
        const auto lost = dropped.exchange(0);
        if (lost) {
            try { write("event=LOG_DROPPED count=" + std::to_string(lost) + "\n"); } catch (...) {}
        }
        if (file) std::fflush(file);
        if (!accepting) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (queue.empty()) break;
        }
    }
}
void memory_sample() noexcept {
    try {
        std::ostringstream out;
#ifdef _WIN32
        MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
        const BOOL mem_ok = GlobalMemoryStatusEx(&memory);
        PROCESS_MEMORY_COUNTERS_EX process{}; process.cb = sizeof(process);
        const BOOL proc_ok = GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),sizeof(process));
        PERFORMANCE_INFORMATION perf{}; perf.cb = sizeof(perf);
        const BOOL perf_ok = GetPerformanceInfo(&perf,sizeof(perf));
        out << "platform=windows system_ok=" << !!mem_ok << " process_ok=" << !!proc_ok << " commit_ok=" << !!perf_ok;
        if (mem_ok) out << " physical_total=" << memory.ullTotalPhys << " physical_available=" << memory.ullAvailPhys;
        if (proc_ok) out << " working_set=" << process.WorkingSetSize << " working_set_peak=" << process.PeakWorkingSetSize
                         << " process_commit=" << process.PrivateUsage << " page_fault_count=" << process.PageFaultCount;
        if (perf_ok) out << " system_commit=" << uint64_t(perf.CommitTotal)*perf.PageSize
                         << " system_commit_limit=" << uint64_t(perf.CommitLimit)*perf.PageSize
                         << " system_commit_peak=" << uint64_t(perf.CommitPeak)*perf.PageSize;
#elif defined(__linux__)
        out << "platform=linux";
        std::ifstream memory("/proc/meminfo");
        std::string line, key;
        while (std::getline(memory,line)) {
            std::istringstream in(line); uint64_t kib = 0;
            if (!(in >> key >> kib)) continue;
            if (key == "MemTotal:" || key == "MemAvailable:" || key == "Committed_AS:" || key == "CommitLimit:")
                out << ' ' << key.substr(0,key.size()-1) << "_bytes=" << kib*1024;
        }
        std::ifstream status("/proc/self/status");
        while (std::getline(status,line)) {
            std::istringstream in(line); uint64_t kib = 0;
            if (!(in >> key >> kib)) continue;
            if (key == "VmRSS:" || key == "VmHWM:" || key == "VmSize:" || key == "VmSwap:")
                out << ' ' << key.substr(0,key.size()-1) << "_bytes=" << kib*1024;
        }
        rusage usage{};
        if (getrusage(RUSAGE_SELF,&usage) == 0) out << " minor_faults=" << usage.ru_minflt << " major_faults=" << usage.ru_majflt;
#else
        out << "platform=other memory_sample=unavailable";
#endif
        auto& s = state();
        {
            std::lock_guard<std::mutex> lock(s.geometry_mutex);
            uint64_t vertices = 0, indices = 0;
            for (const auto& pair : s.geometries) { vertices += pair.second.vertex_requested; indices += pair.second.index_requested; }
            out << " tracked_geometries=" << s.geometries.size() << " live_vertex_requested=" << vertices << " live_index_requested=" << indices;
        }
        out << " active_lod_tasks=" << active_tasks.load() << " peak_lod_tasks=" << peak_tasks.load() << " gpu_budget=not_sampled";
        emit("MEMORY",out.str());
    } catch (...) {}
}
void State::watch() noexcept {
    while (!monitor_stop) {
        try {
            const uint64_t now = now_us();
            for (size_t i=0; i<(std::min)(slot_count.load(),MaxSlots); ++i) {
                auto& s = slots[i];
                const auto version = s.version.load(std::memory_order_acquire);
                if (version & 1) continue;
                const auto since = s.since.load();
                const auto stage = s.stage.load();
                const auto frame = s.frame.load(), scope = s.scope.load(), query = s.query.load();
                const auto volume = s.volume.load(), geometry = s.geometry.load(), canvas = s.canvas.load();
                const auto uid = s.geometry_uid.load();
                const auto pass = s.pass.load();
                if (version != s.version.load(std::memory_order_acquire)) continue;
                if (!since || now < since || now-since < 2000000) continue;
                std::ostringstream out;
                out << "watched_tid=" << i << " watched_frame=" << frame << " watched_scope=" << scope
                    << " watched_query=" << query << " watched_stage=" << stage << " age_ms=" << (now-since)/1000
                    << " watched_pass=" << pass << " watched_canvas=" << canvas << " watched_volume=" << volume
                    << " watched_geometry=" << geometry << " watched_geometry_uid=" << uid;
#ifdef _WIN32
                out << " debugger=" << !!IsDebuggerPresent();
#endif
                emit("STALL",out.str());
            }
            memory_sample();
        } catch (...) {}
        std::unique_lock<std::mutex> lock(wait_mutex);
        monitor_cv.wait_for(lock,std::chrono::seconds(1),[this] { return monitor_stop.load(); });
    }
}
} // namespace

bool enabled() noexcept { return state().active; }
bool detailed() noexcept { return enabled() && state().detail; }
bool gpu_timing() noexcept { return enabled() && state().gpu; }
bool probe_errors() noexcept { return enabled() && state().errors; }
uint64_t now_us() noexcept { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count()); }
uint64_t next_id() noexcept { return ids.fetch_add(1,std::memory_order_relaxed); }
Context& context() noexcept { return tls_context; }
void shutdown() noexcept { state().stop(); }
void task_delta(int delta) noexcept {
    if (!enabled()) return;
    const int count = active_tasks.fetch_add(delta)+delta;
    int peak = peak_tasks.load();
    while (count > peak && !peak_tasks.compare_exchange_weak(peak,count)) {}
    PD_EVENT("LOD_TASK_COUNT","active=" << count << " peak=" << peak_tasks.load());
}
void camera_state(const void* canvas, const double* view, const double* projection, const int* viewport) noexcept {
    if (!enabled()) return;
    uint64_t hash = 14695981039346656037ull;
    const auto add = [&hash](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for(size_t i=0;i<size;++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    };
    add(view,16*sizeof(double)); add(projection,16*sizeof(double)); add(viewport,4*sizeof(int));
    try {
        auto& s=state(); std::lock_guard<std::mutex> lock(s.cache_mutex);
        if(s.caches.size()<128 || s.caches.count(canvas)) s.caches[canvas].camera=hash;
    } catch (...) {}
    PD_DETAIL("CAMERA","fingerprint=" << hash << " width=" << viewport[2] << " height=" << viewport[3]);
}
void cache_note(const void* canvas, const char* action) noexcept {
    if (!enabled()) return;
    try {
        Cache copy;
        auto& s=state();
        {
            std::lock_guard<std::mutex> lock(s.cache_mutex);
            if(s.caches.size()>=128 && !s.caches.count(canvas)) return;
            auto& cache=s.caches[canvas];
            if(std::strcmp(action,"invalidate_scene")==0) ++cache.scene_invalidations;
            if(std::strcmp(action,"invalidate_picking")==0) ++cache.pick_invalidations;
            if(std::strcmp(action,"main_redraw")==0) cache.draw_frame=context().frame;
            if(std::strcmp(action,"capture_success")==0) { cache.captured_camera=cache.camera; cache.capture_frame=context().frame; }
            copy=cache;
            if(std::strcmp(action,"destroy")==0) s.caches.erase(canvas);
        }
        PD_EVENT("CACHE_STATE","action=" << action << " cache_canvas=" << canvas << " camera=" << copy.camera
            << " captured_camera=" << copy.captured_camera << " capture_frame=" << copy.capture_frame << " draw_frame=" << copy.draw_frame
            << " camera_matches=" << (copy.capture_frame && copy.camera==copy.captured_camera)
            << " scene_invalidations=" << copy.scene_invalidations << " pick_invalidations=" << copy.pick_invalidations);
    } catch (...) {}
}
void publish() noexcept {
    if (!enabled()) return;
    const auto index = slot_index();
    if (index >= MaxSlots) return;
    auto& s = state().slots[index];
    s.version.fetch_add(1,std::memory_order_acq_rel);
    s.frame = tls_context.frame; s.scope = tls_context.scope; s.query = tls_context.query;
    s.geometry_uid = tls_context.geometry_uid;
    s.canvas = tls_context.canvas; s.volume = tls_context.volume; s.geometry = tls_context.geometry;
    s.pass = tls_context.pass; s.stage = tls_context.stage; s.since = tls_context.stage_start_us;
    s.version.fetch_add(1,std::memory_order_release);
}
void emit(const char* event, const std::string& fields) noexcept {
    auto& s = state();
    if (!s.accepting) return;
    try {
        const auto tid = slot_index();
        const auto& c = tls_context;
        std::ostringstream out;
        out << "seq=" << next_id() << " t_us=" << now_us() << " tid=" << tid
            << " frame=" << c.frame << " scope=" << c.scope << " query=" << c.query
            << " canvas=" << c.canvas << " pass=" << c.pass << " volume=" << c.volume
            << " geometry=" << c.geometry << " geometry_uid=" << c.geometry_uid
            << " event=" << event;
        if (!fields.empty()) out << ' ' << fields;
        std::string line = out.str();
        for (char& ch : line) if (ch == '\n' || ch == '\r') ch = ' ';
        if (line.size()>2048) line.resize(2048);
        line += '\n';
        std::unique_lock<std::mutex> lock(s.queue_mutex);
        if (s.queue.size() >= MaxQueue) { ++s.dropped; return; }
        s.queue.emplace_back(std::move(line));
        lock.unlock(); s.writer_cv.notify_one();
    } catch (...) { ++s.dropped; }
}
void note_draw(uint64_t triangles) noexcept {
    if (!enabled()) return;
    ++tls_counters.draws; tls_counters.triangles += triangles;
    if (std::strcmp(context().pass,"picking") == 0) tls_counters.picking_triangles += triangles;
}
void note_read(uint64_t bytes) noexcept {
    if (!enabled()) return;
    ++tls_counters.read_calls; tls_counters.read_requested_bytes += bytes;
}
void note_upload(uint32_t target, uint64_t bytes) noexcept {
    if (!enabled()) return;
    ++tls_counters.upload_calls; tls_counters.upload_requested_bytes += bytes;
    try {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.geometry_mutex);
        auto it = s.geometries.find(context().geometry);
        if (it != s.geometries.end()) {
            if (target == 0x8892) it->second.vertex_requested = bytes; // GL_ARRAY_BUFFER
            if (target == 0x8893) it->second.index_requested = bytes; // GL_ELEMENT_ARRAY_BUFFER
        }
    } catch (...) {}
}
void note_lod(const char* level) noexcept {
    if (!enabled()) return;
    if (std::strcmp(level,"High")==0) ++tls_counters.high_visits;
    if (std::strcmp(level,"Middle")==0) ++tls_counters.middle_visits;
    if (std::strcmp(level,"Small")==0) ++tls_counters.small_visits;
}
void retire_geometry(const void* geometry) noexcept {
    if (!enabled()) return;
    try { auto& s = state(); std::lock_guard<std::mutex> lock(s.geometry_mutex); s.geometries.erase(geometry); } catch (...) {}
}
Scope::Scope(const char* name, bool verbose) noexcept : name_(name) {
    if (!enabled()) return;
    active_ = true; log_ = !verbose || detailed();
    previous_ = context(); start_ = now_us(); exceptions_ = std::uncaught_exceptions();
    context().scope = next_id(); context().stage = name; context().stage_start_us = start_;
    publish();
    if (log_) PD_EVENT("BEGIN","stage=" << name_ << " parent_scope=" << previous_.scope);
}
Scope::~Scope() noexcept {
    if (!active_) return;
    const auto elapsed = now_us()-start_;
    if (log_ || elapsed >= 16000) PD_EVENT("END","stage=" << name_ << " cpu_wall_us=" << elapsed
        << " result=" << (std::uncaught_exceptions()>exceptions_ ? "exception" : result_) << " begin_logged=" << log_);
    context() = previous_; publish();
}
Tag::Tag(const char* pass) noexcept : kind_(Kind::Pass) {
    if (!enabled()) return;
    active_ = true; previous_ = context(); context().pass = pass; publish();
}
Tag::Tag(Kind kind, const void* value) noexcept : kind_(kind) {
    if (!enabled()) return;
    active_ = true; previous_ = context();
    if (kind == Kind::Volume) context().volume = value;
    if (kind == Kind::Geometry) { context().geometry = value; context().geometry_uid = geometry_id(value); }
    if (kind == Kind::Query) context().query = next_id();
    publish();
}
Tag::~Tag() noexcept {
    if (!active_) return;
    if (kind_ == Kind::Pass) context().pass = previous_.pass;
    if (kind_ == Kind::Volume) context().volume = previous_.volume;
    if (kind_ == Kind::Geometry) { context().geometry = previous_.geometry; context().geometry_uid = previous_.geometry_uid; }
    if (kind_ == Kind::Query) context().query = previous_.query;
    publish();
}
Frame::Frame(const void* canvas, bool only_init, bool overlay_only) noexcept {
    if (!enabled()) return;
    active_ = true; previous_ = context(); initial_ = tls_counters; start_ = now_us();
    context().frame = next_id(); context().canvas = canvas;
    context().stage = "render"; context().stage_start_us = start_; publish();
    PD_EVENT("RENDER_ATTEMPT","only_init=" << only_init << " overlay_only=" << overlay_only << " parent_frame=" << previous_.frame);
}
void Frame::accept() noexcept { accepted_ = true; if (active_) emit("FRAME_ACCEPT"); }
Frame::~Frame() noexcept {
    if (!active_) return;
    PD_EVENT(accepted_ ? "FRAME_END" : "FRAME_ABORT","cpu_wall_us=" << now_us()-start_ << " swap_returned=" << swapped_
        << " draws=" << tls_counters.draws-initial_.draws << " triangles=" << tls_counters.triangles-initial_.triangles
        << " picking_triangles=" << tls_counters.picking_triangles-initial_.picking_triangles
        << " high_visits=" << tls_counters.high_visits-initial_.high_visits
        << " middle_visits=" << tls_counters.middle_visits-initial_.middle_visits
        << " small_visits=" << tls_counters.small_visits-initial_.small_visits
        << " upload_calls=" << tls_counters.upload_calls-initial_.upload_calls
        << " upload_requested_bytes=" << tls_counters.upload_requested_bytes-initial_.upload_requested_bytes
        << " read_calls=" << tls_counters.read_calls-initial_.read_calls
        << " read_requested_bytes=" << tls_counters.read_requested_bytes-initial_.read_requested_bytes);
    context() = previous_; publish();
}
}}}
