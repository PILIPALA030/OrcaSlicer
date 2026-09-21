#version 140
in vec3 v_position;
uniform mat4 view_projection_matrix;
uniform float plate_surface_z;
out vec3 plate_world_position;
void main()
{
    // PartPlate vertices already include its XY origin; do not translate a second time.
    plate_world_position = vec3(v_position.xy, plate_surface_z);
    gl_Position = view_projection_matrix * vec4(plate_world_position, 1.0);
}
