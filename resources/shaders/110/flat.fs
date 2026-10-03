#version 110

uniform vec4 uniform_color;
uniform sampler2D shadow_map;
uniform bool shadow_enabled;
uniform vec2 shadow_map_texel_size;
uniform float shadow_light_size;
uniform float shadow_bias;

varying vec4 shadow_position;

vec2 shadow_poisson_offset(int index)
{
    if (index == 0) return vec2(-0.94201624, -0.39906216);
    if (index == 1) return vec2(0.94558609, -0.76890725);
    if (index == 2) return vec2(-0.09418410, -0.92938870);
    if (index == 3) return vec2(0.34495938, 0.29387760);
    if (index == 4) return vec2(-0.91588581, 0.45771432);
    if (index == 5) return vec2(-0.81544232, -0.87912464);
    if (index == 6) return vec2(-0.38277543, 0.27676845);
    return vec2(0.97484398, 0.75648379);
}

float pcss_shadow_factor()
{
    if (shadow_position.w <= 0.0)
        return 1.0;
    vec3 projected = shadow_position.xyz / shadow_position.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;

    float receiverDepth = projected.z * 0.5 + 0.5;
    float searchRadius = shadow_light_size * (1.0 + receiverDepth * 2.0) * 32.0;
    float blockerDepth = 0.0;
    float blockerCount = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 sampleUv = uv + shadow_poisson_offset(i) * shadow_map_texel_size * searchRadius;
        float sampleDepth = texture2D(shadow_map, sampleUv).r;
        if (sampleDepth + shadow_bias < receiverDepth) {
            blockerDepth += sampleDepth;
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5)
        return 1.0;

    blockerDepth /= blockerCount;
    float penumbra = (receiverDepth - blockerDepth) / max(blockerDepth, 0.05);
    float filterRadius = clamp(penumbra * shadow_light_size * 32.0, 1.0, 6.0);
    float lit = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 sampleUv = uv + shadow_poisson_offset(i) * shadow_map_texel_size * filterRadius;
        lit += texture2D(shadow_map, sampleUv).r + shadow_bias >= receiverDepth ? 1.0 : 0.0;
    }
    return lit / 4.0;
}

void main()
{
    float shadowFactor = shadow_enabled ? pcss_shadow_factor() : 1.0;
    gl_FragColor = vec4(uniform_color.rgb * shadowFactor, uniform_color.a);
}
