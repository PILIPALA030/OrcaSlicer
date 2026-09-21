#version 140
in vec3 v_position;
uniform mat4 volume_world_matrix;
uniform mat4 pcss_matrix;
out vec3 shadow_world_position;
void main()
{
    vec4 world = volume_world_matrix * vec4(v_position, 1.0);
    shadow_world_position = world.xyz;
    gl_Position = pcss_matrix * world;
}
