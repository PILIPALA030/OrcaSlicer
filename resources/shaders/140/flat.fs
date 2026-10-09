#version 140

uniform vec4 uniform_color;
uniform sampler2D shadow_map;
uniform mat4 shadow_matrix;
uniform bool shadow_enabled;
uniform vec2 shadow_map_texel_size;
uniform float shadow_light_size;
uniform float shadow_bias;

in vec4 shadow_position;

out vec4 out_color;

// PCSS v2: separate blocker search, penumbra estimation and variable-radius PCF.
// Keep this block identical in all six receivers (apart from texture2D/texture).
// Maximum: 9 blocker reads + 12 bilinear comparisons (4 reads each) = 57 reads.
// No blockers: 9 reads. No stochastic frame noise or extra render targets.
vec2 shadow_receiver_gradient()
{
    vec3 p = shadow_position.xyz / max(shadow_position.w, 0.000001) * 0.5 + 0.5;
    vec3 dx = dFdx(p);
    vec3 dy = dFdy(p);
    float determinant = dx.x * dy.y - dx.y * dy.x;
    if (abs(determinant) < 0.000000000001)
        return vec2(0.0);
    return vec2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / determinant;
}

float shadow_raw_depth(vec2 uv)
{
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0))))
        return 1.0;
    return texture(shadow_map, uv).r;
}

// Two balanced rings and a center sample. Do not take an unbalanced prefix
// of a larger Poisson table, and do not omit the center for thin casters.
vec2 shadow_search_offset(int i)
{
    if (i == 0) return vec2(0.0);
    if (i == 1) return vec2(0.35, 0.0);
    if (i == 2) return vec2(-0.35, 0.0);
    if (i == 3) return vec2(0.0, 0.35);
    if (i == 4) return vec2(0.0, -0.35);
    if (i == 5) return vec2(0.67, 0.67);
    if (i == 6) return vec2(-0.67, -0.67);
    if (i == 7) return vec2(-0.67, 0.67);
    return vec2(0.67, -0.67);
}

// Antipodal disk samples: deterministic, zero mean, three radial strata.
vec2 shadow_filter_offset(int i)
{
    if (i == 0) return vec2(0.288675, 0.0);
    if (i == 1) return vec2(-0.288675, 0.0);
    if (i == 2) return vec2(0.0, 0.500000);
    if (i == 3) return vec2(0.0, -0.500000);
    if (i == 4) return vec2(0.456435, 0.456435);
    if (i == 5) return vec2(-0.456435, -0.456435);
    if (i == 6) return vec2(-0.540062, 0.540062);
    if (i == 7) return vec2(0.540062, -0.540062);
    if (i == 8) return vec2(0.800103, 0.331414);
    if (i == 9) return vec2(-0.800103, -0.331414);
    if (i == 10) return vec2(-0.366391, 0.884558);
    return vec2(0.366391, -0.884558);
}

float shadow_compare(vec2 sample_uv, vec3 receiver, vec2 gradient, float bias)
{
    // Compare against the receiver plane AT the fetched texel center.
    float reference = receiver.z + dot(gradient, sample_uv - receiver.xy) - bias;
    return step(clamp(reference, 0.0, 1.0), shadow_raw_depth(sample_uv));
}

float shadow_bilinear_pcf(vec2 uv, vec3 receiver, vec2 gradient, float bias)
{
    vec2 pixel = uv / shadow_map_texel_size - 0.5;
    vec2 base = (floor(pixel) + 0.5) * shadow_map_texel_size;
    vec2 f = fract(pixel);
    vec2 ex = vec2(shadow_map_texel_size.x, 0.0);
    vec2 ey = vec2(0.0, shadow_map_texel_size.y);
    // Interpolate VISIBILITY, not raw depth. Works with the existing NEAREST
    // sampler on both legacy GL and GL 3.1, without changing texture state.
    float a = shadow_compare(base, receiver, gradient, bias);
    float b = shadow_compare(base + ex, receiver, gradient, bias);
    float c = shadow_compare(base + ey, receiver, gradient, bias);
    float d = shadow_compare(base + ex + ey, receiver, gradient, bias);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float pcss_shadow_factor(vec2 gradient)
{
    if (shadow_position.w <= 0.0)
        return 1.0;
    vec3 receiver = shadow_position.xyz / shadow_position.w * 0.5 + 0.5;
    if (any(lessThan(receiver, vec3(0.0))) || any(greaterThan(receiver, vec3(1.0))))
        return 1.0;

    // The current light is directional and its projection is ORTHOGRAPHIC.
    // Rows of P*V have lengths 2/width, 2/height, 2/(far-near).
    vec3 row_x = vec3(shadow_matrix[0][0], shadow_matrix[1][0], shadow_matrix[2][0]);
    vec3 row_y = vec3(shadow_matrix[0][1], shadow_matrix[1][1], shadow_matrix[2][1]);
    vec3 row_z = vec3(shadow_matrix[0][2], shadow_matrix[1][2], shadow_matrix[2][2]);
    float depth_scale = length(row_z);
    if (depth_scale < 0.00000001)
        return 1.0;
    vec2 penumbra_scale = max(shadow_light_size, 0.0) * vec2(length(row_x), length(row_y)) / depth_scale;
    float bias = min(shadow_bias, max(0.000001, 0.01 * depth_scale));
    vec2 max_radius = 8.0 * shadow_map_texel_size;

    // 1. Search independently of the final filter. The light's near plane
    // bounds potential separation; the eight-texel cap bounds the work/aliasing.
    vec2 search_radius = clamp(receiver.z * penumbra_scale,
                               1.5 * shadow_map_texel_size, max_radius);
    float gap_sum = 0.0;
    float blocker_count = 0.0;
    for (int i = 0; i < 9; ++i) {
        vec2 uv = receiver.xy + shadow_search_offset(i) * search_radius;
        uv = (floor(uv / shadow_map_texel_size) + 0.5) * shadow_map_texel_size;
        float depth = shadow_raw_depth(uv);
        float plane_depth = clamp(receiver.z + dot(gradient, uv - receiver.xy), 0.0, 1.0);
        if (depth < plane_depth - bias) {
            gap_sum += plane_depth - depth;
            blocker_count += 1.0;
        }
    }
    if (blocker_count < 0.5)
        return 1.0;

    // 2. Contact hardening: separationWorld * tan(angularRadius) / frustumSize.
    // Never divide by normalized blocker depth (its origin is arbitrary).
    vec2 radius = min((gap_sum / blocker_count) * penumbra_scale, max_radius);
    if (max(radius.x / shadow_map_texel_size.x, radius.y / shadow_map_texel_size.y) < 0.05)
        return shadow_bilinear_pcf(receiver.xy, receiver, gradient, bias);

    // 3. Resample across the computed footprint, not the old fixed 3x3 grid.
    // All blocker-search samples being blocked is NOT a safe umbra early-out.
    float visibility = 0.0;
    for (int i = 0; i < 12; ++i)
        visibility += shadow_bilinear_pcf(receiver.xy + shadow_filter_offset(i) * radius, receiver, gradient, bias);
    return visibility / 12.0;
}

void main()
{
    // Evaluate derivatives before non-uniform lighting branches.
    vec2 shadowGradient = shadow_enabled ? shadow_receiver_gradient() : vec2(0.0);
    float shadowFactor = shadow_enabled ? pcss_shadow_factor(shadowGradient) : 1.0;
    out_color = vec4(uniform_color.rgb * shadowFactor, uniform_color.a);
}
