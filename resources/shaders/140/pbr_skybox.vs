#version 140

in vec3 v_position;

// Normalized device coordinates of the full screen quad.
out vec2 ndc;

void main()
{
    ndc = v_position.xy;
    gl_Position = vec4(v_position.xy, 0.0, 1.0);
}
