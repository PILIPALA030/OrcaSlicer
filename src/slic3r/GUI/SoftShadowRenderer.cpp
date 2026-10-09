#include "libslic3r/libslic3r.h"
#include "SoftShadowRenderer.hpp"

#include "3DScene.hpp"
#include "Camera.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "OpenGLManager.hpp"
#include "PartPlate.hpp"

#include <GL/glew.h>

#include <boost/functional/hash.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace Slic3r {
namespace GUI {

namespace {

// Direction towards the light in world space, tilted so the shadow falls towards the default camera.
const Vec3d TO_LIGHT_DIR = Vec3d(-0.2, 0.3, 1.0).normalized();
constexpr float LIGHT_SPREAD = 0.06f;
constexpr double MAX_SEARCH_MM = 25.0;
constexpr double MIN_PENUMBRA_TEXELS = 1.5;
constexpr double DEPTH_BIAS_MM = 0.05;
constexpr double FRUSTUM_PADDING_MM = 1.0;
constexpr double GROUND_Z = 0.0;
constexpr unsigned int SHADOW_MAP_SIZE = 2048;
constexpr unsigned int MASK_SIZE = 2048;
constexpr double MIN_SHADOW_TEXEL_MM = 0.1;
constexpr double MIN_MASK_TEXEL_MM = 0.15;
constexpr double FOOTPRINT_MARGIN_TEXELS = 2.0;
constexpr int BLOCKER_SAMPLES_HQ = 16;
constexpr int PCF_SAMPLES_HQ = 32;
constexpr int BLOCKER_SAMPLES_LQ = 8;
constexpr int PCF_SAMPLES_LQ = 16;
constexpr float INTERACTIVE_MASK_SCALE = 0.5f;
constexpr float RECEIVER_Z = -0.01f;
constexpr float STRENGTH_LIGHT = 0.35f;
constexpr float STRENGTH_DARK = 0.5f;
constexpr float GPU_TIME_SMOOTHING = 0.1f;

/** @brief OpenGL states touched by the soft shadow passes, restored on destruction. */
class ShadowStateGuard
{
public:
    ShadowStateGuard()
    {
        _blend = glIsEnabled(GL_BLEND);
        _cullFace = glIsEnabled(GL_CULL_FACE);
        _depthTest = glIsEnabled(GL_DEPTH_TEST);
        glsafe(::glGetBooleanv(GL_DEPTH_WRITEMASK, &_depthMask));
        glsafe(::glGetBooleanv(GL_COLOR_WRITEMASK, _colorMask.data()));
        glsafe(::glGetFloatv(GL_COLOR_CLEAR_VALUE, _clearColor.data()));
        glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &_depthFunc));
        glsafe(::glGetIntegerv(GL_FRONT_FACE, &_frontFace));
        glsafe(::glGetIntegerv(GL_CULL_FACE_MODE, &_cullFaceMode));
        glsafe(::glGetIntegerv(GL_BLEND_SRC_RGB, &_blendSrc));
        glsafe(::glGetIntegerv(GL_BLEND_DST_RGB, &_blendDst));
        glsafe(::glGetIntegerv(GL_VIEWPORT, _viewport.data()));
        glsafe(::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &_drawFramebuffer));
        glsafe(::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &_readFramebuffer));
        glsafe(::glGetIntegerv(GL_ACTIVE_TEXTURE, &_activeTexture));
        glsafe(::glActiveTexture(GL_TEXTURE0));
        glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &_texture0));
        glsafe(::glActiveTexture(GL_TEXTURE1));
        glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &_texture1));
        glsafe(::glActiveTexture(static_cast<GLenum>(_activeTexture)));
    }

    ~ShadowStateGuard()
    {
        SetEnabled(GL_BLEND, _blend);
        SetEnabled(GL_CULL_FACE, _cullFace);
        SetEnabled(GL_DEPTH_TEST, _depthTest);
        glsafe(::glDepthMask(_depthMask));
        glsafe(::glColorMask(_colorMask[0], _colorMask[1], _colorMask[2], _colorMask[3]));
        glsafe(::glClearColor(_clearColor[0], _clearColor[1], _clearColor[2], _clearColor[3]));
        glsafe(::glDepthFunc(static_cast<GLenum>(_depthFunc)));
        glsafe(::glFrontFace(static_cast<GLenum>(_frontFace)));
        glsafe(::glCullFace(static_cast<GLenum>(_cullFaceMode)));
        glsafe(::glBlendFunc(static_cast<GLenum>(_blendSrc), static_cast<GLenum>(_blendDst)));
        glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(_drawFramebuffer)));
        glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(_readFramebuffer)));
        glsafe(::glViewport(_viewport[0], _viewport[1], _viewport[2], _viewport[3]));
        glsafe(::glBindSampler(0, 0));
        glsafe(::glBindSampler(1, 0));
        glsafe(::glActiveTexture(GL_TEXTURE1));
        glsafe(::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(_texture1)));
        glsafe(::glActiveTexture(GL_TEXTURE0));
        glsafe(::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(_texture0)));
        glsafe(::glActiveTexture(static_cast<GLenum>(_activeTexture)));
    }

    ShadowStateGuard(const ShadowStateGuard&) = delete;
    ShadowStateGuard& operator=(const ShadowStateGuard&) = delete;

private:
    static void SetEnabled(GLenum cap, GLboolean enabled)
    {
        if (enabled == GL_TRUE)
            glsafe(::glEnable(cap));
        else
            glsafe(::glDisable(cap));
    }

    GLboolean _blend{ GL_FALSE };
    GLboolean _cullFace{ GL_FALSE };
    GLboolean _depthTest{ GL_FALSE };
    GLboolean _depthMask{ GL_TRUE };
    std::array<GLboolean, 4> _colorMask{ { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE } };
    std::array<GLfloat, 4> _clearColor{ { 0.0f, 0.0f, 0.0f, 0.0f } };
    GLint _depthFunc{ GL_LESS };
    GLint _frontFace{ GL_CCW };
    GLint _cullFaceMode{ GL_BACK };
    GLint _blendSrc{ GL_SRC_ALPHA };
    GLint _blendDst{ GL_ONE_MINUS_SRC_ALPHA };
    std::array<GLint, 4> _viewport{ { 0, 0, 0, 0 } };
    GLint _drawFramebuffer{ 0 };
    GLint _readFramebuffer{ 0 };
    GLint _activeTexture{ GL_TEXTURE0 };
    GLint _texture0{ 0 };
    GLint _texture1{ 0 };
};

/** @brief Returns the 8 corners of a bounding box. */
std::array<Vec3d, 8> GetBoxCorners(const BoundingBoxf3& box)
{
    std::array<Vec3d, 8> corners;
    for (int i = 0; i < 8; ++i)
    {
        corners[i] = Vec3d((i & 1) != 0 ? box.max.x() : box.min.x(),
                           (i & 2) != 0 ? box.max.y() : box.min.y(),
                           (i & 4) != 0 ? box.max.z() : box.min.z());
    }
    return corners;
}

/** @brief Projects a point along the light direction onto the ground plane. */
Vec3d ProjectToGround(const Vec3d& point)
{
    const double t = (point.z() - GROUND_Z) / TO_LIGHT_DIR.z();
    return point - TO_LIGHT_DIR * t;
}

/** @brief Returns the ground-plane XY rectangle that may receive the shadow of a box. */
BoundingBoxf GetGroundFootprint(const BoundingBoxf3& box, double margin)
{
    BoundingBoxf footprint;
    const std::array<Vec3d, 8> corners = GetBoxCorners(box);
    for (const Vec3d& corner : corners)
    {
        footprint.merge(Vec2d(corner.x(), corner.y()));
        const Vec3d ground = ProjectToGround(corner);
        footprint.merge(Vec2d(ground.x(), ground.y()));
    }
    footprint.min -= Vec2d(margin, margin);
    footprint.max += Vec2d(margin, margin);
    return footprint;
}

/** @brief Builds a column-major orthographic projection equal to glOrtho. */
Matrix4d MakeOrtho(double left, double right, double bottom, double top, double nearZ, double farZ)
{
    Matrix4d m = Matrix4d::Identity();
    m(0, 0) = 2.0 / (right - left);
    m(1, 1) = 2.0 / (top - bottom);
    m(2, 2) = -2.0 / (farZ - nearZ);
    m(0, 3) = -(right + left) / (right - left);
    m(1, 3) = -(top + bottom) / (top - bottom);
    m(2, 3) = -(farZ + nearZ) / (farZ - nearZ);
    return m;
}

/** @brief Creates a 2D texture with the given storage and clamp mode. */
unsigned int CreateTexture2D(GLint internalFormat, GLenum format, GLenum type, unsigned int size)
{
    GLuint texture = 0;
    glsafe(::glGenTextures(1, &texture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, texture));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, static_cast<GLsizei>(size), static_cast<GLsizei>(size), 0,
                          format, type, nullptr));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    return static_cast<unsigned int>(texture);
}

/** @brief Creates a depth sampler; compare enables hardware PCF. */
unsigned int CreateDepthSampler(bool compare)
{
    GLuint sampler = 0;
    const GLfloat border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glsafe(::glGenSamplers(1, &sampler));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, compare ? GL_LINEAR : GL_NEAREST));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, compare ? GL_LINEAR : GL_NEAREST));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER));
    glsafe(::glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, compare ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE));
    glsafe(::glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL));
    return static_cast<unsigned int>(sampler);
}

/** @brief Returns true when the currently bound draw framebuffer is complete. */
bool IsDrawFramebufferComplete()
{
    const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    return status == GL_FRAMEBUFFER_COMPLETE;
}

/** @brief Returns true when the volume should cast a shadow. */
bool IsShadowCaster(const GLVolume& volume, bool renderSlaAuxiliaries)
{
    if (!volume.is_active || !volume.visible || volume.is_modifier)
        return false;
    if (!renderSlaAuxiliaries && volume.composite_id.volume_id < 0)
        return false;
    if (volume.force_transparent || volume.color.is_transparent())
        return false;
    return true;
}

} // namespace

void SoftShadowGpuTimer::Poll()
{
    for (std::size_t i = 0; i < _queries.size(); ++i)
    {
        if (!_pending[i] || static_cast<int>(i) == _activeSlot)
            continue;

        GLint available = 0;
        glsafe(::glGetQueryObjectiv(_queries[i], GL_QUERY_RESULT_AVAILABLE, &available));
        if (available == 0)
            continue;

        GLuint64 elapsedNs = 0;
        glsafe(::glGetQueryObjectui64v(_queries[i], GL_QUERY_RESULT, &elapsedNs));
        const float elapsedMs = static_cast<float>(static_cast<double>(elapsedNs) * 1.0e-6);
        _timeMs += (elapsedMs - _timeMs) * GPU_TIME_SMOOTHING;
        _pending[i] = false;
    }
}

void SoftShadowGpuTimer::Begin()
{
    if (_activeSlot >= 0)
        return;

    if (_queries[0] == 0)
        glsafe(::glGenQueries(static_cast<GLsizei>(_queries.size()), _queries.data()));

    Poll();
    if (_pending[_next])
        return;

    _activeSlot = static_cast<int>(_next);
    glsafe(::glBeginQuery(GL_TIME_ELAPSED, _queries[_next]));
}

void SoftShadowGpuTimer::End()
{
    if (_activeSlot < 0)
        return;

    glsafe(::glEndQuery(GL_TIME_ELAPSED));
    _pending[_activeSlot] = true;
    _next = (_next + 1) % static_cast<unsigned int>(_queries.size());
    _activeSlot = -1;
}

void SoftShadowGpuTimer::Release()
{
    if (_queries[0] != 0)
        glsafe(::glDeleteQueries(static_cast<GLsizei>(_queries.size()), _queries.data()));
    _queries.fill(0);
    _pending.fill(false);
    _next = 0;
    _activeSlot = -1;
    _timeMs = 0.0f;
}

bool SoftShadowRenderer::IsSupported()
{
    if (!wxGetApp().is_gl_version_greater_or_equal_to(3, 3))
        return false;
    if (OpenGLManager::get_framebuffers_type() != OpenGLManager::EFramebufferType::Arb)
        return false;
    return wxGetApp().get_shader("shadow_ground_mask") != nullptr && wxGetApp().get_shader("shadow_receiver") != nullptr;
}

bool SoftShadowRenderer::CreateShadowMapResources()
{
    _shadowDepthTexture = CreateTexture2D(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, _shadowMapSize);
    _rawDepthSampler = CreateDepthSampler(false);
    _compareDepthSampler = CreateDepthSampler(true);

    GLuint framebuffer = 0;
    glsafe(::glGenFramebuffers(1, &framebuffer));
    _shadowFramebuffer = static_cast<unsigned int>(framebuffer);
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer));
    glsafe(::glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _shadowDepthTexture, 0));
    glsafe(::glDrawBuffer(GL_NONE));
    glsafe(::glReadBuffer(GL_NONE));
    return _shadowDepthTexture != 0 && _rawDepthSampler != 0 && _compareDepthSampler != 0 && IsDrawFramebufferComplete();
}

bool SoftShadowRenderer::CreateMaskResources()
{
    _maskTexture = CreateTexture2D(GL_R8, GL_RED, GL_UNSIGNED_BYTE, _maskSize);

    GLuint framebuffer = 0;
    glsafe(::glGenFramebuffers(1, &framebuffer));
    _maskFramebuffer = static_cast<unsigned int>(framebuffer);
    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer));
    glsafe(::glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _maskTexture, 0));
    return _maskTexture != 0 && IsDrawFramebufferComplete();
}

bool SoftShadowRenderer::EnsureResources()
{
    if (_shadowFramebuffer != 0 && _maskFramebuffer != 0)
        return true;
    if (_resourcesFailed)
        return false;

    GLint maxTextureSize = 0;
    glsafe(::glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize));
    const unsigned int maxSize = static_cast<unsigned int>(std::max(maxTextureSize, 0));
    _shadowMapSize = std::min(SHADOW_MAP_SIZE, maxSize);
    _maskSize = std::min(MASK_SIZE, maxSize);
    if (_shadowMapSize < 512 || _maskSize < 512)
    {
        _resourcesFailed = true;
        BOOST_LOG_TRIVIAL(warning) << "Soft shadows disabled: max texture size " << maxTextureSize << " is too small";
        return false;
    }

    bool complete = false;
    {
        ShadowStateGuard stateGuard;
        complete = CreateShadowMapResources() && CreateMaskResources();
    }

    if (!complete)
    {
        Release();
        _resourcesFailed = true;
        BOOST_LOG_TRIVIAL(warning) << "Soft shadows disabled: unable to create complete framebuffers";
        return false;
    }
    return true;
}

bool SoftShadowRenderer::CollectCasters(const SoftShadowFrameInput& input)
{
    _casters.clear();
    _instanceBoxes.clear();
    _casterBox = BoundingBoxf3();

    std::map<std::pair<int, int>, std::size_t> instanceIndices;
    for (GLVolume* volume : input.volumes->volumes)
    {
        if (volume == nullptr || !IsShadowCaster(*volume, input.renderSlaAuxiliaries))
            continue;

        const BoundingBoxf3& box = volume->transformed_bounding_box();
        if (!box.defined)
            continue;

        _casters.push_back(volume);
        _casterBox.merge(box);

        const std::pair<int, int> key(volume->composite_id.object_id, volume->composite_id.instance_id);
        const std::map<std::pair<int, int>, std::size_t>::const_iterator it = instanceIndices.find(key);
        if (it == instanceIndices.end())
        {
            instanceIndices.emplace(key, _instanceBoxes.size());
            _instanceBoxes.push_back(box);
        }
        else
        {
            _instanceBoxes[it->second].merge(box);
        }
    }
    return !_casters.empty();
}

std::size_t SoftShadowRenderer::ComputeSignature(bool interactive) const
{
    std::size_t seed = 0;
    boost::hash_combine(seed, interactive);
    boost::hash_combine(seed, _casters.size());
    for (const GLVolume* volume : _casters)
    {
        boost::hash_combine(seed, volume);
        const Transform3d world = volume->world_matrix();
        for (int i = 0; i < 16; ++i)
            boost::hash_combine(seed, world.data()[i]);

        const BoundingBoxf3& box = volume->transformed_bounding_box();
        for (int i = 0; i < 3; ++i)
        {
            boost::hash_combine(seed, box.min[i]);
            boost::hash_combine(seed, box.max[i]);
        }
        boost::hash_combine(seed, volume->model.indices_count());
    }
    return seed;
}

void SoftShadowRenderer::FitLightFrustum()
{
    // Light basis: view +Z points towards the light, so the light looks down -Z.
    const Vec3d axisZ = TO_LIGHT_DIR;
    const Vec3d axisX = Vec3d::UnitY().cross(axisZ).normalized();
    const Vec3d axisY = axisZ.cross(axisX);
    Matrix3d rotation;
    rotation.row(0) = axisX.transpose();
    rotation.row(1) = axisY.transpose();
    rotation.row(2) = axisZ.transpose();
    _lightView = Transform3d::Identity();
    _lightView.linear() = rotation;

    BoundingBoxf3 lightBox;
    const std::array<Vec3d, 8> corners = GetBoxCorners(_casterBox);
    for (const Vec3d& corner : corners)
    {
        lightBox.merge(Vec3d(_lightView * corner));
        lightBox.merge(Vec3d(_lightView * ProjectToGround(corner)));
    }

    const double minExtent = static_cast<double>(_shadowMapSize) * MIN_SHADOW_TEXEL_MM;
    const double side = std::max({ lightBox.size().x(), lightBox.size().y(), minExtent }) + 2.0 * MAX_SEARCH_MM;
    const Vec3d center = lightBox.center();
    const double nearZ = -lightBox.max.z() - FRUSTUM_PADDING_MM;
    const double farZ = -lightBox.min.z() + FRUSTUM_PADDING_MM;
    _lightProjection = MakeOrtho(center.x() - 0.5 * side, center.x() + 0.5 * side,
                                 center.y() - 0.5 * side, center.y() + 0.5 * side, nearZ, farZ);
    _lightExtent = Vec2f(static_cast<float>(side), static_cast<float>(side));
    _depthSpan = static_cast<float>(farZ - nearZ);

    const BoundingBoxf ground = GetGroundFootprint(_casterBox, MAX_SEARCH_MM);
    const double maskMinExtent = static_cast<double>(_maskSize) * MIN_MASK_TEXEL_MM;
    const double maskSide = std::max({ ground.size().x(), ground.size().y(), maskMinExtent });
    const Vec2d maskCenter = ground.center();
    _maskRect = { { static_cast<float>(maskCenter.x() - 0.5 * maskSide), static_cast<float>(maskCenter.y() - 0.5 * maskSide),
                    static_cast<float>(maskCenter.x() + 0.5 * maskSide), static_cast<float>(maskCenter.y() + 0.5 * maskSide) } };
}

bool SoftShadowRenderer::RenderShadowMap()
{
    GLShaderProgram* const shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return false;

    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _shadowFramebuffer));
    glsafe(::glViewport(0, 0, static_cast<GLsizei>(_shadowMapSize), static_cast<GLsizei>(_shadowMapSize)));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE));
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));

    shader->start_using();
    shader->set_uniform("projection_matrix", _lightProjection);
    for (GLVolume* volume : _casters)
    {
        shader->set_uniform("view_model_matrix", _lightView * volume->world_matrix());
        volume->render();
    }
    shader->stop_using();

    glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    return true;
}

void SoftShadowRenderer::BuildFootprintModel()
{
    GLModel::Geometry geometry;
    geometry.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    geometry.reserve_vertices(4 * _instanceBoxes.size());
    geometry.reserve_indices(6 * _instanceBoxes.size());

    const double maskTexel = static_cast<double>(_maskRect[2] - _maskRect[0]) / static_cast<double>(_maskSize);
    const double margin = MAX_SEARCH_MM + FOOTPRINT_MARGIN_TEXELS * maskTexel;
    unsigned int vertexId = 0;
    for (const BoundingBoxf3& box : _instanceBoxes)
    {
        const BoundingBoxf footprint = GetGroundFootprint(box, margin);
        geometry.add_vertex(Vec3f(static_cast<float>(footprint.min.x()), static_cast<float>(footprint.min.y()), 0.0f));
        geometry.add_vertex(Vec3f(static_cast<float>(footprint.max.x()), static_cast<float>(footprint.min.y()), 0.0f));
        geometry.add_vertex(Vec3f(static_cast<float>(footprint.max.x()), static_cast<float>(footprint.max.y()), 0.0f));
        geometry.add_vertex(Vec3f(static_cast<float>(footprint.min.x()), static_cast<float>(footprint.max.y()), 0.0f));
        geometry.add_triangle(vertexId, vertexId + 1, vertexId + 2);
        geometry.add_triangle(vertexId, vertexId + 2, vertexId + 3);
        vertexId += 4;
    }

    _footprintModel.reset();
    _footprintModel.init_from(std::move(geometry));
}

bool SoftShadowRenderer::RenderGroundMask(bool interactive)
{
    GLShaderProgram* const shader = wxGetApp().get_shader("shadow_ground_mask");
    if (shader == nullptr)
        return false;

    glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _maskFramebuffer));
    glsafe(::glViewport(0, 0, static_cast<GLsizei>(_maskSize), static_cast<GLsizei>(_maskSize)));
    glsafe(::glClearColor(1.0f, 1.0f, 1.0f, 1.0f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT));

    _maskUvScale = interactive ? INTERACTIVE_MASK_SCALE : 1.0f;
    const GLsizei viewportSize = static_cast<GLsizei>(std::lround(static_cast<double>(_maskSize) * _maskUvScale));
    glsafe(::glViewport(0, 0, viewportSize, viewportSize));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glDisable(GL_CULL_FACE));

    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _shadowDepthTexture));
    glsafe(::glBindSampler(0, _rawDepthSampler));
    glsafe(::glActiveTexture(GL_TEXTURE1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _shadowDepthTexture));
    glsafe(::glBindSampler(1, _compareDepthSampler));

    const double shadowTexel = static_cast<double>(_lightExtent.x()) / static_cast<double>(_shadowMapSize);
    shader->start_using();
    shader->set_uniform("shadow_depth", 0);
    shader->set_uniform("shadow_depth_cmp", 1);
    shader->set_uniform("light_view_projection", Matrix4d(_lightProjection * _lightView.matrix()));
    shader->set_uniform("mask_rect", _maskRect);
    shader->set_uniform("ground_z", static_cast<float>(GROUND_Z));
    shader->set_uniform("depth_span", _depthSpan);
    shader->set_uniform("light_extent", _lightExtent);
    shader->set_uniform("light_spread", LIGHT_SPREAD);
    shader->set_uniform("min_penumbra", static_cast<float>(MIN_PENUMBRA_TEXELS * shadowTexel));
    shader->set_uniform("max_search", static_cast<float>(MAX_SEARCH_MM));
    shader->set_uniform("depth_bias", static_cast<float>(DEPTH_BIAS_MM / static_cast<double>(_depthSpan)));
    shader->set_uniform("blocker_samples", interactive ? BLOCKER_SAMPLES_LQ : BLOCKER_SAMPLES_HQ);
    shader->set_uniform("pcf_samples", interactive ? PCF_SAMPLES_LQ : PCF_SAMPLES_HQ);
    _footprintModel.render();
    shader->stop_using();
    return true;
}

bool SoftShadowRenderer::Update(const SoftShadowFrameInput& input)
{
    if (input.volumes == nullptr || !IsSupported())
        return false;

    if (!CollectCasters(input))
    {
        _maskValid = false;
        return false;
    }

    const std::size_t signature = ComputeSignature(input.interactive);
    const bool settled = _lastWasInteractive && !input.interactive;
    if (_maskValid && signature == _lastSignature && !settled)
        return true;

    if (!EnsureResources())
    {
        _maskValid = false;
        return false;
    }

    bool rendered = false;
    {
        ShadowStateGuard stateGuard;
        _updateTimer.Begin();
        FitLightFrustum();
        BuildFootprintModel();
        rendered = RenderShadowMap() && RenderGroundMask(input.interactive);
        _updateTimer.End();
    }

    _maskValid = rendered;
    _lastSignature = signature;
    _lastWasInteractive = input.interactive;
    return _maskValid;
}

void SoftShadowRenderer::RenderReceivers(const Camera& camera, PartPlateList& plates, bool darkMode)
{
    if (!_maskValid || _maskTexture == 0)
        return;

    GLShaderProgram* const shader = wxGetApp().get_shader("shadow_receiver");
    if (shader == nullptr)
        return;

    ShadowStateGuard stateGuard;
    _receiverTimer.Begin();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _maskTexture));

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("receiver_z", RECEIVER_Z);
    shader->set_uniform("shadow_mask", 0);
    shader->set_uniform("mask_rect", _maskRect);
    shader->set_uniform("mask_uv_scale", _maskUvScale);
    shader->set_uniform("shadow_strength", darkMode ? STRENGTH_DARK : STRENGTH_LIGHT);
    plates.RenderShadowReceivers();
    shader->stop_using();
    _receiverTimer.End();
}

void SoftShadowRenderer::Release()
{
    if (_shadowFramebuffer != 0)
        glsafe(::glDeleteFramebuffers(1, &_shadowFramebuffer));
    if (_maskFramebuffer != 0)
        glsafe(::glDeleteFramebuffers(1, &_maskFramebuffer));
    if (_shadowDepthTexture != 0)
        glsafe(::glDeleteTextures(1, &_shadowDepthTexture));
    if (_maskTexture != 0)
        glsafe(::glDeleteTextures(1, &_maskTexture));
    if (_rawDepthSampler != 0)
        glsafe(::glDeleteSamplers(1, &_rawDepthSampler));
    if (_compareDepthSampler != 0)
        glsafe(::glDeleteSamplers(1, &_compareDepthSampler));

    _shadowFramebuffer = 0;
    _maskFramebuffer = 0;
    _shadowDepthTexture = 0;
    _maskTexture = 0;
    _rawDepthSampler = 0;
    _compareDepthSampler = 0;
    _footprintModel.reset();
    _updateTimer.Release();
    _receiverTimer.Release();
    _casters.clear();
    _instanceBoxes.clear();
    _maskValid = false;
    _lastSignature = 0;
    _lastWasInteractive = false;
}

} // namespace GUI
} // namespace Slic3r
