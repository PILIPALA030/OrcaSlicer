#!/usr/bin/env python3
"""Generate opt-in diagnostic translation units; never edit source-tree files.

Exact anchors fail closed when renderer code changes. The manifest lists every
installed hook, skipped optional hook, and the SHA-256 of each original source.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

RP = "Slic3r::GUI::RenderProfile"
TOKENS = re.compile(
    r'R"(?P<tag>[^\s\\()]*)\(.*?\)(?P=tag)"|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
    re.S,
)


def masked(source: str) -> str:
    return TOKENS.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group()), source)


def matching(source: str, begin: int, left: str, right: str) -> int:
    level = 0
    for index in range(begin, len(source)):
        if source[index] == left:
            level += 1
        elif source[index] == right:
            level -= 1
            if level == 0:
                return index
    raise ValueError(f"Unbalanced {left}{right} at {begin}")


def bodies(source: str, name: str) -> list[tuple[int, int]]:
    code = masked(source)
    results = []
    for found in re.finditer(re.escape(name) + r'\s*\(', code):
        parenthesis = code.find('(', found.start())
        end = matching(code, parenthesis, '(', ')')
        brace = code.find('{', end + 1)
        semicolon = code.find(';', end + 1)
        if brace < 0 or (0 <= semicolon < brace):
            continue
        # Reject calls followed by unrelated code rather than a function body.
        qualifiers = code[end + 1:brace].strip()
        if qualifiers not in ('', 'const', 'override', 'const override', 'noexcept'):
            continue
        results.append((brace, matching(code, brace, '{', '}')))
    return results


class Editor:
    def __init__(self, source: str, filename: str):
        self.source = source
        self.filename = filename
        self.hooks: list[str] = []
        self.skipped: list[str] = []

    def function(self, name: str, transform, optional: bool = False):
        found = bodies(self.source, name)
        if not found and optional:
            self.skipped.append(name)
            return
        if len(found) != 1:
            raise ValueError(f"{self.filename}: expected one definition of {name}, found {len(found)}")
        start, end = found[0]
        body = self.source[start + 1:end]
        self.source = self.source[:start + 1] + transform(body) + self.source[end:]
        self.hooks.append(name)

    def scope(self, name: str, label: str, gpu: bool = True, optional: bool = False):
        self.function(name, lambda b: f'\n    {RP}::Scope orca_profile_scope({label}, {str(gpu).lower()});' + b, optional)

    def includes(self):
        self.source = once(self.source, '#include <GL/glew.h>',
                           '#include <GL/glew.h>\n#include "OrcaProfileBuild.hpp"\n#include "RenderProfile.hpp"')


def once(source: str, old: str, new: str) -> str:
    count = source.count(old)
    if count != 1:
        raise ValueError(f"Expected one anchor, got {count}: {old[:130]!r}")
    return source.replace(old, new, 1)


def instrument_canvas(source: str) -> Editor:
    e = Editor(source, 'GLCanvas3D.cpp')
    has_shadow = bool(bodies(source, 'GLCanvas3D::RenderShadowMap'))
    e.includes()
    e.function('GLCanvas3D::~GLCanvas3D', lambda b:
               f'\n    {RP}::release(this, [this]() {{ return m_canvas != nullptr && _set_current(); }});' + b)

    def render(b):
        b = f'\n    const auto orca_profile_entry = {RP}::entry_time();' + b
        anchor = '    const Size& cnv_size = get_canvas_size();'
        requested = 'wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool("show_model_shadow")' if has_shadow else 'false'
        b = once(b, anchor, anchor + f'''
    {RP}::Frame orca_profile_frame(this,
        {{static_cast<int>(m_canvas_type), static_cast<int>(cnv_size.get_width()), static_cast<int>(cnv_size.get_height()),
         {requested}, false, !m_selection.is_empty(), m_picking_enabled, m_volumes.volumes.size()}}, orca_profile_entry);
    {RP}::Scope orca_profile_prepare("prepare");''')
        b = once(b, '    if (m_picking_enabled) {', '    orca_profile_prepare.stop();\n    if (m_picking_enabled) {')
        if has_shadow:
            b = once(b, '    else\n        m_shadowMap.valid = false;',
                     '    else\n        m_shadowMap.valid = false;\n    orca_profile_frame.ready(shadowMapReady);')
            b = once(b, '    const auto setShadowEnabled = [](bool enabled) {',
                     '    const auto setShadowEnabled = [](bool enabled) {\n' +
                     f'        {RP}::Scope orca_profile_reset("shadow_uniform_reset", false);')
        # Scope a value-returning call without changing its evaluation count.
        anchor = 'm_mouse.scene_position = _mouse_to_3d(m_mouse.position.cast<coord_t>());'
        b = once(b, anchor, f'''m_mouse.scene_position = [&]() {{
            {RP}::Scope orca_profile_mouse("mouse_depth_readback");
            return _mouse_to_3d(m_mouse.position.cast<coord_t>());
        }}();''')
        b = once(b, '    wxGetApp().imgui()->render();', f'''    {{
        {RP}::Scope orca_profile_imgui("imgui");
        wxGetApp().imgui()->render();
    }}''')
        b = once(b, '    m_canvas->SwapBuffers();', f'''    orca_profile_frame.before_swap();
    {{
        {RP}::Scope orca_profile_swap("swap", false);
        m_canvas->SwapBuffers();
    }}
    orca_profile_frame.finish();''')
        return b
    e.function('GLCanvas3D::render', render)
    if has_shadow:
        def shadow(b):
            b = once(b, '    m_shadowMap.valid = false;',
                     f'    {RP}::Scope orca_profile_total("shadow_total", false);\n'
                     '    m_shadowMap.valid = false;\n' + f'    if ({RP}::static_off()) return false;')
            anchor = '    GLShaderProgram* const shader = wxGetApp().get_shader("shadow_depth");'
            b = once(b, anchor, f'    {RP}::Scope orca_profile_enabled("shadow_enabled_total");\n'
                     f'    {RP}::Scope orca_profile_setup("shadow_setup");\n' + anchor)
            anchor = 'const BoundingBoxf3 bounds = _max_bounding_box(false, true, true);'
            b = once(b, anchor, f'''const BoundingBoxf3 bounds = [&]() {{
        {RP}::Scope orca_profile_bounds("shadow_bounds", false);
        return _max_bounding_box(false, true, true);
    }}();''')
            anchor = '    shader->start_using();\n    for (GLVolume* volume : m_volumes.volumes)'
            b = once(b, anchor, '    orca_profile_setup.stop();\n' +
                     f'    {RP}::Scope orca_profile_geometry("shadow_geometry");\n' + anchor)
            b = once(b, '    shader->stop_using();', '    shader->stop_using();\n    orca_profile_geometry.stop();\n' +
                     f'    {RP}::Scope orca_profile_restore("shadow_restore");')
            return b
        e.function('GLCanvas3D::RenderShadowMap', shadow)
        e.scope('GLCanvas3D::EnsureShadowMapResources', '"shadow_resources"')
        e.function('GLCanvas3D::BindShadowUniforms', lambda b:
                   f'\n    {RP}::Scope orca_profile_uniforms("shadow_uniforms", false);' + once(
                       b, 'shader->set_uniform("shadow_enabled", m_shadowMap.valid);',
                       f'shader->set_uniform("shadow_enabled", m_shadowMap.valid && !{RP}::depth_only());'))
    e.scope('GLCanvas3D::_render_objects',
            'type == GLVolumeCollection::ERenderType::Transparent ? "objects_transparent" : "objects_opaque"')
    required = {
        '_picking_pass': 'picking', '_rectangular_selection_picking_pass': 'picking_rectangle',
        '_render_bed': 'bed', '_render_platelist': 'plate_list', '_render_selection': 'selection',
        '_render_overlays': 'overlays', '_render_background': 'background',
        '_render_current_gizmo': 'gizmos', '_render_sequential_clearance': 'sequential_clearance',
    }
    for name, label in required.items():
        e.scope('GLCanvas3D::' + name, json.dumps(label))
    optional = {
        'ResolveSelectionHighlightMode': 'selection_prepare',
        'RenderSelectionHighlightMask': 'selection_mask',
        'RenderSelectionOutlineTextures': 'selection_outline_textures',
        'RenderSelectionGaussianPass': 'selection_gaussian',
        'CompositeSelectionHighlight': 'selection_composite',
        'RenderSelectionStencilFallback': 'selection_stencil',
    }
    for name, label in optional.items():
        e.scope('GLCanvas3D::' + name, json.dumps(label), optional=True)
    return e


def instrument_model(source: str) -> Editor:
    e = Editor(source, 'GLModel.cpp')
    e.includes()
    anchor = '    glsafe(::glDrawElements(mode, range.second - range.first, index_type, (const void*)(range.first * Geometry::index_stride_bytes(data))));'
    e.source = once(e.source, anchor, f'    {RP}::draw(mode, range.second - range.first, 1, shader->get_name());\n' + anchor)
    anchor = '    glsafe(::glDrawElementsInstanced(mode, indices_count(), index_type, (const void*)0, instances_count));'
    e.source = once(e.source, anchor, f'    {RP}::draw(mode, indices_count(), instances_count, shader->get_name());\n' + anchor)
    e.hooks += ['draw_elements', 'draw_elements_instanced']
    def upload(b):
        # Count bytes at each real BufferData call, including compressed index types.
        for size in ('data.vertices_size_bytes()', 'indices_count * sizeof(unsigned char)',
                     'indices_count * sizeof(unsigned short)', 'data.indices_size_bytes()'):
            pattern = re.compile(r'(?m)^(\s*)glsafe\(::glBufferData\((GL_ARRAY_BUFFER|GL_ELEMENT_ARRAY_BUFFER), ' + re.escape(size) + r',')
            matches = list(pattern.finditer(b))
            if len(matches) != 1:
                raise ValueError(f"BufferData anchor missing: {size}")
            start = matches[0].start()
            indent = matches[0].group(1)
            b = b[:start] + indent + f'{RP}::upload({size});\n' + b[start:]
        return f'\n    {RP}::Scope orca_profile_upload("model_upload", false);' + b
    e.function('GLModel::send_to_gpu', upload)
    return e


def instrument_scene(source: str) -> Editor:
    e = Editor(source, '3DScene.cpp')
    e.includes()
    def render(b):
        anchor = '    if (shader == nullptr)\n        return;'
        b = once(b, anchor, anchor + f'''
    if ({RP}::current()) {{
        {RP}::count("volume_calls");
        {RP}::count("volume_source_triangles", model.indices_count() / 3);
        {RP}::count("volume_lod_" + std::to_string(static_cast<int>(m_curLodLevel)));
        if (m_lodSmallReady && !m_lodSmallReady->load(std::memory_order_acquire))
            {RP}::count("visible_small_lod_pending_calls");
        if (m_lodMiddleReady && !m_lodMiddleReady->load(std::memory_order_acquire))
            {RP}::count("visible_middle_lod_pending_calls");
    }}''')
        if 'if (shadow_model)' in b:
            anchor = '        if (shadow_model) {'
            b = once(b, anchor, f'''        if ({RP}::current()) {{
            if (!shadow_model) {RP}::count("shadow_omitted_calls");
            else if (shadow_model == &model) {RP}::count("shadow_original_calls");
            else {RP}::count("shadow_lod_or_proxy_calls");
        }}
''' + anchor)
        return b
    # LOD instrumentation is specifically for this feature branch, not arbitrary upstream versions.
    if 'm_lodSmallReady' in source:
        e.function('GLVolume::render', render)
    else:
        e.skipped.append('GLVolume::render: legacy/no-LOD source')
    return e


def instrument_shader(source: str) -> Editor:
    e = Editor(source, 'GLShader.cpp')
    e.includes()
    anchor = '        s.close();'
    e.source = once(e.source, anchor, anchor + f'''
        // Diagnostic control only: allow the GLSL compiler/linker to remove
        // unreachable PCSS code and unused shadow varyings in an A/B process.
        if ({RP}::static_off() &&
            (filename == "110/flat.fs" || filename == "140/flat.fs" ||
             filename == "110/gouraud.fs" || filename == "140/gouraud.fs" ||
             filename == "110/gouraud_light.fs" || filename == "140/gouraud_light.fs")) {{
            const std::string declaration = "uniform bool shadow_enabled;";
            const auto pos = source.find(declaration);
            if (pos != std::string::npos)
                source.replace(pos, declaration.size(), "const bool shadow_enabled = false;");
        }}''')
    e.hooks.append('static_off_receiver_compile')
    return e


def instrument_proxy(source: str) -> Editor:
    e = Editor(source, 'ShadowMeshProxy.hpp')
    e.source = once(e.source, '#include "GLModel.hpp"',
                    '#include "GLModel.hpp"\n#include "OrcaProfileBuild.hpp"\n#include "RenderProfile.hpp"')
    def get(b):
        anchor = '        const State state = m_task->state.load(std::memory_order_acquire);'
        b = once(b, anchor, anchor + f'\n        if ({RP}::current())\n'
                 f'            {RP}::count("proxy_state_" + std::to_string(static_cast<int>(state)));')
        return f'\n        {RP}::Scope orca_profile_adopt("proxy_get", false);' + b
    e.function('GLModel* get', get)
    e.source = once(e.source, '            std::thread([source, task, slot] {',
                    '            std::thread([source, task, slot] {\n' +
                    f'                {RP}::Job orca_profile_job(source->its.indices.size());')
    anchor = '                    auto mesh = std::make_unique<indexed_triangle_set>(source->its);'
    e.source = once(e.source, anchor, anchor + '\n                    orca_profile_job.copied();')
    anchor = '                    its_quadric_edge_collapse(*mesh, TRIANGLE_BUDGET, nullptr, check_cancel);'
    e.source = once(e.source, anchor, anchor + '\n                    orca_profile_job.simplified();')
    anchor = '                    task->result = std::move(mesh);'
    e.source = once(e.source, anchor, '                    orca_profile_job.result(mesh->indices.size());\n' + anchor)
    e.hooks.append('proxy_copy_and_actual_qem_wall_time')
    return e


PATCHERS = {
    'GLCanvas3D.cpp': instrument_canvas, 'GLModel.cpp': instrument_model,
    '3DScene.cpp': instrument_scene, 'GLShader.cpp': instrument_shader,
    'ShadowMeshProxy.hpp': instrument_proxy,
}


def write_changed(path: Path, contents: bytes):
    if not path.exists() or path.read_bytes() != contents:
        path.write_bytes(contents)


def generate(gui_dir: Path, output_dir: Path) -> dict:
    gui_dir, output_dir = gui_dir.resolve(), output_dir.resolve()
    if gui_dir == output_dir or gui_dir in output_dir.parents:
        raise ValueError('Output must be outside the source GUI directory')
    prepared = {}
    manifest = {'schema': 1, 'files': {}}
    for name, patcher in PATCHERS.items():
        path = gui_dir / name
        if name == 'ShadowMeshProxy.hpp' and not path.exists():
            continue
        raw = path.read_bytes()
        editor = patcher(raw.decode('utf-8').replace('\r\n', '\n'))
        prepared[name] = editor.source.encode('utf-8')
        manifest['files'][name] = {'sha256': hashlib.sha256(raw).hexdigest(), 'hooks': editor.hooks, 'skipped': editor.skipped}
    digest = hashlib.sha256(json.dumps(manifest['files'], sort_keys=True).encode()).hexdigest()
    repo = gui_dir.parents[2]
    result = subprocess.run(['git', '-C', str(repo), 'describe', '--always', '--dirty', '--abbrev=12'],
                            text=True, capture_output=True, check=False)
    revision = result.stdout.strip() if result.returncode == 0 else 'unknown'
    manifest.update(revision=revision, source_digest=digest)
    header = (f'#define ORCA_PROFILE_REVISION {json.dumps(revision)}\n'
              f'#define ORCA_PROFILE_SOURCE_DIGEST {json.dumps(digest)}\n')
    # All validation precedes all writes; a missing anchor never partially patches a source file.
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, raw in prepared.items():
        write_changed(output_dir / name, raw)
    write_changed(output_dir / 'OrcaProfileBuild.hpp', header.encode())
    write_changed(output_dir / 'manifest.json', (json.dumps(manifest, indent=2) + '\n').encode())
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gui-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = generate(args.gui_dir, args.output_dir)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Render profiling generation failed: {error}\n')
    print(f'Render profiling: {sum(len(v["hooks"]) for v in result["files"].values())} hooks; '
          f'revision={result["revision"]}; manifest={args.output_dir / "manifest.json"}')


if __name__ == '__main__':
    main()
