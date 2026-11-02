#version 140

// One separable Gaussian pass of the PBR glow halo, RGB.
uniform sampler2D source_texture;
// Distance between taps in uv, along the blur direction.
uniform vec2 step_uv;

in vec2 tex_coord;
out vec4 frag_color;

// Normalized weights of taps 0..4, sigma = 2 taps.
const float WEIGHTS[5] = float[](0.2042, 0.1802, 0.1238, 0.0663, 0.0276);

void main()
{
    vec3 color = texture(source_texture, tex_coord).rgb * WEIGHTS[0];
    for (int i = 1; i < 5; ++i) {
        vec2 offset = step_uv * float(i);
        color += (texture(source_texture, tex_coord + offset).rgb + texture(source_texture, tex_coord - offset).rgb) * WEIGHTS[i];
    }
    frag_color = vec4(color, 1.0);
}
