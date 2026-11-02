#version 140

// Adds the blurred glow of PBR emissive materials over the scene (additive blending).
uniform sampler2D glow_texture;
uniform float intensity;

in vec2 tex_coord;
out vec4 frag_color;

void main()
{
    frag_color = vec4(texture(glow_texture, tex_coord).rgb * intensity, 1.0);
}
