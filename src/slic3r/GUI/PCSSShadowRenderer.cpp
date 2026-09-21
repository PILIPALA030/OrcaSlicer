#include "PCSSShadowRenderer.hpp"

#include <GL/glew.h>

namespace Slic3r { namespace GUI {
namespace {

constexpr unsigned SHADOW_TEXTURE_UNIT = 7;

void set_enabled(GLenum capability, GLboolean enabled)
{
    if (enabled)
        glEnable(capability);
    else
        glDisable(capability);
}

// The private VAO is essential: restoring a VAO name cannot undo edits made to that same VAO's attributes.
class ShadowGLState
{
    GLint                       m_draw_fbo{}, m_read_fbo{}, m_program{}, m_vao{}, m_array_buffer{}, m_element_buffer{}, m_unpack_buffer{};
    GLint                       m_viewport[4]{}, m_polygon_mode[2]{}, m_depth_func{}, m_front_face{}, m_cull_face{};
    GLint                       m_blend_src_rgb{}, m_blend_dst_rgb{}, m_blend_src_alpha{}, m_blend_dst_alpha{};
    GLint                       m_blend_eq_rgb{}, m_blend_eq_alpha{}, m_active_texture{}, m_texture{};
    GLboolean                   m_color_mask[4]{}, m_depth_mask{};
    GLdouble                    m_depth_range[2]{}, m_clear_depth{};
    const std::array<GLenum, 9> m_capabilities{GL_BLEND,        GL_DEPTH_TEST,          GL_CULL_FACE,          GL_SCISSOR_TEST,
                                               GL_STENCIL_TEST, GL_POLYGON_OFFSET_FILL, GL_RASTERIZER_DISCARD, GL_SAMPLE_ALPHA_TO_COVERAGE,
                                               GL_DEPTH_CLAMP};
    std::array<GLboolean, 9>    m_enabled{};

public:
    ShadowGLState()
    {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_draw_fbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_read_fbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_array_buffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &m_unpack_buffer);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &m_element_buffer);
        glGetIntegerv(GL_VIEWPORT, m_viewport);
        glGetIntegerv(GL_POLYGON_MODE, m_polygon_mode);
        glGetIntegerv(GL_DEPTH_FUNC, &m_depth_func);
        glGetIntegerv(GL_FRONT_FACE, &m_front_face);
        glGetIntegerv(GL_CULL_FACE_MODE, &m_cull_face);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depth_mask);
        glGetBooleanv(GL_COLOR_WRITEMASK, m_color_mask);
        glGetDoublev(GL_DEPTH_RANGE, m_depth_range);
        glGetDoublev(GL_DEPTH_CLEAR_VALUE, &m_clear_depth);
        glGetIntegerv(GL_BLEND_SRC_RGB, &m_blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &m_blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_blend_src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &m_blend_dst_alpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_blend_eq_rgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_blend_eq_alpha);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &m_active_texture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_texture);
        for (unsigned i = 0; i < m_capabilities.size(); ++i) {
            if (m_capabilities[i] == GL_DEPTH_CLAMP && !(GLEW_VERSION_3_2 || GLEW_ARB_depth_clamp))
                continue;
            m_enabled[i] = glIsEnabled(m_capabilities[i]);
        }
    }
    ~ShadowGLState()
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_draw_fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read_fbo);
        glUseProgram(m_program);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_array_buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_unpack_buffer);
        if (m_vao != 0)
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_element_buffer);
        glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
        if (m_polygon_mode[0] == m_polygon_mode[1])
            glPolygonMode(GL_FRONT_AND_BACK, m_polygon_mode[0]);
        else { // Separate face modes are only possible in compatibility contexts.
            glPolygonMode(GL_FRONT, m_polygon_mode[0]);
            glPolygonMode(GL_BACK, m_polygon_mode[1]);
        }
        glDepthFunc(m_depth_func);
        glDepthMask(m_depth_mask);
        glDepthRange(m_depth_range[0], m_depth_range[1]);
        glClearDepth(m_clear_depth);
        glColorMask(m_color_mask[0], m_color_mask[1], m_color_mask[2], m_color_mask[3]);
        glFrontFace(m_front_face);
        glCullFace(m_cull_face);
        glBlendFuncSeparate(m_blend_src_rgb, m_blend_dst_rgb, m_blend_src_alpha, m_blend_dst_alpha);
        glBlendEquationSeparate(m_blend_eq_rgb, m_blend_eq_alpha);
        glActiveTexture(m_active_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        for (unsigned i = 0; i < m_capabilities.size(); ++i) {
            if (m_capabilities[i] == GL_DEPTH_CLAMP && !(GLEW_VERSION_3_2 || GLEW_ARB_depth_clamp))
                continue;
            set_enabled(m_capabilities[i], m_enabled[i]);
        }
    }
};

void uniform_int(unsigned program, const char* name, int value) { glUniform1i(glGetUniformLocation(program, name), value); }
void uniform_float(unsigned program, const char* name, float value) { glUniform1f(glGetUniformLocation(program, name), value); }

} // namespace

bool PCSSShadowRenderer::set_settings(const PCSSSettings& settings)
{
    if (settings.resolution < 256 || settings.resolution > 8192 || settings.blocker_samples < 1 || settings.blocker_samples > 64 ||
        settings.filter_samples < 1 || settings.filter_samples > 64 || !std::isfinite(settings.angular_diameter_deg) ||
        settings.angular_diameter_deg < 0.0f || settings.angular_diameter_deg > 20.0f || !std::isfinite(settings.bias_mm) ||
        settings.bias_mm < 0.0f || !std::isfinite(settings.max_radius_mm) || settings.max_radius_mm <= 0.0f ||
        !std::isfinite(settings.plate_strength) || settings.plate_strength < 0.0f || settings.plate_strength > 1.0f)
        return false;
    if (settings.resolution != m_settings.resolution || settings.max_radius_mm != m_settings.max_radius_mm)
        invalidate();
    m_settings = settings;
    return true;
}

bool PCSSShadowRenderer::initialize_gl()
{
    if (m_failed)
        return false;
    GLint max_size = 0, units = 0;
    if (!(GLEW_VERSION_3_1 && (GLEW_VERSION_3_0 || GLEW_ARB_framebuffer_object))) {
        m_error  = "PCSS requires OpenGL 3.1 and framebuffer objects";
        m_failed = true;
        return false;
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
    const unsigned resolution = std::min(m_settings.resolution, static_cast<unsigned>(std::max(0, max_size)));
    if (resolution < 256 || units <= static_cast<int>(SHADOW_TEXTURE_UNIT)) {
        m_error  = "PCSS depth texture or texture-unit limit is insufficient";
        m_failed = true;
        return false;
    }
    if (m_framebuffer != 0 && m_resolution == resolution)
        return true;

    ShadowGLState state;
    GLuint        texture = 0, framebuffer = 0, vao = 0;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); // nullptr means no upload, never an offset into a caller PBO.
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, resolution, resolution, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glGenVertexArrays(1, &vao);
    if (texture == 0 || framebuffer == 0 || vao == 0 || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteTextures(1, &texture);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteVertexArrays(1, &vao);
        m_error  = "PCSS depth framebuffer allocation failed";
        m_failed = true;
        m_ready  = false;
        return false;
    }
    // Allocate successfully before replacing the old map. No live receiver scope is allowed during update.
    if (m_depth_texture != 0)
        glDeleteTextures(1, &m_depth_texture);
    if (m_framebuffer != 0)
        glDeleteFramebuffers(1, &m_framebuffer);
    if (m_vertex_array != 0)
        glDeleteVertexArrays(1, &m_vertex_array);
    m_depth_texture = texture;
    m_framebuffer   = framebuffer;
    m_vertex_array  = vao;
    m_resolution    = resolution;
    m_ready         = false;
    return true;
}

void PCSSShadowRenderer::shutdown_gl()
{
    if (m_depth_texture != 0)
        glDeleteTextures(1, &m_depth_texture);
    if (m_framebuffer != 0)
        glDeleteFramebuffers(1, &m_framebuffer);
    if (m_vertex_array != 0)
        glDeleteVertexArrays(1, &m_vertex_array);
    abandon_lost_context();
}

void PCSSShadowRenderer::abandon_lost_context()
{
    m_depth_texture = m_framebuffer = m_vertex_array = m_resolution = 0;
    m_ready = m_failed = false;
    m_error.clear();
}

bool PCSSShadowRenderer::update(const PCSSFrameInput& input, unsigned depth_program, const std::function<void()>& draw_depth)
{
    pcss::Projection next;
    if (depth_program == 0 || !draw_depth ||
        !pcss::fit_projection(input.casters, input.receivers, input.to_light, m_settings.max_radius_mm, m_settings.resolution, next)) {
        invalidate();
        return false;
    }
    if (!initialize_gl()) {
        invalidate();
        return false;
    }
    if (m_resolution != m_settings.resolution &&
        !pcss::fit_projection(input.casters, input.receivers, input.to_light, m_settings.max_radius_mm, m_resolution, next)) {
        invalidate();
        return false;
    }
    if (m_ready && input.revision == m_revision && next.matrix == m_projection.matrix &&
        next.min_caster_depth == m_projection.min_caster_depth)
        return true;

    ShadowGLState state;
    m_ready = false; // Also invalid if the host draw callback throws.
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glBindVertexArray(m_vertex_array);
    glViewport(0, 0, m_resolution, m_resolution);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE); // Thin/open meshes and mirrored instances must still cast shadows.
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    if (GLEW_VERSION_3_2 || GLEW_ARB_depth_clamp)
        glDisable(GL_DEPTH_CLAMP);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDepthRange(0.0, 1.0);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUseProgram(depth_program);
    glUniformMatrix4fv(glGetUniformLocation(depth_program, "pcss_matrix"), 1, GL_FALSE, next.matrix.data());
    draw_depth();
    m_projection = next;
    m_revision   = input.revision;
    ++m_depth_generation;
    m_ready = true;
    return true;
}

void PCSSShadowRenderer::set_receiver_uniforms(unsigned program) const
{
    uniform_int(program, "pcss_enabled", 1);
    uniform_int(program, "pcss_depth", SHADOW_TEXTURE_UNIT);
    uniform_int(program, "pcss_blocker_samples", m_settings.blocker_samples);
    uniform_int(program, "pcss_filter_samples", m_settings.filter_samples);
    glUniformMatrix4fv(glGetUniformLocation(program, "pcss_matrix"), 1, GL_FALSE, m_projection.matrix.data());
    glUniform2fv(glGetUniformLocation(program, "pcss_extent"), 1, m_projection.extent.data());
    uniform_float(program, "pcss_depth_span", m_projection.depth_span);
    uniform_float(program, "pcss_min_caster_depth", m_projection.min_caster_depth);
    uniform_float(program, "pcss_tan_half_angle", pcss::tan_half_angle(m_settings.angular_diameter_deg));
    uniform_float(program, "pcss_bias_mm", m_settings.bias_mm);
    uniform_float(program, "pcss_max_radius_mm", m_settings.max_radius_mm);
    uniform_float(program, "pcss_plate_strength", m_settings.plate_strength);
}

PCSSReceiverScope::PCSSReceiverScope(const PCSSShadowRenderer* shadow, unsigned program)
{
    if (program == 0 || shadow == nullptr || !shadow->is_ready())
        return;
    m_program = program;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_active_texture);
    glActiveTexture(GL_TEXTURE0 + SHADOW_TEXTURE_UNIT);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_previous_texture);
    m_has_samplers = GLEW_VERSION_3_3 || GLEW_ARB_sampler_objects;
    if (m_has_samplers) {
        glGetIntegerv(GL_SAMPLER_BINDING, &m_previous_sampler);
        glBindSampler(SHADOW_TEXTURE_UNIT, 0); // Raw depth; an inherited comparison sampler would be incorrect.
    }
    glBindTexture(GL_TEXTURE_2D, shadow->m_depth_texture);
    glActiveTexture(m_active_texture); // Existing material textures keep using their original active unit.
    shadow->set_receiver_uniforms(program);
}

PCSSReceiverScope::~PCSSReceiverScope()
{
    if (m_program == 0)
        return;
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glUseProgram(m_program);
    uniform_int(m_program, "pcss_enabled", 0);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0 + SHADOW_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, m_previous_texture);
    if (m_has_samplers)
        glBindSampler(SHADOW_TEXTURE_UNIT, m_previous_sampler);
    glActiveTexture(m_active_texture);
}

void PCSSShadowRenderer::render_plate(unsigned                     program,
                                      const std::array<float, 16>& view_projection,
                                      float                        surface_z,
                                      const std::function<void()>& draw_polygon) const
{
    if (!m_ready || program == 0 || !draw_polygon || !std::isfinite(surface_z))
        return;
    ShadowGLState state;
    glBindVertexArray(m_vertex_array);
    glUseProgram(program);
    glUniformMatrix4fv(glGetUniformLocation(program, "view_projection_matrix"), 1, GL_FALSE, view_projection.data());
    uniform_float(program, "plate_surface_z", surface_z);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE); // Preserve geometry depth for subsequent picking/unprojection.
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    PCSSReceiverScope receiver(this, program);
    draw_polygon();
}

}} // namespace Slic3r::GUI
