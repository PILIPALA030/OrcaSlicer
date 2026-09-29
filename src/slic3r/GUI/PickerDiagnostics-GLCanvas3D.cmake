pd_entry("void GLCanvas3D::render(bool only_init, bool overlayOnly)" [=[
    ::Slic3r::GUI::PickerDiag::Frame diagnosticFrame(this, only_init, overlayOnly);
    PD_EVENT("FRAME_FLAGS", "in_render=" << m_in_render << " enabled=" << m_enable_render << " dirty=" << m_dirty
        << " overlay_dirty=" << m_overlayDirty << " initialized=" << m_initialized << " context=" << m_context);
]=])
pd_render_replace("    if (only_init)\n        return;" [=[
    if (only_init)
        return;
    diagnosticFrame.accept();
    ::Slic3r::GUI::PickerDiag::on_context(m_context);]=])
pd_render_replace("if (!_is_shown_on_screen() || !_set_current() || !wxGetApp().init_opengl())" [=[if (!PD_CALL("is_shown_on_screen", _is_shown_on_screen()) || !PD_CALL("set_current", _set_current()) || !PD_CALL("init_opengl", wxGetApp().init_opengl()))]=])
pd_render_replace("if (!is_initialized() && !init())" "if (!is_initialized() && !PD_CALL(\"canvas_init\", init()))")
pd_render_replace("    camera.UpdateFrustum();" [=[
    PD_CALL("camera_update_frustum", camera.UpdateFrustum());
    ::Slic3r::GUI::PickerDiag::camera_state(this, camera.get_view_matrix().matrix().data(), camera.get_projection_matrix().matrix().data(), camera.get_viewport().data());]=])
pd_render_replace("camera.apply_projection(_max_bounding_box(true, true, true));" "PD_CALL(\"camera_projection_and_bounds\", camera.apply_projection(_max_bounding_box(true, true, true)));")
pd_render_replace("    if (shouldUpdatePickingBuffer)\n    {" [=[
    PD_EVENT("PICK_DECISION", "enabled=" << m_picking_enabled << " rectangle=" << isRectanglePicking
        << " regular=" << isRegularPicking << " gpu_point=" << isGpuPointPicking << " should_render=" << shouldRenderPickingBuffer
        << " dirty=" << m_pickingBufferDirty << " size_changed=" << pickingBufferSizeChanged << " update=" << shouldUpdatePickingBuffer
        << " fbo_ready=" << m_pickingBuffer.IsReady() << " width=" << m_pickingBuffer.GetWidth() << " height=" << m_pickingBuffer.GetHeight());
    if (shouldUpdatePickingBuffer)
    {]=])
pd_render_replace("        m_pickingBufferDirty = !rendered;" [=[
        m_pickingBufferDirty = !rendered;
        PD_EVENT("PICK_REBUILD_RESULT", "logical_success=" << rendered << " dirty_after=" << m_pickingBufferDirty << " gpu_completion_not_implied=1");]=])
pd_entry("bool GLCanvas3D::RenderPickingBuffer(" [=[
    PD_PASS("picking");
    PD_SCOPE("RenderPickingBuffer");
    PD_GPU_SCOPE("picking");
]=])
pd_entry("bool GLCanvas3D::_render_volumes_for_picking(" [=[
    PD_PASS("picking");
    PD_SCOPE("picking_volumes");
    PD_EVENT("PICK_VOLUMES", "total=" << m_volumes.volumes.size());
]=])
pd_pick_draw_replace("        const ColorRGBA previousColor = volume->model.get_color();" [=[
        PD_TAG(Volume, volume);
        PD_VERBOSE_SCOPE("picking_volume");
        PD_EVENT("PICK_VOLUME", "index=" << volumeIndex << " object=" << volume->composite_id.object_id
            << " instance=" << volume->composite_id.instance_id << " volume_index=" << volume->composite_id.volume_id
            << " requested_lod=" << static_cast<int>(volume->m_curLodLevel) << " mesh=" << volume->m_oriMesh);
        const ColorRGBA previousColor = volume->model.get_color();]=])
pd_entry("GLCanvas3D::VolumePickResult GLCanvas3D::QueryVolumeFromPickingBuffer(" [=[
    PD_SCOPE("QueryVolumeFromPickingBuffer");
    PD_EVENT("VOLUME_QUERY", "x=" << screenPosition.x() << " y=" << screenPosition.y() << " tolerance=" << toleranceRadiusPx
        << " fbo_ready=" << m_pickingBuffer.IsReady() << " fbo_dirty=" << m_pickingBufferDirty);
]=])
pd_query_replace("    VolumePickResult result;" [=[
    VolumePickResult result;
    Slic3r::ScopeGuard diagnosticQueryResult([&result]() {
        PD_EVENT("VOLUME_QUERY_RESULT", "status=" << static_cast<int>(result.status));
    });]=])
pd_query_replace("    const GLPickingBuffer::ColorPixel& centerPixel = pixels[centerIndex];" [=[
    const GLPickingBuffer::ColorPixel& centerPixel = pixels[centerIndex];
    PD_DETAIL("CENTER_PIXEL", "rgba=" << int(centerPixel.rgba[0]) << ',' << int(centerPixel.rgba[1]) << ','
        << int(centerPixel.rgba[2]) << ',' << int(centerPixel.rgba[3]) << " valid=" << centerPixel.IsValid() << " background=" << centerPixel.IsBackground());]=])
pd_query_replace("    result.hit.normal = Vec3f::Zero();" [=[
    result.hit.normal = Vec3f::Zero();
    PD_EVENT("VOLUME_HIT", "id=" << candidate->volumeId << " depth=" << depth << " offset_squared=" << candidate->distanceSquared
        << " world=" << worldPosition.x() << ',' << worldPosition.y() << ',' << worldPosition.z());]=])
pd_entry("GLCanvas3D::PickingPassResult GLCanvas3D::QueryHybridPickingHit(" [=[
    PD_TAG(Query, nullptr);
    PD_SCOPE("QueryHybridPickingHit");
]=])
pd_replace("m_scene_raycaster.hit(samplePosition, camera, &clippingPlane, SceneRaycaster::EHitMask::All)" "PD_CALL(\"CPU_RAYCAST_ALL_FALLBACK\", m_scene_raycaster.hit(samplePosition, camera, &clippingPlane, SceneRaycaster::EHitMask::All))")
pd_replace("m_scene_raycaster.hit(samplePosition, camera, nullptr, SceneRaycaster::EHitMask::NonVolume)" "PD_CALL(\"CPU_RAYCAST_NONVOLUME\", m_scene_raycaster.hit(samplePosition, camera, nullptr, SceneRaycaster::EHitMask::NonVolume))")
pd_replace("m_scene_raycaster.hit(volumeResult.samplePosition, camera, nullptr, SceneRaycaster::EHitMask::Bed)" "PD_CALL(\"CPU_RAYCAST_BED\", m_scene_raycaster.hit(volumeResult.samplePosition, camera, nullptr, SceneRaycaster::EHitMask::Bed))")
pd_entry("bool GLCanvas3D::RaycastVolume(" [=[
    PD_SCOPE("RaycastVolume");
    PD_EVENT("CPU_VOLUME_RAYCAST", "volume_index=" << volumeIndex);
]=])
pd_entry("void GLCanvas3D::ResolveSelectedVolumeOverlap(" [=[
    PD_SCOPE("ResolveSelectedVolumeOverlap");
]=])
pd_entry("std::optional<GLCanvas3D::PickingPassResult> GLCanvas3D::_picking_pass(" [=[
    PD_SCOPE("point_picking_pass");
]=])
pd_entry("bool GLCanvas3D::_rectangular_selection_picking_pass(" [=[
    PD_TAG(Query, nullptr);
    PD_PASS("rectangle_picking");
    PD_SCOPE("rectangle_picking_pass");
]=])
pd_entry("void GLCanvas3D::RenderMainSceneContent(" [=[
    PD_PASS("main");
    PD_SCOPE("RenderMainSceneContent");
    PD_GPU_SCOPE("main_scene");
    ::Slic3r::GUI::PickerDiag::cache_note(this,"main_redraw");
]=])
pd_entry("void GLCanvas3D::_render_objects(" [=[
    PD_PASS(type == GLVolumeCollection::ERenderType::Opaque ? "main_opaque" : "main_transparent");
    PD_SCOPE("render_objects");
    PD_GPU_SCOPE("render_objects");
    PD_EVENT("RENDER_OBJECTS", "type=" << static_cast<int>(type));
]=])
pd_render_replace("    /* view3D render*/" [=[
    PD_EVENT("SCENE_DECISION", "full_refresh=" << fullSceneRefresh << " cache_eligible=" << sceneCacheEligible
        << " cache_valid=" << m_sceneCacheValid << " samples=" << sceneCacheSamples << " target_fbo=" << targetDrawFramebuffer
        << " capture_deferred=" << m_sceneCacheCaptureDeferred);
    /* view3D render*/]=])
pd_render_replace("                m_sceneCacheValid = true;" [=[
                m_sceneCacheValid = true;
                ::Slic3r::GUI::PickerDiag::cache_note(this,"capture_success");]=])
pd_entry("bool GLCanvas3D::CaptureSceneCache(" [=[
    PD_PASS("cache_capture");
    PD_SCOPE("CaptureSceneCache");
    PD_GPU_SCOPE("cache_capture");
    PD_EVENT("CACHE_CAPTURE", "fbo=" << m_sceneCacheResources.framebuffer << " width=" << m_sceneCacheResources.width
        << " height=" << m_sceneCacheResources.height << " samples=" << m_sceneCacheResources.samples);
]=])
pd_entry("bool GLCanvas3D::PresentSceneCache(" [=[
    PD_PASS("cache_present");
    PD_SCOPE("PresentSceneCache");
    PD_GPU_SCOPE("cache_present");
    ::Slic3r::GUI::PickerDiag::cache_note(this,"present_attempt");
]=])
pd_entry("bool GLCanvas3D::EnsureSceneCacheResources(" [=[
    PD_SCOPE("EnsureSceneCacheResources");
    PD_EVENT("CACHE_ENSURE", "width=" << canvasSize.get_width() << " height=" << canvasSize.get_height() << " samples=" << samples);
]=])
pd_entry("void GLCanvas3D::ReleaseSceneCacheResources(" [=[
    PD_SCOPE("ReleaseSceneCacheResources");
]=])
pd_entry("void GLCanvas3D::InvalidatePickingBuffer(" [=[
    PD_EVENT("INVALIDATE_PICKING", "previous_dirty=" << m_pickingBufferDirty << " caller_stage=" << ::Slic3r::GUI::PickerDiag::context().stage);
    ::Slic3r::GUI::PickerDiag::cache_note(this,"invalidate_picking");
]=])
pd_entry("void GLCanvas3D::InvalidateSceneCache(" [=[
    PD_EVENT("INVALIDATE_SCENE", "previous_valid=" << m_sceneCacheValid << " caller_stage=" << ::Slic3r::GUI::PickerDiag::context().stage);
    ::Slic3r::GUI::PickerDiag::cache_note(this,"invalidate_scene");
]=])
foreach(method "ResolveSelectionHighlightMode" "UpdateSelectionHighlightCache" "CompositeSelectionHighlight" "RenderSelectionStencilFallback")
    pd_entry("GLCanvas3D::${method}(" "    PD_PASS(\"selection_highlight\");\n    PD_SCOPE(\"${method}\");\n    PD_GPU_SCOPE(\"${method}\");\n")
endforeach()
foreach(method "_render_sequential_clearance" "_render_overlays" "_render_current_gizmo" "_render_selection_sidebar_hints" "_mouse_to_3d" "UpdateVolumeClippingState")
    pd_entry("GLCanvas3D::${method}(" "    PD_SCOPE(\"${method}\");\n")
endforeach()
foreach(method "on_paint" "on_idle" "_refresh_if_shown_on_screen" "_zoom_to_box" "_update_camera_zoom" "select_view")
    pd_entry("GLCanvas3D::${method}(" "    PD_VERBOSE_SCOPE(\"${method}\");\n")
endforeach()
pd_render_replace("    wxGetApp().imgui()->render();" "    PD_CALL(\"imgui_render\", wxGetApp().imgui()->render());")
pd_render_replace("    m_canvas->SwapBuffers();\n    m_render_stats.increment_fps_counter();" [=[
    PD_CALL("SwapBuffers", m_canvas->SwapBuffers());
    diagnosticFrame.swapped();
    m_render_stats.increment_fps_counter();]=])
pd_replace("    const bool contextCurrent = _set_current();" [=[
    const bool contextCurrent = _set_current();
    if (contextCurrent) ::Slic3r::GUI::PickerDiag::release_context(m_context);
    ::Slic3r::GUI::PickerDiag::cache_note(this,"destroy");]=])
