// Test adapters: the harness compiles the actual shadow renderer with these
// lightweight app interfaces and real Eigen/GLEW/OpenGL. Not a slicer build.
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <Eigen/Geometry>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#define glsafe(cmd) do { cmd; } while (false)
#define BOOST_LOG_TRIVIAL(level) std::cerr

namespace Slic3r {
using Vec3d = Eigen::Vector3d;
using Matrix3d = Eigen::Matrix3d;
using Transform3d = Eigen::Affine3d;
struct BoundingBoxf3 {
    Vec3d min{Vec3d::Zero()}, max{Vec3d::Zero()};
    bool defined{false};
    Vec3d size() const { return max - min; }
    Vec3d center() const { return (min + max) * 0.5; }
    void merge(const Vec3d& p) {
        if (!defined) { min = max = p; defined = true; }
        else { min = min.cwiseMin(p); max = max.cwiseMax(p); }
    }
    void merge(const BoundingBoxf3& box) { if (box.defined) { merge(box.min); merge(box.max); } }
    BoundingBoxf3 transformed(const Transform3d& t) const {
        BoundingBoxf3 b;
        if (defined)
            for (int i = 0; i < 8; ++i) {
                Vec3d p((i & 1) ? max.x() : min.x(), (i & 2) ? max.y() : min.y(), (i & 4) ? max.z() : min.z());
                b.merge(Vec3d(t * p));
            }
        return b;
    }
};
struct GLShaderProgram {
    void start_using() { glUseProgram(0); }
    void stop_using() { glUseProgram(0); }
    template<class T> void set_uniform(const char*, const T&) {}
};
namespace GUI {
struct GLModel {
    unsigned int draws{0};
    std::uint64_t revision{1};
    bool disabled{false};
    size_t count{36};
    bool is_initialized() const { return count != 0; }
    bool is_render_disabled() const { return disabled; }
    size_t indices_count() const { return count; }
    std::uint64_t geometry_revision() const { return revision; }
    void render(const std::pair<size_t, size_t>& range) { assert(range.first < range.second); ++draws; }
};
struct Camera {
    enum class EType { Ortho };
    Transform3d view{Transform3d::Identity()}, projection{Transform3d::Identity()};
    void set_type(EType) {}
    void set_viewport(int, int, int, int) {}
    const Transform3d& get_view_matrix() const { return view; }
    const Transform3d& get_projection_matrix() const { return projection; }
    void look_at(const Vec3d& p, const Vec3d& target, const Vec3d& up) {
        const Vec3d z = (p-target).normalized(), x = up.cross(z).normalized(), y = z.cross(x).normalized();
        view.matrix() << x.x(), x.y(), x.z(), -x.dot(p), y.x(), y.y(), y.z(), -y.dot(p),
                         z.x(), z.y(), z.z(), -z.dot(p), 0, 0, 0, 1;
    }
    void apply_projection(double l, double r, double b, double t, double n, double f) {
        projection.matrix() << 2/(r-l), 0, 0, -(l+r)/(r-l), 0, 2/(t-b), 0, -(b+t)/(t-b),
                               0, 0, -2/(f-n), -(n+f)/(f-n), 0, 0, 0, 1;
    }
};
struct OpenGLManager {
    enum class EFramebufferType { Unknown, Arb, Ext };
    static EFramebufferType type;
    static EFramebufferType get_framebuffers_type() { return type; }
    static bool are_framebuffers_supported() { return true; }
};
OpenGLManager::EFramebufferType OpenGLManager::type = OpenGLManager::EFramebufferType::Arb;
struct Config {
    bool enabled{true};
    bool get_bool(const char*) const { return enabled; }
};
struct App {
    Config config;
    Config* app_config{&config};
    GLShaderProgram shader;
    GLShaderProgram* get_shader(const char*) { return &shader; }
};
App& wxGetApp() { static App a; return a; }
} // GUI
struct GLVolume {
    bool is_active{true}, is_modifier{false}, is_wipe_tower{false}, is_extrusion_path{false};
    GUI::GLModel model;
    std::shared_ptr<GUI::GLModel> m_modelSmall, m_modelMiddle;
    std::pair<size_t, size_t> tverts_range{0, static_cast<size_t>(-1)};
    Transform3d world{Transform3d::Identity()};
    bool pending_middle{false};
    BoundingBoxf3 transformed_bounding_box() const {
        BoundingBoxf3 b; b.merge(Vec3d(0,0,0)); b.merge(Vec3d(30,30,80)); return b.transformed(world);
    }
    Transform3d world_matrix() const { return world; }
    void promote_ready_lod_models() {
        if (pending_middle) { m_modelMiddle->disabled = false; pending_middle = false; }
    }
};
struct Volumes {
    std::vector<GLVolume*> volumes;
    std::array<float,2> z_range{-1000,1000};
    std::array<double,4> clip{0,0,0,1};
    const std::array<float,2>& get_z_range() const { return z_range; }
    const std::array<double,4>& get_clipping_plane() const { return clip; }
};
namespace GUI {
class GLCanvas3D {
public:
// PCSS_RESOURCE_FIELDS -- injected verbatim from GLCanvas3D.hpp by the test
    Volumes m_volumes;
    BoundingBoxf3 _max_bounding_box(bool, bool, bool) const {
        BoundingBoxf3 b; b.merge(Vec3d(-150,-150,0)); b.merge(Vec3d(150,150,0));
        for (const auto* v : m_volumes.volumes) if (v && v->is_active) b.merge(v->transformed_bounding_box());
        return b;
    }
    void BindShadowUniforms(GLShaderProgram*) const;
    void BindShadowTextures();
    void UnbindShadowTextures();
    bool EnsureShadowMapResources(unsigned int);
    void ReleaseShadowMapResources();
    bool RenderShadowMap(const Camera&);
};
} // GUI
} // Slic3r
