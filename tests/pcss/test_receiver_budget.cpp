#include "PCSSShadowRenderer.hpp"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
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
unsigned                    checks = 0;
constexpr unsigned          SIZE   = 512;
const std::array<float, 16> IDENTITY{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
void                        check(bool ok, const char* what)
{
    ++checks;
    if (!ok)
        throw std::runtime_error(what);
}
void no_error() { check(glGetError() == GL_NO_ERROR, "OpenGL error"); }
void environment(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
std::string read(const std::string& root, const std::string& name)
{
    std::ifstream file(root + "/" + name);
    if (!file)
        throw std::runtime_error("Missing test source " + name);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::string common(std::string text, bool fragment)
{
    const auto end = text.find('\n');
    check(end != std::string::npos, "GLSL version line");
    text.insert(end + 1, std::string("#define ENABLE_PCSS\n") + (fragment ? read(PCSS_SHADER_DIRECTORY, "pcss.glsl") : ""));
    return text;
}
GLuint program(const std::string& vs, const std::string& fs)
{
    GLuint      stages[2]{glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER)};
    const char* texts[]{vs.c_str(), fs.c_str()};
    for (unsigned i = 0; i < 2; ++i) {
        glShaderSource(stages[i], 1, &texts[i], nullptr);
        glCompileShader(stages[i]);
        GLint ok = GL_FALSE;
        glGetShaderiv(stages[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[8192]{};
            glGetShaderInfoLog(stages[i], sizeof(log), nullptr, log);
            throw std::runtime_error(log);
        }
    }
    GLuint id = glCreateProgram();
    for (GLuint shader : stages)
        glAttachShader(id, shader);
    glLinkProgram(id);
    GLint ok = GL_FALSE;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[8192]{};
        glGetProgramInfoLog(id, sizeof(log), nullptr, log);
        throw std::runtime_error(log);
    }
    for (GLuint shader : stages)
        glDeleteShader(shader);
    return id;
}
int uniform(GLuint p, const char* name)
{
    GLint       value    = -1;
    const GLint location = glGetUniformLocation(p, name);
    check(location >= 0, "Active receiver uniform");
    glGetUniformiv(p, location, &value);
    return value;
}
std::array<GLboolean, 8> clip_state()
{
    std::array<GLboolean, 8> values{};
    for (unsigned i = 0; i < 8; ++i)
        values[i] = glIsEnabled(GL_CLIP_DISTANCE0 + i);
    return values;
}
} // namespace
int main(int argc, char** argv)
{
    GLFWwindow* window = nullptr;
    try {
        bool core = false, benchmark = false;
        for (int i = 1; i < argc; ++i) {
            core |= std::string(argv[i]) == "--core";
            benchmark |= std::string(argv[i]) == "--benchmark";
        }
        environment("ORCA_PCSS_PROFILE", "1");
        environment("ORCA_PCSS_PROFILE_GEOMETRY", "1");
        environment("ORCA_PCSS_PROFILE_LOG", "pcss-budget-profile.log");
        environment("ORCA_PCSS_BOUNDS", "");
        environment("ORCA_PCSS_FILTER", "pcss");
        environment("ORCA_PCSS_RECEIVERS", "all");
        environment("ORCA_PCSS_MODEL_QUALITY", "balanced");
        std::remove("pcss-budget-profile.log");
        check(glfwInit() == GLFW_TRUE, "GLFW init");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, core ? 3 : 1);
        if (core)
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(SIZE, 64, "PCSS receiver budget", nullptr, nullptr);
        check(window != nullptr, "GLFW context");
        glfwMakeContextCurrent(window);
        glewExperimental = GL_TRUE;
        check(glewInit() == GLEW_OK, "GLEW init");
        while (glGetError() != GL_NO_ERROR) {}
        std::cout << "GL " << glGetString(GL_VERSION) << "; renderer=" << glGetString(GL_RENDERER) << '\n';
        std::vector<GLuint> programs;
        const auto          keep = [&](GLuint p) {
            programs.push_back(p);
            return p;
        };
        const auto shipping = [&](const char* name) {
            return keep(program(common(read(PCSS_SHADER_DIRECTORY, std::string(name) + ".vs"), false),
                                common(read(PCSS_SHADER_DIRECTORY, std::string(name) + ".fs"), true)));
        };
        const GLuint depth  = keep(program(read(PCSS_SHADER_DIRECTORY, "pcss_depth.vs"), read(PCSS_SHADER_DIRECTORY, "pcss_depth.fs")));
        const GLuint legacy = keep(
            program(read(PCSS_REFERENCE_DIRECTORY, "pcss_depth_fragment.vs"), read(PCSS_REFERENCE_DIRECTORY, "pcss_depth_fragment.fs")));
        const GLuint mesh = shipping("gouraud"), gcode = shipping("gouraud_light");
        const GLuint probe = keep(program(R"(#version 140
out vec2 uv;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0);
})",
                                          common(R"(#version 140
in vec2 uv;
struct PrintVolumeDetection { int type; };
uniform PrintVolumeDetection print_volume;
uniform float receiver_z;
out vec4 color;
void main() {
    float v = pcss_visibility(vec3((uv - 0.5) * vec2(40.0, 4.0), receiver_z));
    if (print_volume.type == 99) v = 0.0; // Same active marker as the shipping mesh material.
    color = vec4(v, v, v, 1.0);
})",
                                                 true)));
        GLuint       vao, vbo, fbo, image;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        std::vector<float> vertices;
        const auto         upload = [&](float z, unsigned copies = 1) {
            vertices.clear();
            for (unsigned i = 0; i < copies; ++i) {
                const float h = z + static_cast<float>(i) * 0.001f;
                const float quad[]{-5, -5, h, 5, -5, h, 5, 5, h, -5, -5, h, 5, 5, h, -5, 5, h};
                vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
            }
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_DYNAMIC_DRAW);
        };
        upload(20);
        glGenTextures(1, &image);
        glBindTexture(GL_TEXTURE_2D, image);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, SIZE, 64, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Probe FBO");
        PCSSFrameInput input;
        input.casters   = {{-5, -5, 0}, {5, 5, 45}};
        input.receivers = {{-20, -20, 0}, {20, 20, 40}};
        input.revision  = 1;
        PCSSShadowRenderer shadow, old_depth;
        PCSSSettings       settings;
        check(!settings.use_depth_bounds, "Bounds default off without an opt-in");
        settings.resolution           = SIZE;
        settings.angular_diameter_deg = 12;
        check(shadow.set_settings(settings) && old_depth.set_settings(settings), "Settings accepted");
        auto                 world = IDENTITY;
        std::array<float, 4> clip{0, 0, 0, 1};
        std::array<float, 2> z_range{-1000, 1000};
        unsigned             draws = 0;
        const auto           draw  = [&](GLuint p) {
            ++draws;
            glUniformMatrix4fv(glGetUniformLocation(p, "volume_world_matrix"), 1, GL_FALSE, world.data());
            glUniform4fv(glGetUniformLocation(p, "clipping_plane"), 1, clip.data());
            glUniform2fv(glGetUniformLocation(p, "z_range"), 1, z_range.data());
            const GLint position = glGetAttribLocation(p, "v_position");
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
            glEnableVertexAttribArray(position);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / 3));
            glDisableVertexAttribArray(position);
        };
        const auto update = [&](PCSSShadowRenderer& s, GLuint p) { check(s.update(input, p, [&] { draw(p); }), "Depth update"); };
        update(shadow, depth);
        const auto budget = [&](GLuint p, bool plate, unsigned b, unsigned f) {
            glUseProgram(p);
            PCSSReceiverScope scope(&shadow, p, plate);
            check(uniform(p, "pcss_blocker_samples") == static_cast<int>(b), "Receiver blocker budget");
            check(uniform(p, "pcss_filter_samples") == static_cast<int>(f), "Receiver filter budget");
            const auto        expected = pcss::make_disk_samples(b);
            float             xy[2]{};
            const std::string name = "pcss_blocker_disk[" + std::to_string(b - 1) + "]";
            glGetUniformfv(p, glGetUniformLocation(p, name.c_str()), xy);
            check(std::abs(xy[0] - expected[2 * (b - 1)]) < 1e-6f && std::abs(xy[1] - expected[2 * (b - 1) + 1]) < 1e-6f,
                  "Actual-count disk spans the full support");
        };
        budget(mesh, false, 8, 16);
        budget(gcode, false, 16, 32);
        budget(probe, true, 16, 32);
        const auto generation = shadow.depth_generation();
        update(shadow, depth);
        check(shadow.depth_generation() == generation, "Static depth cache retained");
        settings.model_blocker_samples = 16;
        settings.model_filter_samples  = 32;
        check(shadow.set_settings(settings), "Reference model budget via API");
        update(shadow, depth);
        check(shadow.depth_generation() == generation, "Changing receiver quality does not redraw depth");
        budget(mesh, false, 16, 32);
        budget(gcode, false, 16, 32);
        settings.model_blocker_samples = 8;
        settings.model_filter_samples  = 16;
        settings.blocker_samples       = 3;
        settings.filter_samples        = 7;
        check(shadow.set_settings(settings), "Base cap");
        budget(mesh, false, 3, 7);
        settings.blocker_samples = 16;
        settings.filter_samples  = 32;
        shadow.set_settings(settings);
        PCSSSettings invalid         = settings;
        invalid.model_filter_samples = 0;
        check(!shadow.set_settings(invalid), "Reject invalid model samples");
        budget(mesh, false, 8, 16);
        const auto depth_pixels = [&](PCSSShadowRenderer& s) {
            glUseProgram(probe);
            std::vector<float> out(SIZE * SIZE);
            PCSSReceiverScope  scope(&s, probe, true);
            glActiveTexture(GL_TEXTURE7);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, out.data());
            return out;
        };
        for (unsigned test = 0; test < 7; ++test) {
            clip    = {0, 0, 0, 1};
            z_range = {-1000, 1000};
            world   = IDENTITY;
            if (test == 1)
                clip = {1, 0, 0, 0};
            if (test == 2)
                clip = {1, 1, 0, -1};
            if (test == 3)
                z_range = {21, 40};
            if (test == 4)
                z_range = {-1000, 19};
            if (test == 5) {
                world[0] = -1;
                clip     = {1, 0, 0, 0};
            }
            if (test == 6)
                z_range = {-3.402823466e38f, 3.402823466e38f};
            ++input.revision;
            glEnable(GL_CLIP_DISTANCE0 + 5);
            glEnable(GL_CLIP_DISTANCE0 + 7);
            const auto before = clip_state();
            update(shadow, depth);
            check(before == clip_state(), "Vertex depth restores all host clip enables");
            update(old_depth, legacy);
            check(before == clip_state(), "Legacy depth restores clip enables");
            const auto a = depth_pixels(shadow), b = depth_pixels(old_depth);
            unsigned   differences = 0;
            for (unsigned i = 0; i < a.size(); ++i)
                if (std::abs(a[i] - b[i]) > 1e-5f)
                    ++differences;
            // Geometric clipping and fragment rejection can differ on a one-pixel silhouette boundary.
            check(differences <= ((test == 1 || test == 2 || test == 5) ? 4 * SIZE : 0),
                  "Clipped depth matches reference except raster edges");
            std::cout << "Depth clip case " << test << " differing pixels=" << differences << '\n';
            no_error();
        }
        for (unsigned i = 0; i < 8; ++i)
            glDisable(GL_CLIP_DISTANCE0 + i);
        clip                  = {0, 0, 0, 1};
        z_range               = {-1000, 1000};
        world                 = IDENTITY;
        const auto visibility = [&](float receiver_z, bool plate = false) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, SIZE, 64);
            glBindVertexArray(vao);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glUseProgram(probe);
            glUniform1f(glGetUniformLocation(probe, "receiver_z"), receiver_z);
            {
                PCSSReceiverScope scope(&shadow, probe, plate);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            std::vector<float> row(SIZE * 4);
            glReadPixels(0, 32, SIZE, 1, GL_RGBA, GL_FLOAT, row.data());
            for (float v : row)
                check(std::isfinite(v) && v >= 0 && v <= 1, "Finite PCSS with reduced budget");
            return row;
        };
        const auto fractional = [&](const std::vector<float>& row) {
            unsigned n = 0;
            for (unsigned i = 0; i < SIZE; ++i)
                if (row[4 * i] > 0.001f && row[4 * i] < 0.999f)
                    ++n;
            return n;
        };
        upload(10);
        ++input.revision;
        update(shadow, depth);
        const auto low = visibility(0);
        upload(40);
        ++input.revision;
        update(shadow, depth);
        const auto high = visibility(0);
        check(high[SIZE * 2] < 0.01f && high[0] > 0.99f, "Retain shadow center and lit exterior");
        check(fractional(high) > fractional(low), "Reduced PCSS still widens with separation");
        check(fractional(visibility(39.9f)) < fractional(low), "Reduced PCSS contact hardening");
        const auto full = visibility(0, true);
        double     mean = 0, max_error = 0;
        for (unsigned i = 0; i < SIZE; ++i) {
            double e = std::abs(full[4 * i] - high[4 * i]);
            mean += e / SIZE;
            max_error = std::max(max_error, e);
        }
        std::cout << "8/16 versus 16/32 visibility MAE=" << mean << " max=" << max_error
                  << " (quality tradeoff, not an equivalence test)\n";
        settings.use_depth_bounds = true;
        shadow.set_settings(settings);
        const auto cached = shadow.depth_generation();
        glEnable(GL_CLIP_DISTANCE0 + 6);
        const auto before = clip_state();
        update(shadow, depth);
        check(before == clip_state(), "Bounds update restores inherited clipping");
        check(shadow.depth_generation() == cached, "Opt-in bounds build reuses depth");
        glDisable(GL_CLIP_DISTANCE0 + 6);
        glUseProgram(probe);
        {
            PCSSReceiverScope scope(&shadow, probe);
            check(uniform(probe, "pcss_ranges_enabled") == 1, "Bounds opt-in active");
        }
        settings.use_depth_bounds = false;
        shadow.set_settings(settings);
        glUseProgram(probe);
        {
            PCSSReceiverScope scope(&shadow, probe);
            check(uniform(probe, "pcss_ranges_enabled") == 0, "Bounds disabled again");
        }
        const auto n = shadow.depth_generation();
        update(shadow, legacy);
        check(shadow.depth_generation() == n + 1, "Program change invalidates depth");
        update(shadow, depth);
        GLuint outer;
        glGenQueries(1, &outer);
        glBeginQuery(GL_PRIMITIVES_GENERATED, outer);
        ++input.revision;
        update(shadow, depth);
        GLint active = 0;
        glGetQueryiv(GL_PRIMITIVES_GENERATED, GL_CURRENT_QUERY, &active);
        check(active == static_cast<GLint>(outer), "Profiler never interrupts a host primitive query");
        glEndQuery(GL_PRIMITIVES_GENERATED);
        glDeleteQueries(1, &outer);
        // Test-only completion allows deterministic inspection of delayed query results. No production wait is introduced.
        glFinish();
        for (unsigned i = 0; i < 4; ++i)
            update(shadow, depth);
        input.to_light = {0.1, 0, 1};
        update(shadow, depth);
        input.to_light = {0, 0, 1};
        update(shadow, depth);
        glFinish();
        for (unsigned i = 0; i < 4; ++i)
            update(shadow, depth);
        if (benchmark) {
            shadow.shutdown_gl();
            old_depth.shutdown_gl();
            environment("ORCA_PCSS_PROFILE", "0"); // Exclude diagnostic-query overhead from timed comparisons.
            settings.resolution = 2048;
            shadow.set_settings(settings);
            old_depth.set_settings(settings);
            update(shadow, depth);
            update(old_depth, legacy);
            glBindTexture(GL_TEXTURE_2D, image);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1024, 768, 0, GL_RGBA, GL_FLOAT, nullptr);
            const auto bench = [&](const char* label, const std::function<void()>& body) {
                for (int i = 0; i < 3; ++i)
                    body();
                glFinish();
                std::vector<double> times;
                for (int k = 0; k < 5; ++k) {
                    auto start = std::chrono::steady_clock::now();
                    for (int i = 0; i < 4; ++i)
                        body();
                    glFinish();
                    times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 4);
                }
                std::sort(times.begin(), times.end());
                std::cout << "BENCH " << label << " ms=" << times[2] << '\n';
            };
            // Same receiver program and output; only the configured sample budget changes.
            const auto receiver_draw = [&] {
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glViewport(0, 0, 1024, 768);
                glBindVertexArray(vao);
                glDisable(GL_DEPTH_TEST);
                glUseProgram(probe);
                glUniform1f(glGetUniformLocation(probe, "receiver_z"), 0);
                PCSSReceiverScope scope(&shadow, probe);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            };
            settings.model_blocker_samples = 16;
            settings.model_filter_samples  = 32;
            shadow.set_settings(settings);
            bench("receiver_16_32", receiver_draw);
            settings.model_blocker_samples = 8;
            settings.model_filter_samples  = 16;
            shadow.set_settings(settings);
            bench("receiver_8_16", receiver_draw);
            upload(20, 64);
            bench("depth_fragment", [&] {
                ++input.revision;
                update(old_depth, legacy);
            });
            bench("depth_vertex", [&] {
                ++input.revision;
                update(shadow, depth);
            });
            std::cout << "Benchmark excludes Orca GUI and production-sized geometry. Driver-dependent; no FPS guarantee.\n";
        }
        shadow.shutdown_gl();
        old_depth.shutdown_gl();
        no_error();
        environment("ORCA_PCSS_MODEL_QUALITY", "reference");
        PCSSShadowRenderer reference;
        reference.set_settings(PCSSSettings{});
        update(reference, depth);
        glUseProgram(mesh);
        {
            PCSSReceiverScope scope(&reference, mesh);
            check(uniform(mesh, "pcss_blocker_samples") == 16, "Reference env restored search");
            check(uniform(mesh, "pcss_filter_samples") == 32, "Reference env restored filtering");
        }
        reference.shutdown_gl();
        const auto log = read(".", "pcss-budget-profile.log");
        for (const char* token : {"revision=receiver-budget-v3", "model_samples=8/16", "bounds=off", "depth_clip=vertex",
                                  "cache_hits=", "redraw_revision=", "redraw_light=", "redraw_program="})
            check(log.find(token) != std::string::npos, "Updated diagnostic metadata");
        if (GLEW_VERSION_3_3 || GLEW_ARB_timer_query)
            check(log.find("depth_primitives=2.000") != std::string::npos, "Async primitive result collected");
        for (GLuint p : programs)
            glDeleteProgram(p);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glDeleteTextures(1, &image);
        glDeleteFramebuffers(1, &fbo);
        no_error();
        std::cout << "PCSS receiver budget: " << checks << " checks passed\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        if (window)
            glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
}
