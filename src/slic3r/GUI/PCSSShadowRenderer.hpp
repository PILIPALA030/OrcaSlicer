#ifndef slic3r_PCSSShadowRenderer_hpp_
#define slic3r_PCSSShadowRenderer_hpp_

#include "PCSSShadowMath.hpp"

#include <functional>
#include <string>

namespace Slic3r { namespace GUI {

struct PCSSSettings
{
    unsigned resolution{2048};
    unsigned blocker_samples{16};
    unsigned filter_samples{32};
    float    angular_diameter_deg{4.0f}; // Full angular diameter, not angular radius.
    float    bias_mm{0.02f};
    float    max_radius_mm{12.0f};
    float    plate_strength{0.55f}; // Presentation alpha, not light transmission.
};

struct PCSSFrameInput
{
    pcss::Bounds  casters;
    pcss::Bounds  receivers;
    pcss::Vec3    to_light{0, 0, 1}; // World-space surface-to-light direction.
    std::uint64_t revision{0};       // Geometry, transforms, clipping and visible draw ranges.
};

// One owner per actual GL context. Shader programs and geometry are borrowed, never deleted here.
// All methods touching GL require that owner's context current on the UI rendering thread.
class PCSSShadowRenderer
{
    unsigned         m_framebuffer{0};
    unsigned         m_depth_texture{0};
    unsigned         m_vertex_array{0};
    unsigned         m_resolution{0};
    bool             m_ready{false};
    bool             m_failed{false};
    std::uint64_t    m_revision{0};
    std::uint64_t    m_depth_generation{0};
    PCSSSettings     m_settings;
    pcss::Projection m_projection;
    std::string      m_error;

    bool initialize_gl();
    friend class PCSSReceiverScope;
    void set_receiver_uniforms(unsigned program) const;

public:
    PCSSShadowRenderer()                                     = default;
    ~PCSSShadowRenderer()                                    = default; // Host must call shutdown_gl before destroying the context.
    PCSSShadowRenderer(const PCSSShadowRenderer&)            = delete;
    PCSSShadowRenderer& operator=(const PCSSShadowRenderer&) = delete;

    void                shutdown_gl();
    void                abandon_lost_context(); // Only after the old context was destroyed; does not issue GL calls.
    void                invalidate() { m_ready = false; }
    bool                set_settings(const PCSSSettings& settings);
    const PCSSSettings& settings() const { return m_settings; }
    const std::string&  error() const { return m_error; }
    bool                is_ready() const { return m_ready; }
    std::uint64_t       depth_generation() const { return m_depth_generation; }

    // Returns false on unsupported hardware, invalid input or allocation failure; caller draws the old path.
    // draw_depth only submits existing geometry under depth_program. It must not recurse into Canvas::render.
    bool update(const PCSSFrameInput& input, unsigned depth_program, const std::function<void()>& draw_depth);

    // Draw an existing plate polygon at its nominal print surface Z, after its logo and before UI icons.
    void render_plate(unsigned                     program,
                      const std::array<float, 16>& view_projection,
                      float                        surface_z,
                      const std::function<void()>& draw_polygon) const;
};

// Binds raw depth on a reserved texture unit without changing the caller's active texture unit.
// The caller starts its registered receiver shader first. Destruction disables its PCSS uniform and restores bindings.
class PCSSReceiverScope
{
    unsigned m_program{0};
    int      m_active_texture{0};
    int      m_previous_texture{0};
    int      m_previous_sampler{0};
    bool     m_has_samplers{false};

public:
    PCSSReceiverScope(const PCSSShadowRenderer* shadow, unsigned program);
    ~PCSSReceiverScope();
    PCSSReceiverScope(const PCSSReceiverScope&)            = delete;
    PCSSReceiverScope& operator=(const PCSSReceiverScope&) = delete;
};

}} // namespace Slic3r::GUI
#endif
