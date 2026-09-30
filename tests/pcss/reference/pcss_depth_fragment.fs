#version 140
in vec3 shadow_world_position;
uniform vec4 clipping_plane;
uniform vec2 z_range;
void main()
{
    if (dot(vec4(shadow_world_position, 1.0), clipping_plane) < 0.0 ||
        shadow_world_position.z < z_range.x || shadow_world_position.z > z_range.y)
        discard;
    // Fixed-function depth writes the nearest surface; no color attachment is required.
}
