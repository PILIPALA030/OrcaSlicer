#ifndef ORCA_RENDER_PROFILE_HPP
#define ORCA_RENDER_PROFILE_HPP

// Opt-in diagnostics, not a renderer dependency in ordinary builds.
// GPU timestamps describe command-stream intervals, not exclusive shader ALU time.
#include <GL/glew.h>
#include "libslic3r/Utils.hpp"
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef ORCA_PROFILE_REVISION
#define ORCA_PROFILE_REVISION "unknown"
#endif
#ifndef ORCA_PROFILE_SOURCE_DIGEST
#define ORCA_PROFILE_SOURCE_DIGEST "unknown"
#endif

namespace Slic3r { namespace GUI { namespace RenderProfile {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
inline double milliseconds(Time a, Time b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
inline std::string environment(const char* name)
{
    const char* value = boost::nowide::getenv(name);
    return value ? value : "";
}
struct Options {
    bool enabled = environment("ORCA_RENDER_PROFILE") == "1";
    bool cpu_only = environment("ORCA_RENDER_PROFILE_CPU_ONLY") == "1";
    std::string mode = environment("ORCA_RENDER_PROFILE_MODE");
    std::string label = environment("ORCA_RENDER_PROFILE_LABEL").substr(0, 128);
    unsigned stride = 4;
    Options()
    {
        if (!enabled || (mode != "static_off" && mode != "depth_only"))
            mode = "normal";
        const auto text = environment("ORCA_RENDER_PROFILE_STRIDE");
        if (!text.empty()) {
            char* end = nullptr;
            const long n = std::strtol(text.c_str(), &end, 10);
            if (end && *end == '\0' && n >= 1 && n <= 120)
                stride = static_cast<unsigned>(n);
        }
    }
};
inline const Options& options()
{
    // Workers can finish during static teardown. Keep this tiny configuration
    // alive until process exit rather than race destruction of its strings.
    static const Options* const value = new Options;
    return *value;
}
inline bool enabled() { return options().enabled; }
inline bool static_off() { return options().mode == "static_off"; }
inline bool depth_only() { return options().mode == "depth_only"; }
inline Time entry_time() { return enabled() ? Clock::now() : Time{}; }

inline std::string json_string(const std::string& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}
struct Sink {
    std::mutex mutex;
    boost::nowide::ofstream file;
    std::string path;
    Sink()
    {
        path = environment("ORCA_RENDER_PROFILE_OUT");
        if (path.empty()) path = Slic3r::data_dir() + "/pcss-profile.log";
        file.open(path, std::ios::out | std::ios::app);
        BOOST_LOG_TRIVIAL(info) << "PCSS_PROFILE output: " << path;
        if (!file) BOOST_LOG_TRIVIAL(error) << "PCSS_PROFILE cannot open output; using stderr: " << path;
    }
    void write(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (file) { file << "[PCSS_PROFILE] " << line << '\n'; file.flush(); }
        else std::cerr << "[PCSS_PROFILE] " << line << '\n';
    }
};
inline Sink& sink()
{
    // Process-lifetime sink: detached CPU-only proxy workers may still log at
    // shutdown. Every write flushes; OS teardown closes the descriptor.
    static Sink* const value = new Sink;
    return *value;
}
inline void write(const std::string& line) { sink().write(line); }
inline Time session_start() { static const auto value = Clock::now(); return value; }

constexpr unsigned MAX_SPANS = 128;
constexpr unsigned MAX_QUERIES = 128;
constexpr unsigned RING_SIZE = 12;
struct Metadata {
    int canvas = 0, width = 0, height = 0;
    bool requested = false, ready = false, selected = false, picking = false;
    size_t volumes = 0;
    std::string key() const
    {
        std::ostringstream s;
        s << canvas << ':' << width << ':' << height << ':' << requested << ':' << ready
          << ':' << selected << ':' << picking << ':' << volumes;
        return s.str();
    }
};
struct Span {
    const char* name = "";
    int begin = -1, end = -1;
    double cpu = 0.0;
};
struct Record {
    Metadata meta;
    unsigned nspans = 0;
    std::array<Span, MAX_SPANS> spans{};
    std::map<std::string, uint64_t> counters;
    double cadence = -1.0;
};
struct Slot {
    std::array<GLuint, MAX_QUERIES> queries{};
    unsigned nqueries = 0;
    bool pending = false;
    uint64_t serial = 0;
    Time issued_at{};
    Record record;
};
struct Distribution {
    std::vector<double> samples;
    void add(double value) { samples.push_back(value); }
    std::string json() const
    {
        if (samples.empty()) return "null";
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (double n : sorted) sum += n;
        const auto percentile = [&](double p) { return sorted[static_cast<size_t>(std::ceil(p * sorted.size())) - 1]; };
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << std::fixed << std::setprecision(4) << "{\"avg\":" << sum / sorted.size()
          << ",\"p50\":" << percentile(0.5) << ",\"p95\":" << percentile(0.95)
          << ",\"n\":" << sorted.size() << '}';
        return s.str();
    }
};
struct Batch {
    Metadata meta;
    size_t samples = 0;
    std::map<std::string, Distribution> cpu, gpu;
    Distribution cadence;
    std::map<std::string, uint64_t> counters;
};
class Profiler {
public:
    std::array<Slot, RING_SIZE> slots;
    uint64_t serial = 0;
    Time last_present{}, last_report = Clock::now();
    bool gpu = false;
    GLint bits = 0;
    void* owner;
    std::map<std::string, Batch> batches;
    size_t batched = 0;

    explicit Profiler(void* canvas) : owner(canvas)
    {
        (void)session_start();
        gpu = !options().cpu_only && (GLEW_VERSION_3_3 || GLEW_ARB_timer_query) &&
              glQueryCounter && glGetQueryObjectui64v && glGetQueryObjectiv && glGenQueries;
        if (gpu) {
            glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
            gpu = bits >= 30 && bits <= 64;
        }
        const auto gl_text = [](GLenum e) {
            const auto* p = glGetString(e);
            return p ? reinterpret_cast<const char*>(p) : "unavailable";
        };
        GLint samples = 0;
        glGetIntegerv(GL_SAMPLES, &samples);
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << "{\"event\":\"info\",\"schema\":1,\"canvas_id\":" << json_string(id())
          << ",\"revision\":" << json_string(ORCA_PROFILE_REVISION)
          << ",\"source_digest\":" << json_string(ORCA_PROFILE_SOURCE_DIGEST)
#ifdef NDEBUG
          << ",\"build\":\"NDEBUG\""
#else
          << ",\"build\":\"DEBUG_GL_CHECKS\""
#endif
          << ",\"vendor\":" << json_string(gl_text(GL_VENDOR))
          << ",\"renderer\":" << json_string(gl_text(GL_RENDERER))
          << ",\"gl_version\":" << json_string(gl_text(GL_VERSION))
          << ",\"gpu_timestamps\":" << (gpu ? "true" : "false") << ",\"timestamp_bits\":" << bits
          << ",\"msaa_samples\":" << samples << ",\"stride\":" << options().stride
          << ",\"mode\":" << json_string(options().mode) << ",\"label\":" << json_string(options().label)
          << ",\"resources\":" << json_string(Slic3r::resources_dir()) << '}';
        write(s.str());
    }
    std::string id() const { std::ostringstream s; s << owner; return s.str(); }
    Slot* acquire()
    {
        if (!gpu) return nullptr;
        for (auto& slot : slots) {
            if (slot.pending) continue;
            if (slot.queries[0] == 0) glGenQueries(MAX_QUERIES, slot.queries.data());
            if (slot.queries[0] == 0) { gpu = false; return nullptr; }
            slot.record = Record{};
            slot.nqueries = 0;
            slot.serial = serial;
            slot.issued_at = Clock::now();
            return &slot;
        }
        return nullptr; // Never reuse an unresolved query and never wait for a free slot.
    }
    void consume(const Record& record, const std::vector<GLuint64>* times = nullptr)
    {
        auto& batch = batches[record.meta.key()];
        batch.meta = record.meta;
        ++batch.samples;
        ++batched;
        std::map<std::string, double> cpu, gpu_times;
        for (unsigned i = 0; i < record.nspans; ++i) {
            const auto& span = record.spans[i];
            cpu[span.name] += span.cpu;
            if (times && span.begin >= 0 && span.end >= 0) {
                GLuint64 elapsed = (*times)[span.end] - (*times)[span.begin];
                if (bits < 64) elapsed &= (GLuint64(1) << bits) - 1;
                // A long CPU interval can hide multiple wraps on short counters.
                const double wrap_ms = std::ldexp(1.0, bits) * 1e-6;
                if (bits < 64 && span.cpu >= wrap_ms) {
                    ++batch.counters["gpu_counter_wrap_ambiguous"];
                } else if (elapsed < GLuint64(100000000000ULL)) {
                    gpu_times[span.name] += double(elapsed) * 1e-6;
                } else ++batch.counters["gpu_invalid_interval"];
            }
        }
        for (const auto& pair : cpu) batch.cpu[pair.first].add(pair.second);
        for (const auto& pair : gpu_times) batch.gpu[pair.first].add(pair.second);
        for (const auto& pair : record.counters) batch.counters[pair.first] += pair.second;
        if (record.cadence >= 0.0 && record.cadence <= 250.0) batch.cadence.add(record.cadence);
        else ++batch.counters["cadence_idle_or_first"];
    }
    void poll(bool shutdown = false)
    {
        // This method is called only with this canvas's GL context current.
        for (auto& slot : slots) {
            if (!slot.pending || (!shutdown && serial < slot.serial + 2)) continue;
            GLint available = GL_FALSE;
            glGetQueryObjectiv(slot.queries[slot.nqueries - 1], GL_QUERY_RESULT_AVAILABLE, &available);
            if (available == GL_FALSE) continue;
            // TIMESTAMP queries of this type issued before the last one are ready too.
            std::vector<GLuint64> times(slot.nqueries);
            for (unsigned i = 0; i < slot.nqueries; ++i)
                glGetQueryObjectui64v(slot.queries[i], GL_QUERY_RESULT, &times[i]);
            // Do not infer a modulo interval after one or more complete
            // counter periods could have elapsed while the sample was pending.
            if (bits < 64 && milliseconds(slot.issued_at, Clock::now()) >= std::ldexp(1.0, bits) * 1e-6) {
                ++slot.record.counters["gpu_counter_wrap_ambiguous"];
                consume(slot.record);
            } else consume(slot.record, &times);
            slot.pending = false;
        }
    }
    void report(bool force = false)
    {
        if (!force && batched < 240 && milliseconds(last_report, Clock::now()) < 3000.0) return;
        for (const auto& pair : batches) {
            const auto& b = pair.second;
            if (!b.samples) continue;
            std::ostringstream s;
            s.imbue(std::locale::classic());
            s << std::fixed << std::setprecision(4)
              << "{\"event\":\"summary\",\"canvas_id\":" << json_string(id())
              << ",\"seconds\":" << milliseconds(session_start(), Clock::now()) / 1000.0
              << ",\"mode\":" << json_string(options().mode) << ",\"label\":" << json_string(options().label)
              << ",\"canvas_type\":" << b.meta.canvas << ",\"shadow_requested\":" << b.meta.requested
              << ",\"shadow_ready\":" << b.meta.ready << ",\"selected\":" << b.meta.selected
              << ",\"picking\":" << b.meta.picking << ",\"width\":" << b.meta.width
              << ",\"height\":" << b.meta.height << ",\"volumes\":" << b.meta.volumes
              << ",\"sampled_frames\":" << b.samples << ",\"cadence_ms\":" << b.cadence.json();
            const auto metrics = [&s](const char* name, const std::map<std::string, Distribution>& values) {
                s << ',' << json_string(name) << ":{";
                bool first = true;
                for (const auto& value : values) {
                    if (!first) s << ',';
                    first = false;
                    s << json_string(value.first) << ':' << value.second.json();
                }
                s << '}';
            };
            metrics("cpu_ms", b.cpu);
            metrics("gpu_ms", b.gpu);
            s << ",\"counters_per_sample\":{";
            bool first = true;
            for (const auto& counter : b.counters) {
                if (!first) s << ',';
                first = false;
                s << json_string(counter.first) << ':' << double(counter.second) / b.samples;
            }
            s << "}}";
            write(s.str());
        }
        batches.clear();
        batched = 0;
        last_report = Clock::now();
    }
    void release(bool context_current)
    {
        if (context_current) poll(true);
        for (auto& slot : slots) {
            if (slot.pending) {
                ++slot.record.counters["gpu_unresolved_at_close"];
                consume(slot.record); // Keep CPU evidence even when GPU evidence is unavailable.
                slot.pending = false;
            }
            if (context_current && slot.queries[0]) glDeleteQueries(MAX_QUERIES, slot.queries.data());
            slot.queries.fill(0);
        }
        report(true);
    }
};
inline std::map<void*, std::unique_ptr<Profiler>>& registry()
{
    // No GL work in static destructors. Explicit canvas teardown releases queries.
    static std::map<void*, std::unique_ptr<Profiler>> values;
    return values;
}
template<class MakeCurrent> void release(void* canvas, MakeCurrent make_current)
{
    if (!enabled()) return;
    auto& values = registry();
    auto found = values.find(canvas);
    if (found == values.end()) return;
    found->second->release(make_current());
    values.erase(found);
}
class Frame;
inline Frame*& current() { static thread_local Frame* value = nullptr; return value; }
class Frame {
    Profiler* m_profiler = nullptr;
    Slot* m_slot = nullptr;
    std::optional<Record> m_fallback;
    Record* m_record = nullptr;
    Frame* m_previous = nullptr;
    Time m_start{};
    bool m_finished = false;
public:
    const char* draw_stage = "frame_other";
    Frame(void* canvas, const Metadata& meta, Time entry)
    {
        if (!enabled()) return;
        auto& value = registry()[canvas];
        if (!value) value = std::make_unique<Profiler>(canvas);
        m_profiler = value.get();
        ++m_profiler->serial;
        const auto overhead = Clock::now();
        m_profiler->poll();
        m_profiler->report();
        const auto start = Clock::now();
        m_start = entry;
        m_previous = current();
        current() = nullptr;
        if ((m_profiler->serial - 1) % options().stride != 0) return;
        m_slot = m_profiler->acquire();
        if (m_slot) m_record = &m_slot->record;
        else {
            m_fallback.emplace();
            m_record = &*m_fallback;
            if (m_profiler->gpu) ++m_record->counters["gpu_ring_full_skips"];
        }
        m_record->meta = meta;
        m_record->nspans = 3;
        m_record->spans[0].name = "frame";
        m_record->spans[0].begin = stamp();
        m_record->spans[1] = Span{"prelude", -1, -1, milliseconds(entry, overhead)};
        m_record->spans[2] = Span{"profiler_poll_report", -1, -1, milliseconds(overhead, start)};
        current() = this;
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    ~Frame() { finish(); }
    int stamp()
    {
        if (!m_slot) return -1;
        if (m_slot->nqueries == MAX_QUERIES) {
            ++m_record->counters["gpu_marker_overflow"];
            return -1;
        }
        const unsigned index = m_slot->nqueries++;
        glQueryCounter(m_slot->queries[index], GL_TIMESTAMP);
        return static_cast<int>(index);
    }
    int begin(const char* name, bool gpu)
    {
        if (!m_record) return -1;
        if (m_record->nspans == MAX_SPANS) {
            ++m_record->counters["cpu_span_overflow"];
            return -1;
        }
        const unsigned index = m_record->nspans++;
        m_record->spans[index] = Span{name, gpu ? stamp() : -1, -1, 0.0};
        return static_cast<int>(index);
    }
    void end(int index, Time start)
    {
        if (index < 0) return;
        auto& span = m_record->spans[index];
        if (span.begin >= 0) span.end = stamp();
        span.cpu = milliseconds(start, Clock::now());
    }
    void count(const std::string& name, uint64_t value = 1)
    {
        if (m_record) m_record->counters[name] += value;
    }
    void ready(bool value) { if (m_record) m_record->meta.ready = value; }
    void before_swap()
    {
        if (m_record && m_record->spans[0].end < 0) m_record->spans[0].end = stamp();
    }
    void finish()
    {
        if (m_finished || !m_profiler) return;
        m_finished = true;
        before_swap();
        const auto end = Clock::now();
        if (m_record) {
            m_record->spans[0].cpu = milliseconds(m_start, end);
            if (m_profiler->last_present != Time{}) m_record->cadence = milliseconds(m_profiler->last_present, end);
            if (m_slot && m_slot->nqueries) m_slot->pending = true;
            else m_profiler->consume(*m_record);
        }
        m_profiler->last_present = end;
        current() = m_previous;
    }
};
class Scope {
    Frame* m_frame = current();
    const char* m_previous = nullptr;
    Time m_start{};
    int m_index = -1;
public:
    explicit Scope(const char* name, bool gpu = true)
    {
        if (!m_frame) return;
        m_previous = m_frame->draw_stage;
        // CPU-only subscopes keep geometry/upload attribution with their GPU pass.
        if (gpu) m_frame->draw_stage = name;
        m_start = Clock::now();
        m_index = m_frame->begin(name, gpu);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() { stop(); }
    void stop()
    {
        if (!m_frame) return;
        m_frame->end(m_index, m_start);
        m_frame->draw_stage = m_previous;
        m_frame = nullptr;
    }
};
inline void count(const std::string& name, uint64_t value = 1)
{
    if (auto* f = current()) f->count(name, value);
}
inline void draw(GLenum mode, size_t indices, size_t instances, const std::string& shader)
{
    auto* f = current();
    if (!f) return;
    uint64_t triangles = 0;
    if (mode == GL_TRIANGLES) triangles = indices / 3;
    else if ((mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) && indices > 2) triangles = indices - 2;
    triangles *= instances;
    const std::string stage = std::string("draw.") + f->draw_stage;
    f->count(stage + ".calls");
    f->count(stage + ".triangles", triangles);
    f->count("shader." + shader + ".calls");
    f->count("shader." + shader + ".triangles", triangles);
}
inline void upload(size_t bytes)
{
    if (auto* f = current()) {
        f->count(std::string("upload.") + f->draw_stage + ".bytes", bytes);
        f->count(std::string("upload.") + f->draw_stage + ".calls");
    }
}
// CPU-only background-job evidence. Never touches GL or captures a Frame/canvas.
class Job {
    Time m_begin{}, m_copy{}, m_qem{};
    size_t m_input = 0, m_output = 0;
public:
    explicit Job(size_t input) : m_input(input) { if (enabled()) m_begin = Clock::now(); }
    void copied() { if (enabled()) m_copy = Clock::now(); }
    void simplified() { if (enabled()) m_qem = Clock::now(); }
    void result(size_t count) { m_output = count; }
    ~Job()
    {
        if (!enabled()) return;
        try {
            std::ostringstream s;
            s.imbue(std::locale::classic());
            s << std::fixed << std::setprecision(4) << "{\"event\":\"proxy_job\",\"input_triangles\":" << m_input
              << ",\"output_triangles\":" << m_output << ",\"cpu_wall_ms\":" << milliseconds(m_begin, Clock::now())
              << ",\"copy_ms\":" << (m_copy == Time{} ? -1.0 : milliseconds(m_begin, m_copy))
              << ",\"qem_ms\":" << (m_qem == Time{} || m_copy == Time{} ? -1.0 : milliseconds(m_copy, m_qem)) << '}';
            write(s.str());
        } catch (...) { /* Diagnostics must not terminate a worker during unwinding. */ }
    }
};
}}} // namespace Slic3r::GUI::RenderProfile
#endif
