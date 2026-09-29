// Diagnostic-only instrumentation. No OpenGL or GUI access in the log writer.
#pragma once
#include <cstdint>
#include <sstream>
#include <type_traits>
#include <utility>

namespace Slic3r { namespace GUI { namespace PickerDiag {
struct Context {
    uint64_t frame = 0, scope = 0, query = 0, geometry_uid = 0;
    const void* canvas = nullptr;
    const void* volume = nullptr;
    const void* geometry = nullptr;
    const char* pass = "other";
    const char* stage = "idle";
    uint64_t stage_start_us = 0;
};
struct Counters {
    uint64_t draws = 0, triangles = 0, picking_triangles = 0;
    uint64_t high_visits = 0, middle_visits = 0, small_visits = 0;
    uint64_t upload_calls = 0, upload_requested_bytes = 0;
    uint64_t read_calls = 0, read_requested_bytes = 0;
};
bool enabled() noexcept;
bool detailed() noexcept;
bool gpu_timing() noexcept;
bool probe_errors() noexcept;
uint64_t now_us() noexcept;
uint64_t next_id() noexcept;
Context& context() noexcept;
void emit(const char* event, const std::string& fields = {}) noexcept;
void publish() noexcept;
void note_draw(uint64_t triangles) noexcept;
void note_read(uint64_t bytes) noexcept;
void note_upload(uint32_t target, uint64_t bytes) noexcept;
void note_lod(const char* level) noexcept;
void retire_geometry(const void* geometry) noexcept;
void shutdown() noexcept;
void task_delta(int delta) noexcept;
void camera_state(const void* canvas, const double* view, const double* projection, const int* viewport) noexcept;
void cache_note(const void* canvas, const char* action) noexcept;

class Scope {
    Context previous_;
    uint64_t start_ = 0;
    const char* name_;
    const char* result_ = "return";
    int exceptions_ = 0;
    bool active_ = false, log_ = false;
public:
    explicit Scope(const char* name, bool verbose = false) noexcept;
    ~Scope() noexcept;
    void result(const char* value) noexcept { result_ = value; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
class Tag {
public:
    enum class Kind { Pass, Volume, Geometry, Query };
private:
    Context previous_;
    Kind kind_;
    bool active_ = false;
public:
    explicit Tag(const char* pass) noexcept;
    Tag(Kind kind, const void* value) noexcept;
    ~Tag() noexcept;
    Tag(const Tag&) = delete;
    Tag& operator=(const Tag&) = delete;
};
class Frame {
    Context previous_;
    Counters initial_;
    uint64_t start_ = 0;
    bool active_ = false, accepted_ = false, swapped_ = false;
public:
    Frame(const void* canvas, bool only_init, bool overlay_only) noexcept;
    ~Frame() noexcept;
    void accept() noexcept;
    void swapped() noexcept { swapped_ = true; }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

template<class F> decltype(auto) call(const char* name, F&& fn) {
    Scope scope(name);
    if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
        std::forward<F>(fn)();
    } else {
        decltype(auto) value = std::forward<F>(fn)();
        if constexpr (std::is_same_v<std::remove_cv_t<std::remove_reference_t<decltype(value)>>, bool>)
            scope.result(value ? "true" : "false");
        return value;
    }
}
}}}

#define PD_JOIN_I(a,b) a##b
#define PD_JOIN(a,b) PD_JOIN_I(a,b)
#define PD_SCOPE(name) ::Slic3r::GUI::PickerDiag::Scope PD_JOIN(_pd_scope_,__LINE__)(name)
#define PD_VERBOSE_SCOPE(name) ::Slic3r::GUI::PickerDiag::Scope PD_JOIN(_pd_scope_,__LINE__)(name,true)
#define PD_PASS(name) ::Slic3r::GUI::PickerDiag::Tag PD_JOIN(_pd_pass_,__LINE__)(name)
#define PD_TAG(kind,value) ::Slic3r::GUI::PickerDiag::Tag PD_JOIN(_pd_tag_,__LINE__)(::Slic3r::GUI::PickerDiag::Tag::Kind::kind,value)
#define PD_CALL(name,expr) ::Slic3r::GUI::PickerDiag::call(name,[&]() -> decltype(auto) { return (expr); })
#define PD_EVENT(name,fields) do { if (::Slic3r::GUI::PickerDiag::enabled()) { try { std::ostringstream _pd_stream; _pd_stream << fields; ::Slic3r::GUI::PickerDiag::emit(name,_pd_stream.str()); } catch (...) {} } } while(false)
#define PD_DETAIL(name,fields) do { if (::Slic3r::GUI::PickerDiag::detailed()) { PD_EVENT(name,fields); } } while(false)
