// Appended after the actual renderer implementation by test_pcss.py.
using namespace Slic3r;
using namespace Slic3r::GUI;

void smoke(OpenGLManager::EFramebufferType type)
{
    OpenGLManager::type = type;
    GLCanvas3D canvas;
    Camera camera;
    GLVolume volume;
    canvas.m_volumes.volumes.push_back(&volume);
    glViewport(3, 5, 90, 70);
    glDepthFunc(GL_GREATER);
    glClearDepth(0.25);
    glDepthRange(0.2, 0.8);
    glDepthMask(GL_FALSE);
    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    glEnable(GL_STENCIL_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    GLuint fbo[2]{};
    glGenFramebuffers(2, fbo);
    if (type == OpenGLManager::EFramebufferType::Arb) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[0]);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[1]);
    } else {
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[0]);
    }
    assert(canvas.RenderShadowMap(camera));
    assert(volume.model.draws == 1);
    GLint state[4]{};
    glGetIntegerv(GL_VIEWPORT, state);
    assert(state[0] == 3 && state[1] == 5 && state[2] == 90 && state[3] == 70);
    glGetIntegerv(GL_DEPTH_FUNC, state); assert(state[0] == GL_GREATER);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, state); assert(GLuint(state[0]) == fbo[0]);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, state);
    assert(GLuint(state[0]) == (type == OpenGLManager::EFramebufferType::Arb ? fbo[1] : fbo[0]));
    GLdouble clear{}; glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clear); assert(clear == 0.25);
    GLdouble range[2]{}; glGetDoublev(GL_DEPTH_RANGE, range);
    assert(std::abs(range[0] - 0.2) < 1.e-6 && std::abs(range[1] - 0.8) < 1.e-6);
    GLboolean mask[4]{}; glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    assert(!mask[0] && mask[1] && !mask[2] && mask[3]);
    glGetBooleanv(GL_DEPTH_WRITEMASK, mask); assert(!mask[0]);
    assert(glIsEnabled(GL_SCISSOR_TEST) && glIsEnabled(GL_STENCIL_TEST));
    assert(glIsEnabled(GL_POLYGON_OFFSET_FILL) && glIsEnabled(GL_BLEND) && !glIsEnabled(GL_CULL_FACE));
    glGetIntegerv(GL_POLYGON_MODE, state); assert(state[0] == GL_LINE && state[1] == GL_LINE);
    assert(glGetError() == GL_NO_ERROR);

    // Unchanged scene and translated camera: no geometry resubmission.
    assert(canvas.RenderShadowMap(camera)); assert(volume.model.draws == 1);
    camera.view.translation().x() = 10;
    assert(canvas.RenderShadowMap(camera)); assert(volume.model.draws == 1);
    camera.view.linear() = Eigen::AngleAxisd(0.2, Vec3d::UnitZ()).toRotationMatrix();
    assert(canvas.RenderShadowMap(camera)); assert(volume.model.draws == 2);
    volume.world.translation().x() = 10;
    assert(canvas.RenderShadowMap(camera)); assert(volume.model.draws == 3);
    ++volume.model.revision;
    assert(canvas.RenderShadowMap(camera)); assert(volume.model.draws == 4);
    canvas.m_volumes.clip[3] = 2;
    assert(canvas.RenderShadowMap(camera)); assert(volume.model.draws == 5);
    canvas.m_volumes.z_range[1] = 60;
    assert(canvas.RenderShadowMap(camera)); assert(volume.model.draws == 6);
    volume.tverts_range.second = 18;
    assert(canvas.RenderShadowMap(camera)); assert(volume.model.draws == 7);
    volume.tverts_range.second = static_cast<size_t>(-1);
    volume.m_modelMiddle = std::make_shared<GLModel>();
    volume.m_modelMiddle->disabled = true;
    assert(canvas.RenderShadowMap(camera)); assert(volume.model.draws == 8);
    volume.pending_middle = true;
    assert(canvas.RenderShadowMap(camera)); assert(volume.m_modelMiddle->draws == 1);
    assert(canvas.RenderShadowMap(camera)); assert(volume.m_modelMiddle->draws == 1);
    GLVolume second;
    second.world.translation().x() = 60;
    canvas.m_volumes.volumes.push_back(&second);
    assert(canvas.RenderShadowMap(camera)); assert(second.model.draws == 1);
    second.is_active = false;
    assert(canvas.RenderShadowMap(camera)); assert(second.model.draws == 1);
    canvas.m_volumes.volumes.pop_back();
    assert(canvas.RenderShadowMap(camera));
    const auto before_toggle = volume.m_modelMiddle->draws;
    wxGetApp().config.enabled = false;
    assert(!canvas.RenderShadowMap(camera)); assert(!canvas.m_shadowMap.valid);
    wxGetApp().config.enabled = true;
    assert(canvas.RenderShadowMap(camera)); assert(volume.m_modelMiddle->draws == before_toggle + 1);

    GLuint textures[2]{}, samplers[2]{};
    glGenTextures(2, textures); glGenSamplers(2, samplers);
    for (GLuint i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE1 + i); glBindTexture(GL_TEXTURE_2D, textures[i]); glBindSampler(1 + i, samplers[i]);
    }
    glActiveTexture(GL_TEXTURE4);
    canvas.BindShadowTextures();
    glGetIntegerv(GL_ACTIVE_TEXTURE, state); assert(state[0] == GL_TEXTURE0);
    glActiveTexture(GL_TEXTURE2); glGetIntegerv(GL_SAMPLER_BINDING, state);
    assert(GLuint(state[0]) == canvas.m_shadowMap.comparisonSampler);
    canvas.UnbindShadowTextures();
    glGetIntegerv(GL_ACTIVE_TEXTURE, state); assert(state[0] == GL_TEXTURE4);
    for (GLuint i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE1 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, state); assert(GLuint(state[0]) == textures[i]);
        glGetIntegerv(GL_SAMPLER_BINDING, state); assert(GLuint(state[0]) == samplers[i]);
    }
    canvas.ReleaseShadowMapResources();
    glDeleteTextures(2, textures); glDeleteSamplers(2, samplers); glDeleteFramebuffers(2, fbo);
    assert(glGetError() == GL_NO_ERROR);
    std::cout << (type == OpenGLManager::EFramebufferType::Arb ? "ARB" : "EXT")
              << ": cache invalidation, LOD hand-off, allocation/state/texture restoration passed\n";
}
int main()
{
    assert(glfwInit());
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(128, 128, "pcss smoke", nullptr, nullptr);
    assert(window); glfwMakeContextCurrent(window);
    assert(glewInit() == GLEW_OK);
    while (glGetError() != GL_NO_ERROR) {}
    smoke(OpenGLManager::EFramebufferType::Arb);
    smoke(OpenGLManager::EFramebufferType::Ext);
    glfwDestroyWindow(window); glfwTerminate();
}
