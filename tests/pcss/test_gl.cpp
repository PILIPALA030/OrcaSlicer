#include "PCSSShadowRenderer.hpp"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#ifndef PCSS_SHADER_DIRECTORY
#define PCSS_SHADER_DIRECTORY "resources/shaders/140"
#endif

using namespace Slic3r::GUI;

namespace {
unsigned                    assertions = 0;
const std::array<float, 16> IDENTITY{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
void                        check(bool condition, const char* message)
{
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}
void no_errors(const char* message)
{
    GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        std::cerr << "OpenGL error: " << error << '\n';
    check(error == GL_NO_ERROR, message);
}
std::string read(const std::string& name)
{
    std::ifstream file(std::string(PCSS_SHADER_DIRECTORY) + "/" + name);
    if (!file)
        throw std::runtime_error("Cannot read shipping shader " + name);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::string add_common(std::string source, bool fragment, bool environment = false)
{
    const size_t line = source.find('\n');
    if (line == std::string::npos || source.compare(0, 8, "#version") != 0)
        throw std::runtime_error("Shader must start with #version");
    source.insert(line + 1, std::string("#define ENABLE_PCSS\n") + (environment ? "#define ENABLE_ENVIRONMENT_MAP\n" : "") +
                                (fragment ? read("pcss.glsl") : ""));
    return source;
}
GLuint compile(GLenum stage, const std::string& source)
{
    const GLuint shader = glCreateShader(stage);
    const char*  text   = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        throw std::runtime_error(std::string("Shader compilation: ") + log);
    }
    return shader;
}
GLuint program(const std::string& vertex, const std::string& fragment)
{
    const GLuint vs = compile(GL_VERTEX_SHADER, vertex);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
    const GLuint id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glLinkProgram(id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[8192]{};
        glGetProgramInfoLog(id, sizeof(log), nullptr, log);
        throw std::runtime_error(std::string("Shader link: ") + log);
    }
    check(true, "Shipping shader linked");
    return id;
}
void matrix(GLuint id, const char* name, const std::array<float, 16>& value)
{
    glUniformMatrix4fv(glGetUniformLocation(id, name), 1, GL_FALSE, value.data());
}
std::vector<GLint> state()
{
    std::vector<GLint> out;
    for (GLenum token :
         {GL_DRAW_FRAMEBUFFER_BINDING, GL_READ_FRAMEBUFFER_BINDING, GL_CURRENT_PROGRAM, GL_VERTEX_ARRAY_BINDING, GL_ARRAY_BUFFER_BINDING,
          GL_ELEMENT_ARRAY_BUFFER_BINDING, GL_PIXEL_UNPACK_BUFFER_BINDING, GL_ACTIVE_TEXTURE, GL_TEXTURE_BINDING_2D, GL_DEPTH_FUNC,
          GL_DEPTH_WRITEMASK, GL_FRONT_FACE, GL_CULL_FACE_MODE, GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB}) {
        GLint value = 0;
        glGetIntegerv(token, &value);
        out.push_back(value);
    }
    for (GLenum token : {GL_BLEND, GL_SCISSOR_TEST, GL_DEPTH_TEST, GL_CULL_FACE, GL_POLYGON_OFFSET_FILL, GL_STENCIL_TEST,
                         GL_RASTERIZER_DISCARD, GL_SAMPLE_ALPHA_TO_COVERAGE})
        out.push_back(glIsEnabled(token));
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    out.insert(out.end(), viewport, viewport + 4);
    GLint mask[4];
    glGetIntegerv(GL_COLOR_WRITEMASK, mask);
    out.insert(out.end(), mask, mask + 4);
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    GLFWwindow* window = nullptr;
    try {
        check(glfwInit() == GLFW_TRUE, "GLFW initialization (use xvfb-run on headless Linux)");
        const bool core = argc > 1 && std::string(argv[1]) == "--core";
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, core ? 3 : 1);
        if (core)
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(512, 64, "PCSS renderer tests", nullptr, nullptr);
        check(window != nullptr, "Create OpenGL context");
        glfwMakeContextCurrent(window);
        glewExperimental = GL_TRUE;
        check(glewInit() == GLEW_OK, "Initialize GLEW");
        while (glGetError() != GL_NO_ERROR) {} // GLEW probes removed extension enums on some core contexts.
        std::cout << "GL: " << glGetString(GL_VERSION) << "; renderer: " << glGetString(GL_RENDERER) << '\n';
        std::vector<GLuint> programs;
        const GLuint        depth = program(read("pcss_depth.vs"), read("pcss_depth.fs"));
        programs.push_back(depth);
        const GLuint plate = program(add_common(read("pcss_plate.vs"), false), add_common(read("pcss_plate.fs"), true));
        programs.push_back(plate);
        programs.push_back(program(add_common(read("gouraud.vs"), false), add_common(read("gouraud.fs"), true)));
        programs.push_back(program(add_common(read("gouraud.vs"), false, true), add_common(read("gouraud.fs"), true, true)));
        programs.push_back(program(add_common(read("gouraud_light.vs"), false), add_common(read("gouraud_light.fs"), true)));
        const GLuint probe = program(R"(#version 140
out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
})",
                                     add_common(R"(#version 140
in vec2 uv;
uniform float receiver_z;
uniform float receiver_slope;
out vec4 color;
void main() {
    vec2 world_xy = (uv - 0.5) * vec2(40.0, 4.0);
    float visibility = pcss_visibility(vec3(world_xy, receiver_z + world_xy.x * receiver_slope));
    color = vec4(visibility, visibility, visibility, 1.0);
})",
                                                true));
        programs.push_back(probe);

        GLuint vao, vbo, ebo, output_fbo, output_texture;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        const unsigned short indices[]{0, 1, 2, 0, 2, 3};
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        auto upload_plane = [&](float height) {
            const float vertices[]{-5, -5, height, 5, -5, height, 5, 5, height, -5, 5, height};
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
        };
        upload_plane(10);
        glGenTextures(1, &output_texture);
        glBindTexture(GL_TEXTURE_2D, output_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 512, 64, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &output_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_texture, 0);
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Readback framebuffer");
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); // Deliberately distinct read and draw targets.
        glViewport(13, 7, 111, 37);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 1, 1);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CW);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_GREATER);
        glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
        glUseProgram(probe);
        glActiveTexture(GL_TEXTURE3);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, ebo); // Allocation must not interpret nullptr as a PBO byte offset.
        const auto before = state();

        PCSSShadowRenderer shadow;
        PCSSSettings       settings;
        settings.resolution           = 1024;
        settings.angular_diameter_deg = 12;
        settings.blocker_samples      = 64;
        settings.filter_samples       = 64;
        check(shadow.set_settings(settings), "Valid renderer settings");
        PCSSSettings invalid   = settings;
        invalid.filter_samples = 0;
        check(!shadow.set_settings(invalid), "Reject zero filter samples");
        invalid         = settings;
        invalid.bias_mm = NAN;
        check(!shadow.set_settings(invalid), "Reject nonfinite bias");
        PCSSFrameInput input;
        input.casters         = {{-5, -5, 0}, {5, 5, 40}};
        input.receivers       = {{-20, -20, 0}, {20, 20, 0}};
        input.to_light        = {0, 0, 1};
        input.revision        = 1;
        unsigned draws        = 0;
        auto     model_matrix = IDENTITY;
        auto     draw         = [&]() {
            ++draws;
            matrix(depth, "volume_world_matrix", model_matrix);
            glUniform4f(glGetUniformLocation(depth, "clipping_plane"), 0, 0, 0, 1);
            glUniform2f(glGetUniformLocation(depth, "z_range"), -1000, 1000);
            const GLint position = glGetAttribLocation(depth, "v_position");
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
            glEnableVertexAttribArray(position);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
            glDisableVertexAttribArray(position);
        };
        check(shadow.update(input, depth, draw), "Initial actual depth rendering");
        check(shadow.is_ready() && draws == 1, "Depth map is ready");
        check(before == state(), "Depth update restores GL state including separate FBO targets");
        no_errors("Initial update produces no GL errors");
        const auto generation = shadow.depth_generation();
        check(shadow.update(input, depth, draw) && draws == 1 && shadow.depth_generation() == generation,
              "Cache skips identical depth draw");

        auto visibility = [&](float receiver_z = 0.0f, bool enabled = true, float slope = 0.0f) {
            glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glViewport(0, 0, 512, 64);
            glBindVertexArray(vao);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glUseProgram(probe);
            glUniform1f(glGetUniformLocation(probe, "receiver_z"), receiver_z);
            glUniform1f(glGetUniformLocation(probe, "receiver_slope"), slope);
            {
                PCSSReceiverScope scope(enabled ? &shadow : nullptr, probe);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            std::vector<float> values(512 * 4);
            glReadPixels(0, 32, 512, 1, GL_RGBA, GL_FLOAT, values.data());
            std::vector<float> row;
            for (unsigned x = 0; x < 512; ++x)
                row.push_back(values[4 * x]);
            no_errors("Readback shader execution");
            check(std::all_of(row.begin(), row.end(), [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; }),
                  "Finite visibility in [0, 1]");
            return row;
        };
        auto fractional = [](const std::vector<float>& row) {
            return std::count_if(row.begin(), row.end(), [](float v) { return v > 0.001f && v < 0.999f; });
        };
        const auto low = visibility();
        check(low[256] < 0.01f && low.front() > 0.99f, "Blocker center is shadowed and outside is lit");
        check(fractional(low) > 4, "PCSS creates a real partially visible transition");
        const auto disabled = visibility(0, false);
        check(std::all_of(disabled.begin(), disabled.end(), [](float v) { return v == 1; }),
              "Disabled binding is fully lit, with no stale PCSS state");
        upload_plane(40);
        ++input.revision;
        check(shadow.update(input, depth, draw), "Moving geometry updates depth map");
        const auto high = visibility();
        std::cout << "Penumbra fractional pixels, heights 10/40: " << fractional(low) << '/' << fractional(high) << '\n';
        check(fractional(high) > 2 * fractional(low), "Larger blocker-receiver gap increases penumbra");
        const auto near_contact = visibility(39.9f);
        check(fractional(near_contact) < fractional(low), "Contact hardens the shadow");
        settings.angular_diameter_deg = 2;
        check(shadow.set_settings(settings), "Smaller light setting");
        const auto small_light = visibility();
        check(fractional(small_light) < fractional(high), "Smaller light narrows penumbra without rebuilding depth geometry");
        settings.angular_diameter_deg = 0;
        check(shadow.set_settings(settings), "Point limit setting");
        const auto hard = visibility();
        check(fractional(hard) == 0, "Zero angular diameter gives binary hard shadows");

        model_matrix[12] = 12;
        ++input.revision;
        check(shadow.update(input, depth, draw), "Model translation changes the depth pass");
        const auto translated = visibility();
        check(translated[256] > 0.99f && translated[409] < 0.01f, "Translated object moves its shadow");
        model_matrix    = IDENTITY;
        model_matrix[0] = 2;
        model_matrix[5] = 0.5f;
        ++input.revision;
        check(shadow.update(input, depth, draw), "Nonuniform scale updates depth");
        const auto scaled = visibility();
        check(scaled[358] < 0.01f, "Scaled shadow follows the widened object");
        model_matrix[0] = 0;
        model_matrix[1] = 2;
        model_matrix[4] = -0.5f;
        model_matrix[5] = 0;
        ++input.revision;
        check(shadow.update(input, depth, draw), "Object rotation updates depth");
        const auto rotated = visibility();
        check(rotated[358] > 0.99f && rotated[256] < 0.01f, "Rotated nonuniform object changes shadow shape");
        model_matrix = IDENTITY;

        const float tilted_plane[]{-5, -5, 2, 5, -5, 4, 5, 5, 4, -5, 5, 2};
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(tilted_plane), tilted_plane, GL_DYNAMIC_DRAW);
        settings.angular_diameter_deg = 12;
        check(shadow.set_settings(settings), "Tilted receiver settings");
        ++input.revision;
        check(shadow.update(input, depth, draw), "Tilted surface depth update");
        const auto coplanar = visibility(3.0f, true, 0.2f);
        check(*std::min_element(coplanar.begin(), coplanar.end()) > 0.95f,
              "Receiver plane correction avoids self-shadowing a sloped plane");
        upload_plane(40);
        settings.angular_diameter_deg = 0;
        shadow.set_settings(settings);

        // A visible-range change can submit no vertices without preserving the old occluder.
        ++input.revision;
        check(shadow.update(input, depth, [] {}), "Empty visible draw ranges clear the map");
        const auto empty = visibility();
        check(std::all_of(empty.begin(), empty.end(), [](float v) { return v == 1; }), "Hidden extrusion geometry no longer occludes");
        PCSSFrameInput bad = input;
        bad.casters        = {};
        check(!shadow.update(bad, depth, draw) && !shadow.is_ready(), "Invalid domain cannot reuse stale map");
        check(shadow.update(input, depth, draw), "Valid input recovers");
        const auto exception_state = state();
        ++input.revision;
        try {
            shadow.update(input, depth, [] { throw std::runtime_error("intentional draw failure"); });
            check(false, "Exception expected");
        } catch (const std::runtime_error&) {}
        check(state() == exception_state && !shadow.is_ready(), "Draw exception restores state and invalidates map");
        check(shadow.update(input, depth, draw), "Recovery after callback exception");

        // Execute the shipping plate receiver and verify presentation blending on real geometry.
        glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
        glViewport(0, 0, 512, 64);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        const auto plate_state     = state();
        auto       view_projection = IDENTITY;
        view_projection[0]         = 0.05f;
        view_projection[5]         = 0.5f;
        view_projection[10]        = 0.01f;
        shadow.render_plate(plate, view_projection, 0, [&]() {
            const GLint position = glGetAttribLocation(plate, "v_position");
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
            glEnableVertexAttribArray(position);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        });
        check(state() == plate_state, "Plate pass restores state");
        float center[4]{};
        glReadPixels(256, 32, 1, 1, GL_RGBA, GL_FLOAT, center);
        check(std::abs(center[0] - 0.45f) < 0.01f && center[3] == 1, "Plate shadows affect RGB without modifying destination alpha");
        no_errors("Plate receiver execution");

        glUseProgram(probe);
        glActiveTexture(GL_TEXTURE2);
        GLuint sentinel;
        glGenTextures(1, &sentinel);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, sentinel);
        glActiveTexture(GL_TEXTURE2);
        {
            PCSSReceiverScope scope(&shadow, probe);
            GLint             active;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
            check(active == GL_TEXTURE2, "Receiver scope does not redirect material textures");
        }
        glActiveTexture(GL_TEXTURE7);
        GLint binding;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        check(binding == static_cast<GLint>(sentinel), "Receiver scope restores borrowed texture binding");
        glDeleteTextures(1, &sentinel);
        shadow.shutdown_gl();
        check(!shadow.is_ready(), "Explicit GL shutdown clears readiness");
        check(shadow.update(input, depth, draw), "Reenable recreates depth resources");
        shadow.shutdown_gl();
        glDeleteFramebuffers(1, &output_fbo);
        glDeleteTextures(1, &output_texture);
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ebo);
        glDeleteVertexArrays(1, &vao);
        for (GLuint id : programs)
            glDeleteProgram(id);
        no_errors("Cleanup produces no GL errors");
        std::cout << "PCSS OpenGL: " << assertions << " assertions passed\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (window)
            glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
}
