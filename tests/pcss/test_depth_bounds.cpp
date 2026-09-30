#include "PCSSShadowRenderer.hpp"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PCSS_SHADER_DIRECTORY
#define PCSS_SHADER_DIRECTORY "resources/shaders/140"
#endif
#ifndef PCSS_REFERENCE_DIRECTORY
#define PCSS_REFERENCE_DIRECTORY "tests/pcss/reference"
#endif

using namespace Slic3r::GUI;

namespace {
unsigned checks = 0;
void check(bool condition, const char* message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
void no_error()
{
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        std::cerr << "OpenGL error " << error << '\n';
    check(error == GL_NO_ERROR, "No OpenGL errors");
}
void environment(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
std::string read_file(const std::string& path)
{
    std::ifstream stream(path);
    if (!stream)
        throw std::runtime_error("Missing shader: " + path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
GLuint compile(GLenum stage, const std::string& text)
{
    const GLuint shader = glCreateShader(stage);
    const char* source = text.c_str();
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[4096]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        throw std::runtime_error(log);
    }
    return shader;
}
GLuint program(const std::string& vertex_source, const std::string& fragment_source)
{
    const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
    const GLuint result = glCreateProgram();
    glAttachShader(result, vertex);
    glAttachShader(result, fragment);
    glLinkProgram(result);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(result, GL_LINK_STATUS, &ok);
    check(ok == GL_TRUE, "Link test program");
    return result;
}
std::vector<GLint> state()
{
    std::vector<GLint> result;
    for (GLenum parameter : {GL_DRAW_FRAMEBUFFER_BINDING, GL_READ_FRAMEBUFFER_BINDING, GL_CURRENT_PROGRAM,
                             GL_VERTEX_ARRAY_BINDING, GL_ARRAY_BUFFER_BINDING, GL_PIXEL_UNPACK_BUFFER_BINDING,
                             GL_ACTIVE_TEXTURE, GL_TEXTURE_BINDING_2D, GL_DEPTH_FUNC, GL_DEPTH_WRITEMASK}) {
        GLint value = 0;
        glGetIntegerv(parameter, &value);
        result.push_back(value);
    }
    GLint active = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    for (unsigned unit : {6u, 7u}) {
        glActiveTexture(GL_TEXTURE0 + unit);
        GLint value = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &value);
        result.push_back(value);
        if (GLEW_VERSION_3_3 || GLEW_ARB_sampler_objects) {
            glGetIntegerv(GL_SAMPLER_BINDING, &value);
            result.push_back(value);
        }
    }
    glActiveTexture(active);
    for (GLenum capability : {GL_SCISSOR_TEST, GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_STENCIL_TEST,
                              GL_RASTERIZER_DISCARD, GL_SAMPLE_ALPHA_TO_COVERAGE})
        result.push_back(glIsEnabled(capability));
    GLint values[4]{};
    glGetIntegerv(GL_VIEWPORT, values);
    result.insert(result.end(), values, values + 4);
    glGetIntegerv(GL_COLOR_WRITEMASK, values);
    result.insert(result.end(), values, values + 4);
    return result;
}
void quad(std::vector<float>& vertices, float x, float y, float width, float height, float z, float slope = 0.0f)
{
    for (unsigned corner : {0u, 1u, 2u, 0u, 2u, 3u}) {
        const float px = x + ((corner == 1 || corner == 2) ? width : 0.0f);
        const float py = y + (corner >= 2 ? height : 0.0f);
        vertices.insert(vertices.end(), {px, py, z + px * slope});
    }
}
} // namespace

int main(int argc, char** argv)
{
    GLFWwindow* window = nullptr;
    try {
        bool core = false, benchmark = false, profile = false;
        for (int i = 1; i < argc; ++i) {
            core |= std::string(argv[i]) == "--core";
            benchmark |= std::string(argv[i]) == "--benchmark";
            profile |= std::string(argv[i]) == "--profile";
        }
        environment("ORCA_PCSS_BOUNDS", "1");
        environment("ORCA_PCSS_FILTER", "pcss");
        environment("ORCA_PCSS_RECEIVERS", "all");
        environment("ORCA_PCSS_PROFILE", profile ? "1" : "0");
        if (profile)
            environment("ORCA_PCSS_PROFILE_LOG", "pcss-test-profile.log");
        check(glfwInit() == GLFW_TRUE, "Initialize GLFW");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, core ? 3 : 1);
        if (core)
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(64, 64, "PCSS conservative depth bounds", nullptr, nullptr);
        check(window != nullptr, "Create test context");
        glfwMakeContextCurrent(window);
        glewExperimental = GL_TRUE;
        check(glewInit() == GLEW_OK, "Initialize GLEW");
        while (glGetError() != GL_NO_ERROR) {} // GLEW may probe removed extension enums in a Core context.
        std::cout << "GL " << glGetString(GL_VERSION) << "; renderer=" << glGetString(GL_RENDERER) << '\n';
        const GLuint depth = program(R"(#version 140
in vec3 v_position;
uniform mat4 pcss_matrix;
void main() { gl_Position = pcss_matrix * vec4(v_position, 1.0); }
)", "#version 140\nvoid main() {}\n");
        const std::string probe_vertex = R"(#version 140
out vec2 uv;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
        const std::string probe_fragment = R"(
in vec2 uv;
uniform vec2 view_extent;
uniform vec2 view_offset;
uniform vec2 receiver_slope;
uniform float receiver_z;
out vec4 color;
void main() {
    vec2 xy = (uv - 0.5) * view_extent + view_offset;
    float value = pcss_visibility(vec3(xy, receiver_z + dot(xy, receiver_slope)));
    color = vec4(value, value, value, 1.0);
}
)";
        const GLuint reference = program(probe_vertex, "#version 140\n" +
            read_file(std::string(PCSS_REFERENCE_DIRECTORY) + "/pcss_before_depth_bounds.glsl") + probe_fragment);
        const GLuint accelerated = program(probe_vertex, "#version 140\n" +
            read_file(std::string(PCSS_SHADER_DIRECTORY) + "/pcss.glsl") + probe_fragment);
        GLuint vao = 0, vbo = 0, fbo = 0, image = 0, sentinels[2]{}, samplers[2]{};
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &image);
        glGenTextures(2, sentinels);
        if (GLEW_VERSION_3_3 || GLEW_ARB_sampler_objects) {
            glGenSamplers(2, samplers);
            for (unsigned i = 0; i < 2; ++i) {
                glSamplerParameteri(samplers[i], GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
                glBindSampler(6 + i, samplers[i]);
            }
        }
        glBindTexture(GL_TEXTURE_2D, image);
        const int width = benchmark ? 1024 : 192, height = benchmark ? 768 : 128;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Probe FBO complete");
        PCSSShadowRenderer shadow;
        PCSSSettings settings;
        PCSSFrameInput input;
        input.casters = {{-50, -50, -10}, {50, 50, 50}};
        input.receivers = {{-80, -80, -30}, {80, 80, 60}};
        input.to_light = {0, 0, 1};
        input.revision = 1;
        std::vector<float> vertices;
        unsigned depth_calls = 0;
        auto draw_depth = [&] {
            ++depth_calls;
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            const GLint position = glGetAttribLocation(depth, "v_position");
            glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
            glEnableVertexAttribArray(position);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / 3));
            glDisableVertexAttribArray(position);
        };
        auto upload = [&] {
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_DYNAMIC_DRAW);
            ++input.revision;
        };
        auto render = [&](GLuint shader, float z, float sx, float sy, float extent, float offset = 0.0f) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glViewport(0, 0, width, height);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glBindVertexArray(vao);
            glUseProgram(shader);
            glUniform2f(glGetUniformLocation(shader, "view_extent"), extent, extent);
            glUniform2f(glGetUniformLocation(shader, "view_offset"), offset, 0.0f);
            glUniform2f(glGetUniformLocation(shader, "receiver_slope"), sx, sy);
            glUniform1f(glGetUniformLocation(shader, "receiver_z"), z);
            PCSSReceiverScope scope(&shadow, shader);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        };
        auto pixels = [&](GLuint shader, float z, float sx, float sy, float extent, float offset) {
            render(shader, z, sx, sy, extent, offset);
            std::vector<float> result(width * height * 4);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, result.data());
            return result;
        };
        double worst_error = 0.0;
        auto compare = [&](float z, float sx, float sy, float extent, float offset = 0.0f) {
            const auto a = pixels(reference, z, sx, sy, extent, offset);
            const auto b = pixels(accelerated, z, sx, sy, extent, offset);
            double error = 0.0;
            for (size_t i = 0; i < a.size(); i += 4) {
                check(std::isfinite(b[i]) && b[i] >= 0.0f && b[i] <= 1.0f, "Finite accelerated visibility");
                error = std::max(error, static_cast<double>(std::abs(a[i] - b[i])));
            }
            worst_error = std::max(worst_error, error);
            if (error > 1e-6)
                std::cerr << "A/B error=" << error << " z=" << z << " slope=" << sx << ',' << sy << '\n';
            check(error <= 1e-6, "Hierarchy matches the fixed preceding shader");
            no_error();
        };
        for (unsigned resolution : {257u, 512u, 1024u}) {
            settings.resolution = resolution;
            check(shadow.set_settings(settings), "Set resolution");
            vertices.clear();
            quad(vertices, -40, -40, 80, 80, 20);
            upload();
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, sentinels[0]);
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, sentinels[1]);
            glActiveTexture(GL_TEXTURE3);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 1, 1);
            glDepthMask(GL_FALSE);
            glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
            glViewport(3, 7, 37, 19);
            const auto before = state();
            check(shadow.update(input, depth, draw_depth), "Depth and hierarchy generation");
            check(before == state(), "Restore FBO, viewport and both texture/sampler units");
            no_error();
            const auto generation = shadow.depth_generation();
            const auto calls = depth_calls;
            check(shadow.update(input, depth, draw_depth) && shadow.depth_generation() == generation && calls == depth_calls,
                  "Static input does not redraw depth");
            glUseProgram(accelerated);
            {
                PCSSReceiverScope scope(&shadow, accelerated);
                GLint enabled = 0;
                glGetUniformiv(accelerated, glGetUniformLocation(accelerated, "pcss_ranges_enabled"), &enabled);
                check(enabled == 1, "Bounds are actually used, not silently falling back");
                glActiveTexture(GL_TEXTURE7);
                std::vector<float> raw(resolution * resolution);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, raw.data());
                glActiveTexture(GL_TEXTURE6);
                GLint side = 0, levels = 0;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &side);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &levels);
                for (int level = 0; level <= levels; ++level) {
                    std::vector<float> actual(side * side * 4);
                    glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_FLOAT, actual.data());
                    const unsigned footprint = 4u << level;
                    for (unsigned y = 0; y < resolution; ++y) {
                        for (unsigned x = 0; x < resolution; ++x) {
                            const unsigned nx = x / footprint, ny = y / footprint;
                            const float* plane = &actual[4 * (ny * side + nx)];
                            const float cx = (nx + 0.5f) * footprint - 0.5f, cy = (ny + 0.5f) * footprint - 0.5f;
                            const float fitted = plane[0] + plane[1] * (x - cx) + plane[2] * (y - cy);
                            check(std::abs(raw[y * resolution + x] - fitted) <= plane[3] + 1e-7f,
                                  "Every plane/residual mip encloses every source texel");
                        }
                    }
                    side = std::max(1, side / 2);
                }
            }
            for (unsigned count : {1u, 8u, 16u, 64u}) {
                settings.blocker_samples = count;
                settings.filter_samples = count == 16 ? 32 : count;
                shadow.set_settings(settings);
                compare(0, 0, 0, 100);
                compare(19.99f, 0, 0, 100);
                compare(30, 0, 0, 100);
                compare(0, 0.2f, -0.1f, 100);
            }
            vertices.clear();
            quad(vertices, -45, -45, 90, 90, 10, 0.2f);
            upload();
            check(shadow.update(input, depth, draw_depth), "Coplanar depth update");
            compare(10, 0.2f, 0, 100);
            settings.blocker_samples = 16;
            settings.filter_samples = 32;
            settings.angular_diameter_deg = 12;
            shadow.set_settings(settings);
            vertices.clear();
            for (int i = 0; i < 12; ++i)
                quad(vertices, -45.0f + 8.0f * i, -40, i == 6 ? 0.12f : 2.0f, 80, 4.0f + i * 3);
            quad(vertices, -10, -8, 5, 16, 15, 0.3f);
            upload();
            check(shadow.update(input, depth, draw_depth), "Slivers and separated blockers update");
            compare(0, 0, 0, 110);
            compare(2, 0.3f, 0.2f, 110);
            compare(0, 0, 0, 250, 90);
            settings.use_depth_bounds = false;
            shadow.set_settings(settings);
            check(shadow.update(input, depth, draw_depth), "Fallback mode keeps depth");
            compare(0, 0, 0, 110);
            settings.use_depth_bounds = true;
            shadow.set_settings(settings);
            check(shadow.update(input, depth, draw_depth), "Reenable bounds on cached depth");
            ++input.revision;
            check(shadow.update(input, depth, [] {}), "Hidden geometry clears depth and bounds together");
            compare(0, 0, 0, 110);
        }
        std::mt19937 random(0x50435353u);
        std::uniform_real_distribution<float> position(-35.0f, 25.0f), size(0.1f, 18.0f), height_mm(1.0f, 30.0f);
        settings.resolution = 513;
        settings.angular_diameter_deg = 7;
        shadow.set_settings(settings);
        for (unsigned scene = 0; scene < 16; ++scene) {
            vertices.clear();
            for (unsigned object = 0; object < 12; ++object) {
                const float x = position(random), y = position(random);
                const float w = size(random), h = size(random), z = height_mm(random);
                quad(vertices, x, y, w, h, z, 0.1f);
            }
            upload();
            check(shadow.update(input, depth, draw_depth), "Randomized scene update");
            compare(0, 0.11f, -0.07f, 100);
        }
        std::cout << "Previous shader vs hierarchy: worst visibility error=" << worst_error << '\n';
        if (profile) {
            for (unsigned i = 0; i < 100; ++i)
                render(accelerated, 0, 0, 0, 110);
            for (unsigned i = 0; i < 8; ++i) {
                compare(0, 0, 0, 110);
                shadow.update(input, depth, draw_depth);
            }
            no_error(); // A full timer pool must drop samples instead of stalling or reusing pending query IDs.
        }
        if (benchmark) {
            settings = PCSSSettings{};
            shadow.set_settings(settings);
            vertices.clear();
            quad(vertices, -45, -45, 90, 90, 20);
            upload();
            shadow.update(input, depth, draw_depth);
            auto measure = [&](const auto& work) {
                for (int i = 0; i < 3; ++i)
                    work();
                glFinish(); // Explicit test-only completion wait. Never used in the shipping renderer.
                std::vector<double> times;
                for (int round = 0; round < 7; ++round) {
                    const auto start = std::chrono::steady_clock::now();
                    for (int repeat = 0; repeat < 3; ++repeat)
                        work();
                    glFinish();
                    times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 3);
                }
                std::sort(times.begin(), times.end());
                return times[times.size() / 2];
            };
            for (float z : {0.0f, 30.0f}) {
                const auto previous = measure([&] { render(reference, z, 0, 0, 20); });
                const auto current = measure([&] { render(accelerated, z, 0, 0, 20); });
                std::cout << "BENCH " << (z == 0 ? "umbra" : "lit-inside-bounds") << " previous_ms=" << previous
                          << " accelerated_ms=" << current << '\n';
            }
            vertices.clear();
            quad(vertices, -45, -45, 90, 90, 10, 0.2f);
            upload();
            shadow.update(input, depth, draw_depth);
            for (GLuint shader : {reference, accelerated})
                std::cout << "BENCH coplanar-sloped accelerated=" << (shader == accelerated)
                          << " ms=" << measure([&] { render(shader, 10, 0.2f, 0, 40); }) << '\n';
            for (bool use_bounds : {false, true}) {
                settings.use_depth_bounds = use_bounds;
                shadow.set_settings(settings);
                const auto elapsed = measure([&] { ++input.revision; shadow.update(input, depth, draw_depth); });
                std::cout << "BENCH dirty-depth+summary bounds=" << use_bounds << " ms=" << elapsed << '\n';
            }
            vertices.clear();
            for (int i = 0; i < 30; ++i)
                quad(vertices, -45 + i * 3.0f, -45, 1, 90, 20);
            upload();
            shadow.update(input, depth, draw_depth);
            for (GLuint shader : {reference, accelerated})
                std::cout << "BENCH mixed-penumbra accelerated=" << (shader == accelerated)
                          << " ms=" << measure([&] { render(shader, 0, 0, 0, 70); }) << '\n';
        }
        shadow.shutdown_gl();
        environment("ORCA_PCSS_RECEIVERS", "none");
        vertices.clear();
        quad(vertices, -45, -45, 90, 90, 20);
        upload();
        check(shadow.update(input, depth, draw_depth), "Depth-only diagnostic generates geometry");
        const auto lit = pixels(accelerated, 0, 0, 0, 20, 0);
        check(std::all_of(lit.begin(), lit.end(), [](float value) { return value == 1.0f; }), "Depth-only disables all receivers");
        shadow.shutdown_gl();
        environment("ORCA_PCSS_RECEIVERS", "all");
        environment("ORCA_PCSS_FILTER", "hard");
        check(shadow.update(input, depth, draw_depth), "Hard-shadow diagnostic generates geometry");
        const auto hard = pixels(accelerated, 0, 0, 0, 100, 0);
        check(std::all_of(hard.begin(), hard.end(), [](float value) { return value == 0.0f || value == 1.0f; }), "Hard comparison is binary");
        shadow.shutdown_gl();
        environment("ORCA_PCSS_FILTER", "pcss");
        check(!shadow.is_ready(), "Shutdown invalidates shadow data");
        check(shadow.update(input, depth, draw_depth), "Reenable recreates resources");
        shadow.shutdown_gl();
        no_error();
        glDeleteProgram(depth);
        glDeleteProgram(reference);
        glDeleteProgram(accelerated);
        glDeleteTextures(2, sentinels);
        if (samplers[0] != 0)
            glDeleteSamplers(2, samplers);
        glDeleteTextures(1, &image);
        glDeleteFramebuffers(1, &fbo);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        no_error();
        glfwDestroyWindow(window);
        glfwTerminate();
        std::cout << "Depth-bounds checks passed: " << checks << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (window)
            glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
}
