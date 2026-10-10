#version 140

// Ground shadow mask to denoise (R = visibility)
uniform sampler2D source_mask;
// (1, 0) for the horizontal pass, (0, 1) for the vertical pass
uniform ivec2 direction;

out vec4 frag_color;

// Normalized Gaussian (sigma = 1 texel) for offsets 0, 1, 2; removes the grain where penumbrae of
// different blockers overlap while keeping contact edges within about one texel
const float WEIGHT_0 = 0.4026;
const float WEIGHT_1 = 0.2442;
const float WEIGHT_2 = 0.0545;

float FetchMask(ivec2 c, ivec2 size)
{
    return texelFetch(source_mask, clamp(c, ivec2(0), size - ivec2(1)), 0).r;
}

void main()
{
    ivec2 size = textureSize(source_mask, 0);
    ivec2 c = ivec2(gl_FragCoord.xy);
    float v = FetchMask(c, size) * WEIGHT_0;
    v += (FetchMask(c + direction, size) + FetchMask(c - direction, size)) * WEIGHT_1;
    v += (FetchMask(c + 2 * direction, size) + FetchMask(c - 2 * direction, size)) * WEIGHT_2;
    frag_color = vec4(vec3(v), 1.0);
}
