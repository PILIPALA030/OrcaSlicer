#ifndef slic3r_PCSSShadowAcceleration_hpp_
#define slic3r_PCSSShadowAcceleration_hpp_

// Private implementation, included only by PCSSShadowRenderer.cpp. No host geometry or shader ownership changes.
#include <GL/glew.h>
#include "PCSSShadowPolicy.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace Slic3r { namespace GUI {

class PCSSShadowAcceleration
{
public:
    static constexpr unsigned TEXTURE_UNIT = 6;
    enum class Stage : unsigned { Depth, Bounds, Model, GCode, Plate, Count };

private:
    using Clock = std::chrono::steady_clock;
    struct Ticket
    {
        GLuint            queries[2]{};
        GLuint            primitives{0};
        bool              owns_primitives{false};
        Stage             stage{Stage::Depth};
        Clock::time_point start;
        unsigned          state{0}; // 0 free, 1 active, 2 pending. Pending query IDs are never reused.
        std::uint64_t     frame{0};
    };
    struct Statistics
    {
        double   cpu_ms{0}, gpu_ms{0};
        unsigned cpu_count{0}, gpu_count{0};
    };
    GLuint                 m_texture{0}, m_framebuffer{0}, m_program{0};
    unsigned               m_resolution{0}, m_levels{0};
    std::uint64_t          m_generation{0};
    bool                   m_valid{false}, m_failed{false};
    bool                   m_disable_bounds{false}, m_hard{false}, m_plate_only{false}, m_no_receivers{false};
    bool                   m_reference_model{false}, m_profile_geometry{false};
    bool                   m_profile{false}, m_timer_checked{false}, m_timer_supported{false};
    std::array<Ticket, 32> m_tickets{};
    std::array<Statistics, static_cast<unsigned>(Stage::Count)> m_statistics{};
    std::uint64_t                                               m_frame{0}, m_interval_frames{0};
    unsigned                                                    m_dropped{0}, m_accelerated_calls{0};
    Clock::time_point                                           m_report_time{Clock::now()};
    unsigned                                                    m_map_size{0}, m_blocker_count{0}, m_filter_count{0};
    unsigned                                                    m_model_blockers{0}, m_model_filters{0};
    bool                                                        m_bounds_requested{false}, m_vertex_clip{false};
    std::array<unsigned, 6>                                     m_update_reasons{};
    std::uint64_t                                               m_depth_primitives{0};
    unsigned                                                    m_primitive_samples{0};
    unsigned                                                    m_model_budget_calls{0};
    std::ofstream                                               m_log;

    static bool option_is(const char* name, const char* value) { return pcss::environment_is(name, value); }

    void message(const std::string& text)
    {
        if (m_log.is_open()) {
            m_log << text << '\n';
            m_log.flush();
        } else
            std::clog << text << std::endl;
    }

    void report()
    {
        if (!m_profile || m_interval_frames == 0)
            return;
        static constexpr const char* NAMES[]{"depth", "bounds", "model", "gcode", "plate"};
        std::ostringstream           out;
        out << std::fixed << std::setprecision(3) << "[PCSS] revision=receiver-budget-v3 instance=" << this
            << " frames=" << m_interval_frames << " map=" << m_map_size << " samples=" << m_blocker_count << "/" << m_filter_count
            << " model_samples=" << m_model_blockers << "/" << m_model_filters << " filter=" << (m_hard ? "hard" : "pcss")
            << " receivers=" << (m_no_receivers ? "none" : (m_plate_only ? "plate" : "all"))
            << " bounds=" << (m_bounds_requested && bounds_ready() ? "on" : "off")
            << " depth_clip=" << (m_vertex_clip ? "vertex" : "fragment_or_custom");
        for (unsigned i = 0; i < m_statistics.size(); ++i) {
            const Statistics& s = m_statistics[i];
            out << " | " << NAMES[i] << " calls=" << s.cpu_count << " cpu_ms=" << (s.cpu_count ? s.cpu_ms / s.cpu_count : 0.0);
            if (s.gpu_count)
                out << " gpu_ms=" << s.gpu_ms / s.gpu_count << " gpu_samples=" << s.gpu_count;
            else
                out << " gpu_ms=NA gpu_samples=0";
        }
        out << " bounds_receiver_calls=" << m_accelerated_calls << " dropped=" << m_dropped << " | cache_hits=" << m_update_reasons[0]
            << " redraw_invalid=" << m_update_reasons[1] << " redraw_revision=" << m_update_reasons[2]
            << " redraw_light=" << m_update_reasons[3] << " redraw_projection=" << m_update_reasons[4]
            << " redraw_program=" << m_update_reasons[5];
        if (m_primitive_samples)
            out << " depth_primitives=" << static_cast<double>(m_depth_primitives) / m_primitive_samples;
        else
            out << " depth_primitives=NA";
        out << " primitive_samples=" << m_primitive_samples << " model_budget_calls=" << m_model_budget_calls;
        message(out.str());
        m_model_budget_calls = 0;
        m_statistics         = {};
        m_update_reasons     = {};
        m_depth_primitives   = 0;
        m_primitive_samples  = 0;
        m_interval_frames    = 0;
        m_dropped            = 0;
        m_accelerated_calls  = 0;
        m_report_time        = Clock::now();
    }

    bool create_program()
    {
        if (m_program != 0)
            return true;
        // Attribute-free reduction triangle; this program does not draw host GLModel geometry.
        static const char* VERTEX   = R"(#version 140
void main() {
    gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);
})";
        static const char* FRAGMENT = R"(#version 140
uniform sampler2D source_texture;
uniform bool source_is_depth;
uniform float source_footprint;
out vec4 depth_plane;
void main() {
    ivec2 dst = ivec2(gl_FragCoord.xy);
    vec3 plane;
    float error = 0.0;
    if (source_is_depth) {
        ivec2 size = textureSize(source_texture, 0);
        float samples[16];
        plane = vec3(0.0);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                ivec2 p = dst * 4 + ivec2(x, y);
                float d = all(lessThan(p, size)) ? texelFetch(source_texture, p, 0).r : 1.0;
                samples[y * 4 + x] = d;
                plane += d * vec3(1.0 / 16.0, (float(x) - 1.5) / 20.0, (float(y) - 1.5) / 20.0);
            }
        }
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                error = max(error, abs(samples[y * 4 + x] - plane.x - dot(plane.yz, vec2(x, y) - 1.5)));
    } else {
        vec4 child[4];
        child[0] = texelFetch(source_texture, dst * 2, 0);
        child[1] = texelFetch(source_texture, dst * 2 + ivec2(1, 0), 0);
        child[2] = texelFetch(source_texture, dst * 2 + ivec2(0, 1), 0);
        child[3] = texelFetch(source_texture, dst * 2 + ivec2(1, 1), 0);
        plane.x = (child[0].x + child[1].x + child[2].x + child[3].x) * 0.25;
        plane.y = (child[1].x + child[3].x - child[0].x - child[2].x) / (2.0 * source_footprint);
        plane.z = (child[2].x + child[3].x - child[0].x - child[1].x) / (2.0 * source_footprint);
        for (int i = 0; i < 4; ++i) {
            vec2 offset = (vec2(i % 2, i / 2) - 0.5) * source_footprint;
            float center_error = abs(child[i].x - plane.x - dot(plane.yz, offset));
            float slope_error = dot(abs(child[i].yz - plane.yz), vec2((source_footprint - 1.0) * 0.5));
            error = max(error, child[i].w + center_error + slope_error);
        }
    }
    depth_plane = vec4(plane, error + 4e-7);
})";
        GLuint             shaders[2]{glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER)};
        const char*        sources[]{VERTEX, FRAGMENT};
        bool               ok = shaders[0] != 0 && shaders[1] != 0;
        for (unsigned i = 0; ok && i < 2; ++i) {
            glShaderSource(shaders[i], 1, &sources[i], nullptr);
            glCompileShader(shaders[i]);
            GLint compiled = GL_FALSE;
            glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
            ok = compiled == GL_TRUE;
            if (!ok) {
                char log[2048]{};
                glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
                message(std::string("[PCSS] Optional bounds shader disabled: ") + log);
            }
        }
        if (ok) {
            m_program = glCreateProgram();
            if (m_program != 0) {
                for (GLuint shader : shaders)
                    glAttachShader(m_program, shader);
                glLinkProgram(m_program);
                GLint linked = GL_FALSE;
                glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
                ok = linked == GL_TRUE;
                if (!ok) {
                    char log[2048]{};
                    glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
                    message(std::string("[PCSS] Optional bounds program disabled: ") + log);
                }
            } else
                ok = false;
        }
        for (GLuint shader : shaders)
            if (shader != 0)
                glDeleteShader(shader);
        if (!ok && m_program != 0) {
            glDeleteProgram(m_program);
            m_program = 0;
        }
        return ok;
    }

public:
    PCSSShadowAcceleration()
        : m_disable_bounds(option_is("ORCA_PCSS_BOUNDS", "0"))
        , m_hard(option_is("ORCA_PCSS_FILTER", "hard"))
        , m_plate_only(option_is("ORCA_PCSS_RECEIVERS", "plate"))
        , m_no_receivers(option_is("ORCA_PCSS_RECEIVERS", "none"))
        , m_reference_model(option_is("ORCA_PCSS_MODEL_QUALITY", "reference"))
        , m_profile_geometry(option_is("ORCA_PCSS_PROFILE_GEOMETRY", "1"))
        , m_profile(option_is("ORCA_PCSS_PROFILE", "1"))
    {
        if (m_profile) {
            if (const char* path = std::getenv("ORCA_PCSS_PROFILE_LOG")) {
                m_log.open(path, std::ios::out | std::ios::app);
                if (!m_log.is_open())
                    std::clog << "[PCSS] Cannot open requested profile log; writing to stderr instead.\n";
            }
            message("[PCSS] Profiling enabled: CPU submission and asynchronous GPU timings are per pass, not application FPS.");
        }
    }
    ~PCSSShadowAcceleration() { report(); } // No GL calls here: the context may already be lost.
    PCSSShadowAcceleration(const PCSSShadowAcceleration&)            = delete;
    PCSSShadowAcceleration& operator=(const PCSSShadowAcceleration&) = delete;

    bool     reference_model_quality() const { return m_reference_model; }
    bool     hard_shadows() const { return m_hard; }
    bool     receives(bool plate) const { return !m_no_receivers && (!m_plate_only || plate); }
    bool     bounds_ready() const { return m_valid && !m_disable_bounds && !m_hard; }
    unsigned texture() const { return m_texture; }
    unsigned levels() const { return m_levels; }

    void record_model_budget(bool applied)
    {
        if (m_profile && applied)
            ++m_model_budget_calls;
    }

    void record_update(unsigned reason, bool vertex_clip)
    {
        if (!m_profile)
            return;
        m_vertex_clip = vertex_clip;
        if (reason == pcss::CACHE_HIT)
            ++m_update_reasons[0];
        for (unsigned i = 0; i < 5; ++i)
            if ((reason & (1u << i)) != 0)
                ++m_update_reasons[i + 1];
    }

    bool needs_bounds(unsigned resolution, std::uint64_t generation) const
    {
        return !m_disable_bounds && !m_hard && !m_no_receivers && !m_failed &&
               (!m_valid || resolution != m_resolution || generation != m_generation);
    }

    // Caller holds ShadowGLState and its private VAO. Only texture unit 6 is additionally touched here.
    void update_bounds(unsigned depth_texture, unsigned resolution, std::uint64_t generation)
    {
        if (!needs_bounds(resolution, generation))
            return;
        m_valid      = false;
        GLint active = 0, texture = 0, sampler = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0 + TEXTURE_UNIT);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        const bool has_samplers = GLEW_VERSION_3_3 || GLEW_ARB_sampler_objects;
        if (has_samplers) {
            glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
            glBindSampler(TEXTURE_UNIT, 0);
        }
        const auto restore_texture = [&]() {
            glBindTexture(GL_TEXTURE_2D, texture);
            if (has_samplers)
                glBindSampler(TEXTURE_UNIT, sampler);
            glActiveTexture(active);
        };
        if (!create_program()) {
            m_failed = true;
            restore_texture();
            return;
        }
        unsigned side = 1;
        m_levels      = 0;
        while (side < (resolution + 3) / 4) {
            side *= 2;
            ++m_levels;
        }
        if (m_texture == 0 || m_resolution != resolution) {
            if (m_texture != 0)
                glDeleteTextures(1, &m_texture);
            glGenTextures(1, &m_texture);
            glBindTexture(GL_TEXTURE_2D, m_texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, m_levels);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            for (unsigned level = 0; level <= m_levels; ++level)
                glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA32F, side >> level, side >> level, 0, GL_RGBA, GL_FLOAT, nullptr);
            m_resolution = resolution;
        }
        if (m_framebuffer == 0)
            glGenFramebuffers(1, &m_framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_NONE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(m_program);
        glUniform1i(glGetUniformLocation(m_program, "source_texture"), TEXTURE_UNIT);
        const GLint footprint = glGetUniformLocation(m_program, "source_footprint");
        const GLint is_depth  = glGetUniformLocation(m_program, "source_is_depth");
        for (unsigned level = 0; level <= m_levels; ++level) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, level);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                m_failed = true;
                message("[PCSS] Optional depth bounds FBO unavailable; falling back to full PCSS sampling.");
                break;
            }
            glViewport(0, 0, side >> level, side >> level);
            glBindTexture(GL_TEXTURE_2D, level == 0 ? depth_texture : m_texture);
            if (level != 0) {
                // Exclude the attached output mip from the accessible sampling range. Explicit LOD alone
                // is not enough to avoid a GL 3.1 framebuffer feedback loop.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, level - 1);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level - 1);
            }
            glUniform1i(is_depth, level == 0);
            glUniform1f(footprint, level == 0 ? 1.0f : static_cast<float>(4u << (level - 1)));
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, m_levels);
        restore_texture();
        m_generation = generation;
        m_valid      = !m_failed;
    }

    void frame(unsigned resolution,
               unsigned blockers,
               unsigned filters,
               unsigned model_blockers   = 0,
               unsigned model_filters    = 0,
               bool     bounds_requested = true)
    {
        if (!m_profile)
            return;
        m_map_size         = resolution;
        m_blocker_count    = blockers;
        m_filter_count     = filters;
        m_model_blockers   = model_blockers;
        m_model_filters    = model_filters;
        m_bounds_requested = bounds_requested;
        ++m_frame;
        ++m_interval_frames;
        for (Ticket& ticket : m_tickets) {
            if (ticket.state != 2 || m_frame - ticket.frame < 2)
                continue;
            GLint ready = GL_FALSE;
            glGetQueryObjectiv(ticket.queries[1], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (ready != GL_TRUE)
                continue;
            if (ticket.owns_primitives) {
                glGetQueryObjectiv(ticket.primitives, GL_QUERY_RESULT_AVAILABLE, &ready);
                if (ready != GL_TRUE)
                    continue;
            }
            GLuint64 start = 0, end = 0;
            glGetQueryObjectui64v(ticket.queries[0], GL_QUERY_RESULT, &start);
            glGetQueryObjectui64v(ticket.queries[1], GL_QUERY_RESULT, &end);
            if (end >= start) {
                Statistics& s = m_statistics[static_cast<unsigned>(ticket.stage)];
                s.gpu_ms += static_cast<double>(end - start) * 1e-6;
                ++s.gpu_count;
            }
            if (ticket.owns_primitives) {
                GLuint64 count = 0;
                glGetQueryObjectui64v(ticket.primitives, GL_QUERY_RESULT, &count);
                m_depth_primitives += count;
                ++m_primitive_samples;
            }
            ticket.state           = 0;
            ticket.owns_primitives = false;
        }
        if (Clock::now() - m_report_time >= std::chrono::seconds(2))
            report();
    }

    int begin(Stage stage)
    {
        if (!m_profile)
            return -1;
        if (!m_timer_checked) {
            m_timer_checked   = true;
            m_timer_supported = GLEW_VERSION_3_3 || GLEW_ARB_timer_query;
            if (m_timer_supported) {
                GLint bits = 0;
                glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
                m_timer_supported = bits > 0;
            }
            message(std::string("[PCSS] GPU timer=") + (m_timer_supported ? "available; GL=" : "unavailable; GL=") +
                    reinterpret_cast<const char*>(glGetString(GL_VERSION)) +
                    "; renderer=" + reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        }
        for (unsigned i = 0; i < m_tickets.size(); ++i) {
            Ticket& ticket = m_tickets[i];
            if (ticket.state != 0)
                continue;
            ticket.stage           = stage;
            ticket.state           = 1;
            ticket.frame           = m_frame;
            ticket.start           = Clock::now();
            ticket.owns_primitives = false;
            if (m_timer_supported) {
                if (ticket.queries[0] == 0)
                    glGenQueries(2, ticket.queries);
                glQueryCounter(ticket.queries[0], GL_TIMESTAMP);
                if (m_profile_geometry && stage == Stage::Depth) {
                    GLint current = 0;
                    glGetQueryiv(GL_PRIMITIVES_GENERATED, GL_CURRENT_QUERY, &current);
                    if (current == 0) { // Never interrupt a query owned by the host or another scope.
                        if (ticket.primitives == 0)
                            glGenQueries(1, &ticket.primitives);
                        glBeginQuery(GL_PRIMITIVES_GENERATED, ticket.primitives);
                        ticket.owns_primitives = true;
                    }
                }
            }
            return static_cast<int>(i);
        }
        ++m_dropped;
        return -1;
    }

    int begin_receiver(unsigned program, bool plate, bool use_bounds)
    {
        if (!m_profile)
            return -1;
        if (use_bounds && bounds_ready() && receives(plate) && glGetUniformLocation(program, "pcss_depth_ranges") >= 0)
            ++m_accelerated_calls;
        return begin(plate ? Stage::Plate : (glGetUniformLocation(program, "emission_factor") >= 0 ? Stage::GCode : Stage::Model));
    }

    void end(int index)
    {
        if (index < 0)
            return;
        Ticket&     ticket = m_tickets[static_cast<unsigned>(index)];
        Statistics& s      = m_statistics[static_cast<unsigned>(ticket.stage)];
        if (ticket.owns_primitives)
            glEndQuery(GL_PRIMITIVES_GENERATED);
        if (m_timer_supported)
            glQueryCounter(ticket.queries[1], GL_TIMESTAMP);
        s.cpu_ms += std::chrono::duration<double, std::milli>(Clock::now() - ticket.start).count();
        ++s.cpu_count;
        ticket.state = m_timer_supported ? 2 : 0;
    }

    void shutdown_gl()
    {
        report();
        if (m_texture != 0)
            glDeleteTextures(1, &m_texture);
        if (m_framebuffer != 0)
            glDeleteFramebuffers(1, &m_framebuffer);
        if (m_program != 0)
            glDeleteProgram(m_program);
        for (Ticket& ticket : m_tickets) {
            if (ticket.queries[0] != 0)
                glDeleteQueries(2, ticket.queries);
            if (ticket.primitives != 0)
                glDeleteQueries(1, &ticket.primitives);
        }
        m_texture = m_framebuffer = m_program = 0;
        m_tickets                             = {};
        m_valid                               = false;
    }
};

}} // namespace Slic3r::GUI
#endif
