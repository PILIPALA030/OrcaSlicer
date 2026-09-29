pd_replace("GLenum err = glGetError();" [=[GLenum err = glGetError();
    ::Slic3r::GUI::PickerDiag::error_record(err, function_name);]=])
pd_entry("void GLVolume::render()" [=[
    PD_TAG(Volume, this);
    PD_VERBOSE_SCOPE("GLVolume.render");
]=])
pd_entry("void GLVolume::simple_render(" [=[
    PD_TAG(Volume, this);
    PD_VERBOSE_SCOPE("GLVolume.simple_render");
    // Only pointer/atomic readiness observations here: the worker may still be
    // writing the unpublished LOD geometry. Do not inspect its vectors.
    PD_DETAIL("LOD_REQUEST", "requested=" << static_cast<int>(m_curLodLevel) << " picking=" << picking
        << " small_ptr=" << m_modelSmall.get() << " middle_ptr=" << m_modelMiddle.get()
        << " small_ready=" << (m_lodSmallReady && m_lodSmallReady->load(std::memory_order_acquire))
        << " middle_ready=" << (m_lodMiddleReady && m_lodMiddleReady->load(std::memory_order_acquire)));
]=])
pd_replace("                m_modelSmall->render();" [=[
                ::Slic3r::GUI::PickerDiag::note_lod("Small");
                PD_EVENT("LOD_USE", "requested=" << static_cast<int>(m_curLodLevel) << " effective=Small picking=" << picking
                    << " selected_geometry=" << m_modelSmall.get() << " indices=" << m_modelSmall->indices_count());
                m_modelSmall->render();]=])
pd_replace("                m_modelMiddle->render();" [=[
                ::Slic3r::GUI::PickerDiag::note_lod("Middle");
                PD_EVENT("LOD_USE", "requested=" << static_cast<int>(m_curLodLevel) << " effective=Middle picking=" << picking
                    << " selected_geometry=" << m_modelMiddle.get() << " indices=" << m_modelMiddle->indices_count());
                m_modelMiddle->render();]=])
pd_replace("                //model.set_color(ColorRGBA::RED());" [=[
                ::Slic3r::GUI::PickerDiag::note_lod("High");
                PD_EVENT("LOD_USE", "requested=" << static_cast<int>(m_curLodLevel) << " effective=High picking=" << picking
                    << " selected_geometry=" << &model << " indices=" << model.indices_count()
                    << " reason=" << (m_curLodLevel == LODLevel::High ? "requested_high" : "requested_lod_unavailable"));
                //model.set_color(ColorRGBA::RED());]=])
pd_entry("void GLWipeTowerVolume::render()" [=[
    PD_TAG(Volume, this);
    PD_VERBOSE_SCOPE("GLWipeTowerVolume.render");
    PD_DETAIL("WIPE_TOWER", "submodels=" << model_per_colors.size() << " picking=" << picking);
]=])
pd_entry("bool GLVolumeCollection::render(" [=[
    PD_SCOPE("GLVolumeCollection.render");
    PD_EVENT("COLLECTION_RENDER", "render_type=" << static_cast<int>(type) << " volumes=" << volumes.size());
]=])
pd_replace("            if (prevLod != v->m_curLodLevel) {" [=[
            PD_DETAIL("LOD_EVALUATED", "volume=" << v << " before=" << static_cast<int>(prevLod)
                << " after=" << static_cast<int>(v->m_curLodLevel) << " zoom=" << curZoom << " zoom_trigger=" << shouldEvaluate);
            if (prevLod != v->m_curLodLevel) {
                PD_EVENT("LOD_CHANGED", "volume=" << v << " before=" << static_cast<int>(prevLod) << " after=" << static_cast<int>(v->m_curLodLevel));]=])
pd_replace("if (!camera.GetFrustum().Intersects(_worldAABB))" [=[const bool diagnosticVisible = camera.GetFrustum().Intersects(_worldAABB);
        PD_DETAIL("FRUSTUM_TEST", "volume=" << volume.first << " visible=" << diagnosticVisible);
        if (!diagnosticVisible)]=])
pd_entry("bool GLVolume::promote_ready_lod_models()" [=[
    PD_TAG(Volume, this);
    PD_VERBOSE_SCOPE("GLVolume.promote_ready_lod_models");
]=])
pd_replace("        m_modelMiddle->enable_render();" [=[
        PD_EVENT("LOD_PROMOTE", "level=Middle model=" << m_modelMiddle.get() << " requested=" << static_cast<int>(m_curLodLevel));
        m_modelMiddle->enable_render();]=])
pd_replace("        m_modelSmall->enable_render();" [=[
        PD_EVENT("LOD_PROMOTE", "level=Small model=" << m_modelSmall.get() << " requested=" << static_cast<int>(m_curLodLevel));
        m_modelSmall->enable_render();]=])
pd_entry("int GLVolumeCollection::load_object_volume(" [=[
    PD_SCOPE("load_object_volume");
    PD_EVENT("LOAD_VOLUME_BEGIN", "object=" << obj_idx << " volume_index=" << volume_idx << " instance=" << instance_idx
        << " lod_enabled=" << lodEnabled << " need_raycaster=" << need_raycaster);
]=])
pd_replace("    GLVolume& v = *this->volumes.back();\n    v.set_color(color_from_model_volume(*model_volume));" [=[
    GLVolume& v = *this->volumes.back();
    PD_TAG(Volume, &v);
    PD_EVENT("LOAD_VOLUME", "mesh=" << meshPtr << " faces=" << meshPtr->its.indices.size()
        << " main_geometry=" << &v.model << " object=" << obj_idx << " instance=" << instance_idx);
    v.set_color(color_from_model_volume(*model_volume));]=])
pd_replace("            v.m_lodSmallReady  = firstVolume->m_lodSmallReady;" [=[
            v.m_lodSmallReady  = firstVolume->m_lodSmallReady;
            PD_EVENT("LOD_SHARED", "source_volume=" << firstVolume << " small_geometry=" << v.m_modelSmall.get()
                << " middle_geometry=" << v.m_modelMiddle.get() << " main_geometry_not_shared=" << &v.model);]=])
pd_entry("bool GLVolume::SimplifyMesh(const indexed_triangle_set& its," [=[
    PD_SCOPE("LOD_PREPARE");
    PD_EVENT("LOD_INPUT", "lod=" << static_cast<int>(lod) << " input_vertices=" << its.vertices.size() << " input_faces=" << its.indices.size());
]=])
pd_replace("auto itsCopy = std::make_unique<indexed_triangle_set>(its);" "auto itsCopy = PD_CALL(\"LOD_COPY_INPUT\", std::make_unique<indexed_triangle_set>(its));")
pd_replace("    std::thread worker = std::thread(" [=[
    const uint64_t diagnosticTaskId = ::Slic3r::GUI::PickerDiag::enabled() ? ::Slic3r::GUI::PickerDiag::next_id() : 0;
    PD_EVENT("LOD_SPAWN_BEGIN", "task=" << diagnosticTaskId << " lod=" << static_cast<int>(lod) << " model=" << model.get());
    std::thread worker = std::thread(]=])
pd_replace("[model, readyFlag, maxError, originMesh](std::unique_ptr<indexed_triangle_set> itsPtr) {" [=[
        [model, readyFlag, maxError, originMesh, diagnosticTaskId](std::unique_ptr<indexed_triangle_set> itsPtr) {
            PD_TAG(Geometry, model.get());
            PD_SCOPE("LOD_WORKER");
            bool diagnosticReadyPublished = false;
            ::Slic3r::GUI::PickerDiag::task_delta(1);
            PD_EVENT("LOD_TASK_BEGIN", "task=" << diagnosticTaskId);
            Slic3r::ScopeGuard diagnosticTaskEnd([&]() {
                PD_EVENT("LOD_TASK_END", "task=" << diagnosticTaskId << " ready_published=" << diagnosticReadyPublished);
                ::Slic3r::GUI::PickerDiag::task_delta(-1);
            });]=])
pd_replace("            its_quadric_edge_collapse(*itsPtr, triangleCount, &maxErrCopy);" "            PD_CALL(\"LOD_QEM\", its_quadric_edge_collapse(*itsPtr, triangleCount, &maxErrCopy));")
pd_replace("            int endFaceCount = (*itsPtr).indices.size();" [=[
            int endFaceCount = (*itsPtr).indices.size();
            PD_EVENT("LOD_QEM_RESULT", "task=" << diagnosticTaskId << " input_faces=" << initFaceCount << " output_faces=" << endFaceCount);]=])
pd_replace("            if (readyFlag)\n                readyFlag->store(true, std::memory_order_release);" [=[
            if (readyFlag) {
                readyFlag->store(true, std::memory_order_release);
                diagnosticReadyPublished = true;
                // No model data may be inspected after publication.
                PD_EVENT("LOD_READY_PUBLISHED", "task=" << diagnosticTaskId);
            }]=])
pd_replace("        std::move(itsCopy));" [=[
        std::move(itsCopy));
    PD_EVENT("LOD_SPAWN_END", "task=" << diagnosticTaskId);]=])

# Display-only MMU LOD experiment. Default ON in this diagnostic build.
# ORCA_MMU_DISPLAY_LOD=0 (set before starting a new process) restores the
# original multicolor display path without recompiling. Runtime logging OFF
# does not disable the experiment, so logging-overhead comparisons stay valid.
# Middle/Small use the existing single-color LOD; High keeps original colors.
# No paint data, picking policy, LOD evaluation policy, or GL state is changed.
pd_entry("void GLVolume::simple_render(" [=[
    static const bool diagnosticMmuDisplayLodEnabled = []() {
#if defined(_MSC_VER)
        size_t required = 0;
        char value[2] = {};
        return ::getenv_s(&required, value, sizeof(value), "ORCA_MMU_DISPLAY_LOD") != 0 ||
               required != 2 || value[0] != '0';
#else
        const char* value = ::getenv("ORCA_MMU_DISPLAY_LOD");
        return value == nullptr || ::strcmp(value, "0") != 0;
#endif
    }();
    // The main scene uses gouraud; thumbnails and ID/mask shaders stay unchanged.
    const bool diagnosticMainDisplayShader = shader != nullptr && shader->get_name() == "gouraud";
    const bool diagnosticFullRange = tverts_range == std::make_pair<size_t, size_t>(0, -1);
    // Never inspect unpublished geometry. The existing main-thread promotion
    // enables render only after acquiring the worker's ready flag.
    const bool diagnosticSmallAvailable = m_modelSmall && !m_modelSmall->is_render_disabled() && m_modelSmall->is_initialized();
    const bool diagnosticMiddleAvailable = m_modelMiddle && !m_modelMiddle->is_render_disabled() && m_modelMiddle->is_initialized();
    bool diagnosticUseMmuDisplayLod = false;
]=])
pd_replace("        color_volume = true;" [=[
        color_volume = true;
        // Only switch the display path when the requested LOD is ready.
        // Material-ID rendering (ban_light) and partial index ranges retain
        // their original MMU path. Picking preparation/drawing is unchanged.
        diagnosticUseMmuDisplayLod = diagnosticMmuDisplayLodEnabled && !picking && !ban_light && diagnosticMainDisplayShader && diagnosticFullRange &&
            ((m_curLodLevel == LODLevel::Small && diagnosticSmallAvailable) ||
             (m_curLodLevel == LODLevel::Middle && diagnosticMiddleAvailable));
        if (diagnosticUseMmuDisplayLod)
            break; // Defer MMU cache construction until the display needs it.
]=])
pd_replace("    if (color_volume && !picking) {" [=[
    if (color_volume && !picking && ::Slic3r::GUI::PickerDiag::enabled()) {
        const GUI::GLModel* diagnosticSelectedLod = diagnosticUseMmuDisplayLod
            ? (m_curLodLevel == LODLevel::Small ? m_modelSmall.get() : m_modelMiddle.get()) : nullptr;
        const char* diagnosticEffectivePath = !diagnosticUseMmuDisplayLod ? "MMU_HIGH"
            : (m_curLodLevel == LODLevel::Small ? "MMU_SMALL_PLAIN" : "MMU_MIDDLE_PLAIN");
        const char* diagnosticReason = diagnosticUseMmuDisplayLod ? "requested_lod_ready"
            : !diagnosticMmuDisplayLodEnabled ? "experiment_disabled"
            : ban_light ? "material_id_pass"
            : !diagnosticMainDisplayShader ? "other_shader_pass"
            : !diagnosticFullRange ? "partial_index_range"
            : m_curLodLevel == LODLevel::High ? "requested_high" : "requested_lod_unavailable";
        uint64_t diagnosticGeometryIndices = 0;
        uint64_t diagnosticRequestedIndices = 0;
        size_t diagnosticSubmodels = 0;
        bool diagnosticRangeValid = true;
        if (diagnosticSelectedLod != nullptr) {
            diagnosticGeometryIndices = diagnosticSelectedLod->indices_count();
            diagnosticRequestedIndices = diagnosticGeometryIndices;
            diagnosticSubmodels = 1;
        } else {
            for (const GUI::GLModel& diagnosticModel : mmuseg_models) {
                if (diagnosticModel.is_render_disabled() || !diagnosticModel.is_initialized())
                    continue;
                ++diagnosticSubmodels;
                const size_t diagnosticCount = diagnosticModel.indices_count();
                diagnosticGeometryIndices += diagnosticCount;
                if (diagnosticFullRange) {
                    diagnosticRequestedIndices += diagnosticCount;
                } else if (tverts_range.second >= tverts_range.first) {
                    // Report the original request; do not clamp or fix it here.
                    diagnosticRequestedIndices += tverts_range.second - tverts_range.first;
                    diagnosticRangeValid &= tverts_range.second <= diagnosticCount;
                } else {
                    diagnosticRangeValid = false;
                }
            }
        }
        PD_EVENT("MMU_LOD_SELECTION", "object=" << object_idx() << " instance=" << instance_idx()
            << " volume_index=" << volume_idx() << " requested_lod=" << static_cast<int>(m_curLodLevel)
            << " effective_path=" << diagnosticEffectivePath << " lod_applied=" << diagnosticUseMmuDisplayLod
            << " experiment_enabled=" << diagnosticMmuDisplayLodEnabled << " reason=" << diagnosticReason
            << " small_available=" << diagnosticSmallAvailable << " middle_available=" << diagnosticMiddleAvailable
            << " selected_geometry=" << diagnosticSelectedLod << " submodels=" << diagnosticSubmodels
            << " geometry_indices=" << diagnosticGeometryIndices << " requested_indices=" << diagnosticRequestedIndices
            << " requested_triangles=" << diagnosticRequestedIndices / 3 << " range_full=" << diagnosticFullRange
            << " range_valid=" << diagnosticRangeValid << " ban_light=" << ban_light
            << " main_display_shader=" << diagnosticMainDisplayShader
            << " single_color_display=" << diagnosticUseMmuDisplayLod << " gpu_completion_not_implied=1");
    }

    if (color_volume && !picking && !diagnosticUseMmuDisplayLod) {]=])

# Log the existing LOD decision inputs without changing thresholds, evaluation
# frequency, camera state, or the order of rendering passes. The volume tag
# ties LOD_SCREEN_SIZE to the following LOD_EVALUATED record.
pd_replace("            LODLevel prevLod = v->m_curLodLevel;" [=[
            PD_TAG(Volume, v);
            LODLevel prevLod = v->m_curLodLevel;]=])
pd_replace("    double sizeY = box2d.size().y();" [=[
    double sizeY = box2d.size().y();
    PD_EVENT("LOD_SCREEN_SIZE", "projected_width_px=" << sizeX << " projected_height_px=" << sizeY
        << " viewport_width=" << windowWidth << " viewport_height=" << windowHeight
        << " small_max_width=" << LOD_SCREEN_MIN.x() << " small_max_height=" << LOD_SCREEN_MIN.y()
        << " high_min_width=" << LOD_SCREEN_MAX.x() << " high_min_height=" << LOD_SCREEN_MAX.y());]=])
pd_replace("            PD_DETAIL(\"LOD_EVALUATED\"," "            PD_EVENT(\"LOD_EVALUATED\",")
message(STATUS "MMU display LOD experiment: default ON; ORCA_MMU_DISPLAY_LOD=0 restores full multicolor display. Picking unchanged.")
