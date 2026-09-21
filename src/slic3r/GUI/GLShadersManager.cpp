#include "libslic3r/libslic3r.h"
#include "libslic3r/Platform.hpp"
#include "GLShadersManager.hpp"
#include "3DScene.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/nowide/fstream.hpp>
#include <iterator>

#include <cassert>
#include <algorithm>
#include <string_view>
using namespace std::literals;

#include <boost/log/trivial.hpp>
#include <GL/glew.h>

namespace Slic3r {

std::pair<bool, std::string> GLShadersManager::init()
{
    std::string error;

    auto append_shader = [this, &error](const std::string& name, const GLShaderProgram::ShaderFilenames& filenames,
        const std::initializer_list<std::string_view> &defines = {}) {
        m_shaders.push_back(std::make_unique<GLShaderProgram>());
        if (!m_shaders.back()->init_from_files(name, filenames, defines)) {
            error += name + "\n";
            // if any error happens while initializating the shader, we remove it from the list
            m_shaders.pop_back();
            return false;
        }
        return true;
    };

    auto appendOptionalShader = [&append_shader, &error](const std::string& name,
                                                          const GLShaderProgram::ShaderFilenames& filenames) {
        const size_t errorLength = error.size();
        if (append_shader(name, filenames))
            return;

        error.erase(errorLength);
        BOOST_LOG_TRIVIAL(warning) << "Selection highlight shader unavailable: " << name;
    };

    assert(m_shaders.empty());

    bool valid = true;

    const std::string prefix = GUI::wxGetApp().is_gl_version_greater_or_equal_to(3, 1) ? "140/" : "110/";
    // imgui shader
    valid &= append_shader("imgui", { prefix + "imgui.vs", prefix + "imgui.fs" });
    // basic shader, used to render all what was previously rendered using the immediate mode
    valid &= append_shader("flat", { prefix + "flat.vs", prefix + "flat.fs" });
    // used to render selected geometry into the unified mask and stencil fallback
    appendOptionalShader("selection_mask", { prefix + "flat.vs", prefix + "selection_mask.fs" });
    // basic shader with plane clipping, used to render volumes in picking pass
    valid &= append_shader("flat_clip", { prefix + "flat_clip.vs", prefix + "flat_clip.fs" });
    // basic shader for textures, used to render textures
    valid &= append_shader("flat_texture", { prefix + "flat_texture.vs", prefix + "flat_texture.fs" });
    // used to render 3D scene background
    valid &= append_shader("background", { prefix + "background.vs", prefix + "background.fs" });
    // used to composite the selection fill and outline over the completed scene
    appendOptionalShader("selection_composite", { prefix + "background.vs", prefix + "selection_composite.fs" });
    // used to extract the selection edge from the low-resolution selection mask
    appendOptionalShader("selection_edge", { prefix + "background.vs", prefix + "selection_edge.fs" });
    // used to area-downsample the full-resolution selection mask before edge extraction
    appendOptionalShader("selection_area_downsample", { prefix + "background.vs", prefix + "selection_area_downsample.fs" });
    // used to apply directional Gaussian blur to selection edge textures
    appendOptionalShader("selection_gaussian", { prefix + "background.vs", prefix + "selection_gaussian.fs" });
    // used to render bed axes and model, selection hints, gcode sequential view marker model, preview shells, options in gcode preview
    valid &= append_shader("gouraud_light", { prefix + "gouraud_light.vs", prefix + "gouraud_light.fs" });
    //used to render thumbnail
    valid &= append_shader("thumbnail", { prefix + "thumbnail.vs", prefix + "thumbnail.fs"});
    // used to render printbed
    valid &= append_shader("printbed", { prefix + "printbed.vs", prefix + "printbed.fs" });
    // used to render options in gcode preview
    if (GUI::wxGetApp().is_gl_version_greater_or_equal_to(3, 3)) {
        valid &= append_shader("gouraud_light_instanced", { prefix + "gouraud_light_instanced.vs", prefix + "gouraud_light_instanced.fs" });
    }

    // used to render objects in 3d editor
    valid &= append_shader("gouraud", { prefix + "gouraud.vs", prefix + "gouraud.fs" }
#if ENABLE_ENVIRONMENT_MAP
        , { "ENABLE_ENVIRONMENT_MAP"sv }
#endif // ENABLE_ENVIRONMENT_MAP
        );
    // used to render variable layers heights in 3d editor
    valid &= append_shader("variable_layer_height", { prefix + "variable_layer_height.vs", prefix + "variable_layer_height.fs" });
    // used to render highlight contour around selected triangles inside the multi-material gizmo
    valid &= append_shader("mm_contour", { prefix + "mm_contour.vs", prefix + "mm_contour.fs" });
    // Used to render painted triangles inside the multi-material gizmo. Triangle normals are computed inside fragment shader.
    // For Apple's on Arm CPU computed triangle normals inside fragment shader using dFdx and dFdy has the opposite direction.
    // Because of this, objects had darker colors inside the multi-material gizmo.
    // Based on https://stackoverflow.com/a/66206648, the similar behavior was also spotted on some other devices with Arm CPU.
    // Since macOS 12 (Monterey), this issue with the opposite direction on Apple's Arm CPU seems to be fixed, and computed
    // triangle normals inside fragment shader have the right direction.
    if (platform_flavor() == PlatformFlavor::OSXOnArm && wxPlatformInfo::Get().GetOSMajorVersion() < 12)
        valid &= append_shader("mm_gouraud", { prefix + "mm_gouraud.vs", prefix + "mm_gouraud.fs" }, { "FLIP_TRIANGLE_NORMALS"sv });
    else
        valid &= append_shader("mm_gouraud", { prefix + "mm_gouraud.vs", prefix + "mm_gouraud.fs" });

    // Keep PCSS optional: a missing shader or old GPU must not break the ordinary GUI pipeline.
    if (prefix == "140/") {
        const size_t error_length = error.size();
        if (!append_shader("pcss_depth", {"140/pcss_depth.vs", "140/pcss_depth.fs"}))
            BOOST_LOG_TRIVIAL(warning) << "PCSS depth shader unavailable";
        error.resize(error_length);

        auto read_source = [](const std::string& filename) -> std::string {
            boost::nowide::ifstream stream(resources_dir() + "/shaders/140/" + filename, std::ios::binary);
            if (!stream)
                return {};
            return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        };
        const std::string common = read_source("pcss.glsl");
        auto append_pcss = [this, &read_source, &common](const std::string& name, const std::string& vertex, const std::string& fragment) {
            GLShaderProgram::ShaderSources sources{};
            sources[0] = read_source(vertex);
            sources[1] = read_source(fragment);
            if (common.empty() || sources[0].empty() || sources[1].empty()) {
                BOOST_LOG_TRIVIAL(warning) << "PCSS shader source missing: " << name;
                return;
            }
            for (size_t stage = 0; stage < 2; ++stage) {
                const size_t version_end = sources[stage].find('\n');
                if (sources[stage].compare(0, 8, "#version") != 0 || version_end == std::string::npos)
                    return;
                std::string prefix = "#define ENABLE_PCSS\n";
#if ENABLE_ENVIRONMENT_MAP
                prefix += "#define ENABLE_ENVIRONMENT_MAP\n";
#endif
                if (stage == 1)
                    prefix += common + "\n";
                sources[stage].insert(version_end + 1, prefix);
            }
            auto shader = std::make_unique<GLShaderProgram>();
            if (shader->init_from_texts(name, sources))
                m_shaders.push_back(std::move(shader));
            else
                BOOST_LOG_TRIVIAL(warning) << "PCSS receiver shader unavailable: " << name;
        };
        append_pcss("pcss_plate", "pcss_plate.vs", "pcss_plate.fs");
        append_pcss("gouraud_pcss", "gouraud.vs", "gouraud.fs");
        append_pcss("gouraud_light_pcss", "gouraud_light.vs", "gouraud_light.fs");
    }

    return { valid, error };
}

void GLShadersManager::shutdown()
{
    m_shaders.clear();
}

GLShaderProgram* GLShadersManager::get_shader(const std::string& shader_name)
{
    auto it = std::find_if(m_shaders.begin(), m_shaders.end(), [&shader_name](std::unique_ptr<GLShaderProgram>& p) { return p->get_name() == shader_name; });
    return (it != m_shaders.end()) ? it->get() : nullptr;
}

GLShaderProgram* GLShadersManager::get_current_shader()
{
    GLint id = 0;
    glsafe(::glGetIntegerv(GL_CURRENT_PROGRAM, &id));
    if (id == 0)
        return nullptr;

    auto it = std::find_if(m_shaders.begin(), m_shaders.end(), [id](std::unique_ptr<GLShaderProgram>& p) { return static_cast<GLint>(p->get_id()) == id; });
    return (it != m_shaders.end()) ? it->get() : nullptr;
}

} // namespace Slic3r

