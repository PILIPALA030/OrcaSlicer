#include "libslic3r/libslic3r.h"
#include "GLCanvas3D.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "OpenGLManager.hpp"

#include <GL/glew.h>
#include <Eigen/LU>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace Slic3r {
namespace GUI {
namespace {

// Fit useful caster bounds instead of increasing the allocation budget.
constexpr unsigned int SHADOW_MAP_SIZE = 512;
constexpr double SHADOW_ANGULAR_RADIUS_TAN = 0.035;
constexpr double SHADOW_BIAS_MM = 0.02;
constexpr double SHADOW_GUARD_TEXELS = 6.0;

bool has_shadow_samplers()
{
    return GLEW_VERSION_3_3 || GLEW_ARB_sampler_objects;
}

void restore_capability(GLenum capability, GLboolean enabled)
{
    if (enabled)
        glsafe(::glEnable(capability));
    else
        glsafe(::glDisable(capability));
}

// Covers allocation failures too. Picking and selection must not leak their
// depth function or polygon offset into the shadow pass, or lose their FBOs.
struct ShadowPassState
{
    OpenGLManager::EFramebufferType type;
    GLint draw_fbo{ 0 }, read_fbo{ 0 }, program{ 0 }, active_texture{ 0 }, texture{ 0 };
    GLint depth_func{ GL_LESS }, front_face{ GL_CCW }, cull_mode{ GL_BACK };
    std::array<GLint, 4> viewport{};
    std::array<GLint, 2> polygon_mode{};
    std::array<GLboolean, 4> color_mask{};
    std::array<GLdouble, 2> depth_range{};
    GLdouble clear_depth{ 1.0 };
    GLboolean depth_mask{}, depth_test{}, blend{}, cull{}, scissor{}, stencil{}, polygon_offset{};

    explicit ShadowPassState(OpenGLManager::EFramebufferType framebuffer_type) : type(framebuffer_type)
    {
        if (type == OpenGLManager::EFramebufferType::Arb) {
            glsafe(::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo));
            glsafe(::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo));
        } else {
            glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &draw_fbo));
        }
        glsafe(::glGetIntegerv(GL_CURRENT_PROGRAM, &program));
        glsafe(::glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture));
        glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture));
        glsafe(::glGetIntegerv(GL_VIEWPORT, viewport.data()));
        glsafe(::glGetIntegerv(GL_POLYGON_MODE, polygon_mode.data()));
        glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_func));
        glsafe(::glGetIntegerv(GL_FRONT_FACE, &front_face));
        glsafe(::glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode));
        glsafe(::glGetBooleanv(GL_COLOR_WRITEMASK, color_mask.data()));
        glsafe(::glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask));
        glsafe(::glGetDoublev(GL_DEPTH_RANGE, depth_range.data()));
        glsafe(::glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clear_depth));
        depth_test = glIsEnabled(GL_DEPTH_TEST);
        blend = glIsEnabled(GL_BLEND);
        cull = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        polygon_offset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    }

    ~ShadowPassState()
    {
        if (type == OpenGLManager::EFramebufferType::Arb) {
            glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo)));
            glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo)));
        } else {
            glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(draw_fbo)));
        }
        glsafe(::glViewport(viewport[0], viewport[1], viewport[2], viewport[3]));
        glsafe(::glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]));
        glsafe(::glDepthMask(depth_mask));
        glsafe(::glDepthFunc(static_cast<GLenum>(depth_func)));
        glsafe(::glDepthRange(depth_range[0], depth_range[1]));
        glsafe(::glClearDepth(clear_depth));
        glsafe(::glFrontFace(static_cast<GLenum>(front_face)));
        glsafe(::glCullFace(static_cast<GLenum>(cull_mode)));
        if (polygon_mode[0] == polygon_mode[1]) {
            glsafe(::glPolygonMode(GL_FRONT_AND_BACK, static_cast<GLenum>(polygon_mode[0])));
        } else {
            glsafe(::glPolygonMode(GL_FRONT, static_cast<GLenum>(polygon_mode[0])));
            glsafe(::glPolygonMode(GL_BACK, static_cast<GLenum>(polygon_mode[1])));
        }
        restore_capability(GL_DEPTH_TEST, depth_test);
        restore_capability(GL_BLEND, blend);
        restore_capability(GL_CULL_FACE, cull);
        restore_capability(GL_SCISSOR_TEST, scissor);
        restore_capability(GL_STENCIL_TEST, stencil);
        restore_capability(GL_POLYGON_OFFSET_FILL, polygon_offset);
        glsafe(::glUseProgram(static_cast<GLuint>(program)));
        glsafe(::glActiveTexture(static_cast<GLenum>(active_texture)));
        glsafe(::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture)));
    }

    ShadowPassState(const ShadowPassState&) = delete;
    ShadowPassState& operator=(const ShadowPassState&) = delete;
};

bool same_transform(const Transform3d& a, const Transform3d& b)
{
    return (a.matrix().array() == b.matrix().array()).all();
}

} // namespace

void GLCanvas3D::BindShadowUniforms(GLShaderProgram* shader) const
{
    if (shader == nullptr)
        return;
    // Different sampler types must never alias a unit, even when disabled.
    shader->set_uniform("shadow_map", 1);
    shader->set_uniform("shadow_map_pcf", 2);
    shader->set_uniform("shadow_enabled", m_shadowMap.valid);
    if (!m_shadowMap.valid)
        return;
    shader->set_uniform("shadow_matrix", m_shadowMap.lightViewProjection);
    const float texel = 1.0f / static_cast<float>(m_shadowMap.size);
    shader->set_uniform("shadow_map_texel_size", std::array<float, 2>{ texel, texel });
    shader->set_uniform("shadow_penumbra_scale", m_shadowMap.penumbraScale);
    shader->set_uniform("shadow_bias", m_shadowMap.depthBias);
    shader->set_uniform("shadow_hardware_pcf", m_shadowMap.comparisonSampler != 0);
}

void GLCanvas3D::BindShadowTextures()
{
    if (!m_shadowMap.valid || m_shadowMap.texturesBound)
        return;
    glsafe(::glGetIntegerv(GL_ACTIVE_TEXTURE, &m_shadowMap.sceneActiveTexture));
    for (unsigned int i = 0; i < 2; ++i) {
        glsafe(::glActiveTexture(GL_TEXTURE1 + i));
        glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_shadowMap.sceneTextureBindings[i]));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_shadowMap.depthTexture));
        if (has_shadow_samplers()) {
            glsafe(::glGetIntegerv(GL_SAMPLER_BINDING, &m_shadowMap.sceneSamplerBindings[i]));
            glsafe(::glBindSampler(1 + i, i == 0 ? m_shadowMap.rawSampler : m_shadowMap.comparisonSampler));
        }
    }
    glsafe(::glActiveTexture(GL_TEXTURE0));
    m_shadowMap.texturesBound = true;
}

void GLCanvas3D::UnbindShadowTextures()
{
    if (!m_shadowMap.texturesBound)
        return;
    for (unsigned int i = 0; i < 2; ++i) {
        glsafe(::glActiveTexture(GL_TEXTURE1 + i));
        glsafe(::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_shadowMap.sceneTextureBindings[i])));
        if (has_shadow_samplers())
            glsafe(::glBindSampler(1 + i, static_cast<GLuint>(m_shadowMap.sceneSamplerBindings[i])));
    }
    glsafe(::glActiveTexture(static_cast<GLenum>(m_shadowMap.sceneActiveTexture)));
    m_shadowMap.texturesBound = false;
}

bool GLCanvas3D::EnsureShadowMapResources(unsigned int size)
{
    if (size == 0 || !OpenGLManager::are_framebuffers_supported())
        return false;
    if (m_shadowMap.framebuffer != 0 && m_shadowMap.depthTexture != 0 &&
        m_shadowMap.colorTexture != 0 && m_shadowMap.size == size)
        return true;
    ReleaseShadowMapResources();
    const auto type = OpenGLManager::get_framebuffers_type();
    if (type == OpenGLManager::EFramebufferType::Arb) {
        glsafe(::glGenFramebuffers(1, &m_shadowMap.framebuffer));
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_shadowMap.framebuffer));
    } else if (type == OpenGLManager::EFramebufferType::Ext) {
        glsafe(::glGenFramebuffersEXT(1, &m_shadowMap.framebuffer));
        glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_shadowMap.framebuffer));
    } else {
        return false;
    }
    const std::array<GLfloat, 4> border{ 1.0f, 1.0f, 1.0f, 1.0f };
    glsafe(::glGenTextures(1, &m_shadowMap.depthTexture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_shadowMap.depthTexture));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER));
    glsafe(::glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border.data()));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(size),
                         static_cast<GLsizei>(size), 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr));
    // Retain the unwritten compatibility attachment for legacy EXT FBOs.
    glsafe(::glGenTextures(1, &m_shadowMap.colorTexture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_shadowMap.colorTexture));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(size), static_cast<GLsizei>(size),
                         0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    if (type == OpenGLManager::EFramebufferType::Arb) {
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowMap.depthTexture, 0));
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_shadowMap.colorTexture, 0));
    } else {
        glsafe(::glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, m_shadowMap.depthTexture, 0));
        glsafe(::glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, m_shadowMap.colorTexture, 0));
    }
    glsafe(::glDrawBuffer(GL_COLOR_ATTACHMENT0));
    const bool complete = type == OpenGLManager::EFramebufferType::Arb ?
        ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE :
        ::glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;
    if (!complete) {
        BOOST_LOG_TRIVIAL(warning) << "PCSS shadow framebuffer is incomplete; shadows disabled for this canvas";
        ReleaseShadowMapResources();
        m_shadowMap.allocationFailed = true;
        return false;
    }
    if (has_shadow_samplers()) {
        glsafe(::glGenSamplers(1, &m_shadowMap.rawSampler));
        glsafe(::glGenSamplers(1, &m_shadowMap.comparisonSampler));
        const std::array<GLuint, 2> samplers{ m_shadowMap.rawSampler, m_shadowMap.comparisonSampler };
        for (unsigned int i = 0; i < samplers.size(); ++i) {
            const GLuint sampler = samplers[i];
            glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, i == 0 ? GL_NEAREST : GL_LINEAR));
            glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, i == 0 ? GL_NEAREST : GL_LINEAR));
            glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, i == 0 ? GL_NONE : GL_COMPARE_R_TO_TEXTURE));
            glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL));
            glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER));
            glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER));
            glsafe(::glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border.data()));
        }
    }
    m_shadowMap.size = size;
    return true;
}

void GLCanvas3D::ReleaseShadowMapResources()
{
    UnbindShadowTextures();
    const auto type = OpenGLManager::get_framebuffers_type();
    if (m_shadowMap.framebuffer != 0) {
        if (type == OpenGLManager::EFramebufferType::Arb)
            glsafe(::glDeleteFramebuffers(1, &m_shadowMap.framebuffer));
        else if (type == OpenGLManager::EFramebufferType::Ext)
            glsafe(::glDeleteFramebuffersEXT(1, &m_shadowMap.framebuffer));
    }
    if (m_shadowMap.rawSampler != 0)
        glsafe(::glDeleteSamplers(1, &m_shadowMap.rawSampler));
    if (m_shadowMap.comparisonSampler != 0)
        glsafe(::glDeleteSamplers(1, &m_shadowMap.comparisonSampler));
    if (m_shadowMap.depthTexture != 0)
        glsafe(::glDeleteTextures(1, &m_shadowMap.depthTexture));
    if (m_shadowMap.colorTexture != 0)
        glsafe(::glDeleteTextures(1, &m_shadowMap.colorTexture));
    m_shadowMap = ShadowMapResources{};
}

bool GLCanvas3D::RenderShadowMap(const Camera& camera)
{
    const bool was_valid = m_shadowMap.valid;
    m_shadowMap.valid = false;
    if (m_shadowMap.allocationFailed || wxGetApp().app_config == nullptr ||
        !wxGetApp().app_config->get_bool("show_model_shadow"))
        return false;
    GLShaderProgram* const shader = wxGetApp().get_shader("shadow_depth");
    const auto type = OpenGLManager::get_framebuffers_type();
    if (shader == nullptr || (type != OpenGLManager::EFramebufferType::Arb && type != OpenGLManager::EFramebufferType::Ext))
        return false;
    BoundingBoxf3 caster_bounds;
    for (const GLVolume* volume : m_volumes.volumes) {
        if (volume != nullptr && volume->is_active && !volume->is_modifier && !volume->is_wipe_tower && !volume->is_extrusion_path)
            caster_bounds.merge(volume->transformed_bounding_box());
    }
    if (!caster_bounds.defined)
        return false;
    BoundingBoxf3 scene_bounds = _max_bounding_box(false, true, true);
    scene_bounds.merge(caster_bounds);
    const double radius = std::max(1.0, 0.5 * scene_bounds.size().norm());
    if (!std::isfinite(radius))
        return false;
    const Vec3d eye_light_direction(-0.4574957, 0.4574957, 0.7624929);
    const Matrix3d view_rotation = camera.get_view_matrix().matrix().block(0, 0, 3, 3);
    const Vec3d light_direction = (view_rotation.transpose() * eye_light_direction).normalized();
    const Vec3d up = std::abs(light_direction.z()) > 0.95 ? Vec3d::UnitY() : Vec3d::UnitZ();
    const Vec3d target = scene_bounds.center();
    Camera shadow_camera;
    shadow_camera.set_type(Camera::EType::Ortho);
    shadow_camera.set_viewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    shadow_camera.look_at(target + light_direction * (2.0 * radius), target, up);
    const Transform3d& light_view = shadow_camera.get_view_matrix();
    const BoundingBoxf3 light_casters = caster_bounds.transformed(light_view);
    const BoundingBoxf3 light_scene = scene_bounds.transformed(light_view);
    // Shadowed receivers share caster XY. The bed/empty build volume must
    // not consume most of our 512x512 map's useful XY resolution.
    const double useful_extent = std::max(1.0, std::max(light_casters.size().x(), light_casters.size().y()));
    const double extent = useful_extent * SHADOW_MAP_SIZE / (SHADOW_MAP_SIZE - 2.0 * SHADOW_GUARD_TEXELS);
    const double world_texel = extent / SHADOW_MAP_SIZE;
    const double center_x = std::floor(light_casters.center().x() / world_texel + 0.5) * world_texel;
    const double center_y = std::floor(light_casters.center().y() / world_texel + 0.5) * world_texel;
    const double z_margin = std::max(1.0, radius * 0.01);
    const double near_plane = std::max(0.01, -light_scene.max.z() - z_margin);
    const double far_plane = std::max(near_plane + 1.0, -light_scene.min.z() + z_margin);
    shadow_camera.apply_projection(center_x - extent * 0.5, center_x + extent * 0.5,
                                   center_y - extent * 0.5, center_y + extent * 0.5, near_plane, far_plane);
    const Transform3d light_vp = shadow_camera.get_projection_matrix() * light_view;
    std::vector<ShadowCasterState> casters;
    casters.reserve(m_volumes.volumes.size());
    for (GLVolume* volume : m_volumes.volumes) {
        if (volume == nullptr || !volume->is_active || volume->is_modifier || volume->is_wipe_tower || volume->is_extrusion_path)
            continue;
        // Acquire the existing worker hand-off before reading any LOD data.
        volume->promote_ready_lod_models();
        GLModel* model = &volume->model;
        if (model->is_render_disabled() || !model->is_initialized())
            continue;
        const auto original_range = volume->tverts_range;
        const bool full_range = original_range.first == 0 &&
            (original_range.second == static_cast<size_t>(-1) || original_range.second >= model->indices_count());
        if (full_range) {
            const BoundingBoxf3 box = volume->transformed_bounding_box().transformed(light_view);
            const double footprint = std::max(box.size().x(), box.size().y()) / world_texel;
            const auto ready = [](const std::shared_ptr<GLModel>& lod) {
                return lod && !lod->is_render_disabled() && lod->is_initialized();
            };
            // Independent shadow LOD: never alter the visible scene's LOD,
            // build a simplified mesh, or wait for a worker in this pass.
            if (footprint < 96.0 && ready(volume->m_modelSmall))
                model = volume->m_modelSmall.get();
            else if (ready(volume->m_modelMiddle))
                model = volume->m_modelMiddle.get();
        }
        const size_t end = full_range ? model->indices_count() : std::min(original_range.second, model->indices_count());
        const size_t begin = std::min(original_range.first, end);
        if (begin == end)
            continue;
        casters.push_back({ model, model->geometry_revision(), volume->world_matrix(), { begin, end } });
    }
    if (casters.empty())
        return false;
    const auto z_range = m_volumes.get_z_range();
    const auto clipping_plane = m_volumes.get_clipping_plane();
    bool unchanged = was_valid && m_shadowMap.size == SHADOW_MAP_SIZE &&
        same_transform(light_vp, m_shadowMap.lightViewProjection) &&
        z_range == m_shadowMap.cachedZRange && clipping_plane == m_shadowMap.cachedClippingPlane &&
        casters.size() == m_shadowMap.cachedCasters.size();
    for (size_t i = 0; unchanged && i < casters.size(); ++i) {
        const auto& a = casters[i];
        const auto& b = m_shadowMap.cachedCasters[i];
        unchanged = a.model == b.model && a.geometryRevision == b.geometryRevision && a.range == b.range &&
                    same_transform(a.worldMatrix, b.worldMatrix);
    }
    // No GL state queries, texture uploads or geometry submission on a hit.
    if (unchanged) {
        m_shadowMap.valid = true;
        return true;
    }
    const ShadowPassState saved_state(type);
    if (!EnsureShadowMapResources(SHADOW_MAP_SIZE))
        return false;
    if (type == OpenGLManager::EFramebufferType::Arb)
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_shadowMap.framebuffer));
    else
        glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_shadowMap.framebuffer));
    glsafe(::glViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE));
    glsafe(::glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDepthRange(0.0, 1.0));
    glsafe(::glClearDepth(1.0));
    glsafe(::glPolygonMode(GL_FRONT_AND_BACK, GL_FILL));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glCullFace(GL_BACK));
    glsafe(::glDisable(GL_SCISSOR_TEST));
    glsafe(::glDisable(GL_STENCIL_TEST));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    shader->start_using();
    shader->set_uniform("projection_matrix", shadow_camera.get_projection_matrix());
    shader->set_uniform("z_range", z_range);
    shader->set_uniform("clipping_plane", clipping_plane);
    for (const ShadowCasterState& caster : casters) {
        glsafe(::glFrontFace(caster.worldMatrix.matrix().block(0, 0, 3, 3).determinant() < 0.0 ? GL_CW : GL_CCW));
        shader->set_uniform("view_model_matrix", light_view * caster.worldMatrix);
        shader->set_uniform("volume_world_matrix", caster.worldMatrix);
        // Shared buffers and position-only shader; skip GLVolume's material
        // parsing, segmentation reconstruction and unrelated scene work.
        caster.model->render(caster.range);
    }
    shader->stop_using();
    const double depth_range = far_plane - near_plane;
    const float scale = static_cast<float>(depth_range * SHADOW_ANGULAR_RADIUS_TAN / extent);
    m_shadowMap.penumbraScale = { scale, scale };
    m_shadowMap.depthBias = static_cast<float>(SHADOW_BIAS_MM / depth_range);
    m_shadowMap.lightViewProjection = light_vp;
    m_shadowMap.cachedZRange = z_range;
    m_shadowMap.cachedClippingPlane = clipping_plane;
    m_shadowMap.cachedCasters = std::move(casters);
    m_shadowMap.valid = true;
    return true;
}

} // namespace GUI
} // namespace Slic3r
