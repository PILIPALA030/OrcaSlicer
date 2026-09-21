"""Source-contract checks. These are not interactive GUI or GPU acceptance tests."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
GUI = ROOT / 'src/slic3r/GUI'

def read(name):
    return (GUI / name).read_text(encoding='utf-8')

def function(source, name):
    start = source.index(name)
    end = source.index('\n}', start) + 2
    return source[start:end]

class IntegrationContracts(unittest.TestCase):
    def test_render_order(self):
        body = function(read('GLCanvas3D.cpp'), 'void GLCanvas3D::render(bool')
        self.assertLess(body.index('_apply_gcode_slider_changes()'), body.index('_prepare_pcss_shadow_map()'))
        self.assertLess(body.index('_prepare_pcss_shadow_map()'), body.index('_render_objects('))
        self.assertIn('_render_gcode(', body)

    def test_edit_transforms_and_caster_selection(self):
        body = function(read('GLCanvas3D.cpp'), 'void GLCanvas3D::_prepare_pcss_shadow_map()')
        for token in ('world_matrix()', 'geometry_id', 'tverts_range', 'is_modifier', 'is_wipe_tower', 'is_extrusion_path'):
            self.assertIn(token, body)
        self.assertIn('rotation.transpose()', body)
        self.assertIn('render_shadow_depth', body)

    def test_preview_uses_visible_index_ranges_and_caps(self):
        body = function(read('GCodeViewer.cpp'), 'void GCodeViewer::render_shadow_depth')
        for token in ('EMoveType::Extrude', 'render_paths', 'path.sizes', 'path.offsets', 'GL_UNSIGNED_SHORT',
                      'glMultiDrawElements', 'm_sequential_range_caps', 'cap.is_renderable()'):
            self.assertIn(token, body)
        for token in ('loadOBJ', 'ModelObject', 'm_shells', 'create_mesh', 'GL_LINES'):
            self.assertNotIn(token, body)

    def test_range_rebuild_invalidates_cache(self):
        source = read('GCodeViewer.cpp')
        for name in ('void GCodeViewer::reset()', 'void GCodeViewer::refresh_render_paths(bool',
                     'void GCodeViewer::set_toolpath_move_type_visible('):
            self.assertIn('m_shadow_revision', function(source, name))
        self.assertRegex(read('GCodeViewer.hpp'), r'mutable\s+std::uint64_t\s+m_shadow_revision')

    def test_slider_redraw_and_empty_input_guards(self):
        body = function(read('GLCanvas3D.cpp'), 'void GLCanvas3D::_apply_gcode_slider_changes()')
        for token in ('layers_slider->is_dirty()', 'moves_slider->is_dirty()', 'request_extra_frame()',
                      'has_data()', 'std::max(0.0,', 'set_layers_z_range', 'update_sequential_view_current'):
            self.assertIn(token, body)

    def test_display_preference_not_slicing_configuration(self):
        config = (ROOT / 'src/libslic3r/AppConfig.cpp').read_text(encoding="utf-8")
        self.assertRegex(config, r'get\("enable_shadow_map"\)\.empty\(\)\)\s*set_bool\("enable_shadow_map", false\)')
        pref = read('Preferences.cpp')
        self.assertIn('Enable soft shadows (PCSS)', pref)
        self.assertIn('get_preview_canvas3D()', pref)
        self.assertIn('get_view3D_canvas3D()', pref)
        self.assertNotIn('enable_shadow_map', (ROOT / 'src/libslic3r/PrintConfig.cpp').read_text(encoding="utf-8"))

    def test_off_skips_shadow_update(self):
        body = function(read('GLCanvas3D.cpp'), 'void GLCanvas3D::_prepare_pcss_shadow_map()')
        off = body[:body.index('m_gizmos.get_current_type')]
        self.assertIn('get_bool("enable_shadow_map")', off)
        self.assertIn('shutdown_gl()', off)
        self.assertIn('return;', off)

    def test_plate_is_polygon_only_before_icons(self):
        body = function(read('PartPlate.cpp'), 'void PartPlate::render(')
        self.assertIn('m_triangles.model.render()', body)
        self.assertLess(body.index('render_logo('), body.index('shadows->render_plate('))
        self.assertLess(body.index('shadows->render_plate('), body.index('render_icons('))

    def test_optional_registered_shaders(self):
        source = read('GLShadersManager.cpp')
        self.assertIn('prefix == "140/"', source)
        for name in ('pcss_depth', 'pcss_plate', 'gouraud_pcss', 'gouraud_light_pcss'):
            self.assertIn('"' + name + '"', source)
        self.assertIn('init_from_texts', source)
        self.assertIn('error.resize(error_length)', source)

    def test_shipping_sources_in_build(self):
        cmake = (ROOT / 'src/slic3r/CMakeLists.txt').read_text(encoding="utf-8")
        for filename in ('PCSSShadowRenderer.hpp', 'PCSSShadowRenderer.cpp', 'PCSSShadowMath.hpp'):
            self.assertIn('GUI/' + filename, cmake)
        for filename in ('pcss.glsl', 'pcss_depth.vs', 'pcss_depth.fs', 'pcss_plate.vs', 'pcss_plate.fs'):
            self.assertTrue((ROOT / 'resources/shaders/140' / filename).is_file())

    def test_main_light_only_and_legacy_materials(self):
        for filename in ('gouraud.fs', 'gouraud_light.fs'):
            source = (ROOT / 'resources/shaders/140' / filename).read_text(encoding="utf-8")
            self.assertIn('#ifdef ENABLE_PCSS', source)
            self.assertIn('pcss_main_diffuse', source)
            self.assertIn('uniform_color', source)
        common = (ROOT / 'resources/shaders/140/pcss.glsl').read_text(encoding="utf-8")
        self.assertIn('texelFetch', common)
        self.assertLess(common.index('dFdx'), common.index('if (!pcss_enabled'))
        self.assertIn('if (blockers == 0)', common)

    def test_chinese_translation(self):
        source = (ROOT / 'localization/i18n/zh_CN/Snapmaker_Orca_zh_CN.po').read_text(encoding="utf-8")
        self.assertIn('msgid "Enable soft shadows (PCSS)"', source)
        self.assertIn('启用柔和阴影（PCSS）', source)

if __name__ == '__main__':
    unittest.main(verbosity=2)
