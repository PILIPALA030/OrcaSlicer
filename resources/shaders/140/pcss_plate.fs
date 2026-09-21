#version 140
in vec3 plate_world_position;
out vec4 out_color;
void main()
{
    float visibility = pcss_visibility(plate_world_position);
    out_color = vec4(0.0, 0.0, 0.0, pcss_plate_strength * (1.0 - visibility));
}
