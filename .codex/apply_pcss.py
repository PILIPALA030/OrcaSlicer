#!/usr/bin/env python3
"""Apply exact anchored edits to pinned 27a56130; staging helper, not product code."""
from pathlib import Path
import re

ROOT = Path.cwd()
KERNEL = (Path(__file__).parent / 'kernel.glsl').read_text()

def edit(path, callback):
    p = ROOT / path
    old = p.read_text(encoding='utf-8')
    new = callback(old)
    assert new != old, f'No change: {path}'
    p.write_text(new, encoding='utf-8', newline='\n')

def replace_once(text, old, new):
    assert text.count(old) == 1, f'Expected one anchor, found {text.count(old)}: {old[:100]!r}'
    return text.replace(old, new, 1)

def change_canvas(s):
    for line in ('constexpr unsigned int SHADOW_MAP_SIZE = 512;\n',
                 'constexpr float SHADOW_LIGHT_SIZE = 0.035f;\n',
                 'constexpr float SHADOW_BIAS = 0.0015f;\n'):
        assert s.count(line) == 1, line
        s, count = re.subn(r'^[ \t]*' + re.escape(line), '', s, flags=re.MULTILINE)
        assert count == 1, line
    start = s.index('void GLCanvas3D::BindShadowUniforms(')
    end = s.index('GLCanvas3D::ESelectionHighlightMode GLCanvas3D::ResolveSelectionHighlightMode()', start)
    s = s[:start] + s[end:]
    start = s.index('    GLint previousSceneActiveTexture = GL_TEXTURE0;')
    end = s.index('    const ESelectionHighlightMode highlightMode = ResolveSelectionHighlightMode();', start)
    s = s[:start] + '    if (shadowMapReady)\n        BindShadowTextures();\n\n' + s[end:]
    s = replace_once(s, '''        glsafe(::glActiveTexture(GL_TEXTURE1));
        glsafe(::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture1Binding)));
        glsafe(::glActiveTexture(static_cast<GLenum>(previousSceneActiveTexture)));''',
        '        UnbindShadowTextures();')
    return s

FIELDS = '''    struct ShadowCasterState
    {
        GLModel* model{ nullptr };
        std::uint64_t geometryRevision{ 0 };
        Transform3d worldMatrix{ Transform3d::Identity() };
        std::pair<size_t, size_t> range{ 0, 0 };
    };

    struct ShadowMapResources
    {
        unsigned int framebuffer{ 0 };
        unsigned int depthTexture{ 0 };
        unsigned int colorTexture{ 0 };
        unsigned int rawSampler{ 0 };
        unsigned int comparisonSampler{ 0 };
        unsigned int size{ 0 };
        Transform3d lightViewProjection{ Transform3d::Identity() };
        std::array<float, 2> penumbraScale{ 0.0f, 0.0f };
        float depthBias{ 0.0f };
        std::vector<ShadowCasterState> cachedCasters;
        std::array<float, 2> cachedZRange{};
        std::array<double, 4> cachedClippingPlane{};
        std::array<int, 2> sceneTextureBindings{};
        std::array<int, 2> sceneSamplerBindings{};
        int sceneActiveTexture{ 0 };
        bool texturesBound{ false };
        bool allocationFailed{ false };
        bool valid{ false };
    };

    ShadowMapResources m_shadowMap;'''

def change_header(s):
    start = s.index('    struct ShadowMapResources\n')
    end = s.index('    ShadowMapResources m_shadowMap;', start) + len('    ShadowMapResources m_shadowMap;')
    s = s[:start] + FIELDS + s[end:]
    return replace_once(s, '    bool RenderShadowMap(const Camera& camera);', '''    bool RenderShadowMap(const Camera& camera);
    /** @brief Saves/binds, then restores raw-depth and comparison sampling state. */
    void BindShadowTextures();
    void UnbindShadowTextures();''')

edit('src/slic3r/GUI/GLCanvas3D.cpp', change_canvas)
edit('src/slic3r/GUI/GLCanvas3D.hpp', change_header)
edit('src/slic3r/CMakeLists.txt', lambda s: replace_once(s, '    GUI/GLCanvas3D.cpp\n',
                                                      '    GUI/GLCanvas3D.cpp\n    GUI/GLCanvas3DShadow.cpp\n'))

def model_header(s):
    s = replace_once(s, '#include <string>\n', '#include <string>\n#include <cstdint>\n')
    s = replace_once(s, '        RenderData m_render_data;', '''        RenderData m_render_data;
        // Unique across rebuilds/address reuse. Read asynchronous LOD revisions
        // only after the existing ready/acquire hand-off.
        std::uint64_t m_geometry_revision{ 0 };''')
    return replace_once(s, '        size_t vertices_count() const { return m_render_data.vertices_count > 0 ?',
        '''        std::uint64_t geometry_revision() const { return m_geometry_revision; }

        size_t vertices_count() const { return m_render_data.vertices_count > 0 ?''')

edit('src/slic3r/GUI/GLModel.hpp', model_header)

def model_cpp(s):
    anchor = 'void GLModel::init_from(Geometry&& data)'
    s = replace_once(s, anchor, '''namespace {
std::atomic<std::uint64_t> next_geometry_revision{ 1 };
}

''' + anchor)
    s = '#include <atomic>\n' + s
    s = replace_once(s, '    m_render_data.geometry = std::move(data);',
        '''    m_render_data.geometry = std::move(data);
    m_geometry_revision = next_geometry_revision.fetch_add(1, std::memory_order_relaxed);''')
    for method in ('void GLModel::init_from(const indexed_triangle_set& its)',
                   'void GLModel::init_from(const Polygons& polygons, float z)'):
        start = s.index(method)
        data_at = s.index('    Geometry& data = m_render_data.geometry;', start)
        s = s[:data_at] + '    m_geometry_revision = next_geometry_revision.fetch_add(1, std::memory_order_relaxed);\n' + s[data_at:]
    s = replace_once(s, 'void GLModel::reset()\n{', '''void GLModel::reset()
{
    m_geometry_revision = next_geometry_revision.fetch_add(1, std::memory_order_relaxed);''')
    return s

edit('src/slic3r/GUI/GLModel.cpp', model_cpp)

for version in ('110', '140'):
    kernel = KERNEL.replace('SHADOW_TEXTURE', 'texture2D' if version == '110' else 'texture')
    kernel = kernel.replace('SHADOW_COMPARE(shadow_map_pcf, coordinates)',
        'shadow2D(shadow_map_pcf, coordinates).r' if version == '110' else
        'texture(shadow_map_pcf, coordinates)')
    for name in ('flat', 'gouraud', 'gouraud_light'):
        def shader(s, kernel=kernel, version=version):
            s = replace_once(s, 'uniform float shadow_light_size;', '''uniform sampler2DShadow shadow_map_pcf;
uniform bool shadow_hardware_pcf;
uniform vec2 shadow_penumbra_scale;''')
            start = s.index('vec2 shadow_poisson_offset(')
            end = s.index('void main()', start)
            s = s[:start] + kernel + '\n' + s[end:]
            s = replace_once(s, 'void main()\n{', '''void main()
{
    // Evaluate derivatives before discard and non-uniform lighting branches.
    vec2 shadowGradient = shadow_enabled ? shadow_receiver_gradient() : vec2(0.0);''')
            s = replace_once(s, 'pcss_shadow_factor() : 1.0', 'pcss_shadow_factor(shadowGradient) : 1.0')
            if version == '110':
                s = s.replace('texture(environment_tex,', 'texture2D(environment_tex,')
            return s
        edit(f'resources/shaders/{version}/{name}.fs', shader)
    attr = 'attribute' if version == '110' else 'in'
    out_v = 'varying' if version == '110' else 'out'
    in_v = 'varying' if version == '110' else 'in'
    vs = f'''#version {version}

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat4 volume_world_matrix;
uniform vec2 z_range;
uniform vec4 clipping_plane;

{attr} vec3 v_position;
{out_v} vec3 clipping_planes_dots;

void main()
{{
    vec4 world = volume_world_matrix * vec4(v_position, 1.0);
    clipping_planes_dots = vec3(dot(world, clipping_plane), world.z - z_range.x, z_range.y - world.z);
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}}
'''
    fs = f'''#version {version}

{in_v} vec3 clipping_planes_dots;
''' + ('out vec4 out_color;\n' if version == '140' else '') + '''
void main()
{
    if (any(lessThan(clipping_planes_dots, vec3(0.0))))
        discard;
''' + ('    out_color' if version == '140' else '    gl_FragColor') + ''' = vec4(1.0);
}
'''
    (ROOT / f'resources/shaders/{version}/shadow_depth.vs').write_text(vs)
    (ROOT / f'resources/shaders/{version}/shadow_depth.fs').write_text(fs)

edit('src/slic3r/GUI/GLShadersManager.cpp', lambda s: replace_once(s, '    return { valid, error };', '''    // Different sampler types must use distinct units even for draws with
    // shadow_enabled=false. Initialize before any thumbnail or gizmo draw.
    for (const char* name : { "flat", "gouraud", "gouraud_light" }) {
        if (GLShaderProgram* shader = get_shader(name)) {
            shader->start_using();
            shader->set_uniform("shadow_map", 1);
            shader->set_uniform("shadow_map_pcf", 2);
            shader->set_uniform("shadow_enabled", false);
            shader->stop_using();
        }
    }
    return { valid, error };'''))
print('Applied PCSS renderer, shader and cache changes to the pinned base.')
