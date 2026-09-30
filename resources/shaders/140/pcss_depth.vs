#version 140
in vec3 v_position;
uniform mat4 volume_world_matrix;
uniform mat4 pcss_matrix;
uniform vec4 clipping_plane;
uniform vec2 z_range;
// Active uniform is also the renderer's capability marker for this vertex-clipped variant.
uniform bool pcss_vertex_clipping;
out float gl_ClipDistance[3];
void main()
{
    vec4 world = volume_world_matrix * vec4(v_position, 1.0);
    gl_Position = pcss_matrix * world;
    gl_ClipDistance[0] = pcss_vertex_clipping ? dot(world, clipping_plane) : 1.0;
    // Avoid FLT_MAX sentinel interpolation when a clipping half-space is disabled.
    gl_ClipDistance[1] = pcss_vertex_clipping && z_range.x > -3.402823466e38 ? world.z - z_range.x : 1.0;
    gl_ClipDistance[2] = pcss_vertex_clipping && z_range.y < 3.402823466e38 ? z_range.y - world.z : 1.0;
}
