pd_entry("void GLModel::render(const std::pair<size_t, size_t>& range)" [=[
    PD_TAG(Geometry, this);
    PD_VERBOSE_SCOPE("GLModel.render");
]=])
pd_replace("    // sends data to gpu if not done yet" [=[
    PD_DETAIL("MODEL_DRAW_INPUT", "range_begin=" << range.first << " range_end=" << range.second
        << " vertices=" << vertices_count() << " indices=" << indices_count()
        << " vbo=" << m_render_data.vbo_id << " ibo=" << m_render_data.ibo_id
        << " cpu_vertex_capacity_bytes=" << m_render_data.geometry.vertices.capacity()*sizeof(float)
        << " cpu_index_capacity_bytes=" << m_render_data.geometry.indices.capacity()*sizeof(unsigned int));
    // sends data to gpu if not done yet]=])
pd_entry("void GLModel::render_instanced(" [=[
    PD_TAG(Geometry, this);
    PD_VERBOSE_SCOPE("GLModel.render_instanced");
]=])
pd_entry("bool GLModel::send_to_gpu(" [=[
    PD_TAG(Geometry, this);
    PD_SCOPE("GLModel.send_to_gpu");
    PD_EVENT("UPLOAD_MODEL", "vbo_before=" << m_render_data.vbo_id << " ibo_before=" << m_render_data.ibo_id
        << " vertices=" << vertices_count() << " indices=" << indices_count()
        << " cpu_vertex_bytes=" << m_render_data.geometry.vertices.size()*sizeof(float)
        << " cpu_index_bytes=" << m_render_data.geometry.indices.size()*sizeof(unsigned int));
]=])
pd_replace("data.vertices = std::vector<float>();" "PD_CALL(\"release_cpu_vertices\", (data.vertices = std::vector<float>()));")
pd_replace("data.indices = std::vector<unsigned int>();" "PD_CALL(\"release_cpu_indices\", (data.indices = std::vector<unsigned int>()));")
pd_replace("std::vector<unsigned char> reduced_indices(indices_count);" "std::vector<unsigned char> reduced_indices = PD_CALL(\"allocate_u8_indices\", std::vector<unsigned char>(indices_count));")
pd_replace("std::vector<unsigned short> reduced_indices(indices_count);" "std::vector<unsigned short> reduced_indices = PD_CALL(\"allocate_u16_indices\", std::vector<unsigned short>(indices_count));")
pd_entry("void GLModel::init_from(const indexed_triangle_set& its)" [=[
    PD_TAG(Geometry, this);
    PD_SCOPE("GLModel.init_from_mesh");
    PD_EVENT("GEOMETRY_BUILD", "input_vertices=" << its.vertices.size() << " input_faces=" << its.indices.size());
]=])
pd_entry("void GLModel::init_from(Geometry&& data)" [=[
    PD_TAG(Geometry, this);
    PD_VERBOSE_SCOPE("GLModel.init_from_geometry");
]=])
pd_entry("void GLModel::reset(" [=[
    PD_TAG(Geometry, this);
    PD_VERBOSE_SCOPE("GLModel.reset");
    PD_DETAIL("GEOMETRY_RELEASE", "vbo=" << m_render_data.vbo_id << " ibo=" << m_render_data.ibo_id);
    ::Slic3r::GUI::PickerDiag::retire_geometry(this);
]=])
