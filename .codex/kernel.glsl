// Keep this block identical in flat/gouraud/gouraud_light (GLSL 110/140).
// The regression test checks that the six copies have not diverged.
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

vec2 shadow_disk_offset(int index)
{
    // Balanced pairs, not the asymmetric prefix of a larger Poisson array.
    if (index == 0) return vec2( 0.325,  0.140);
    if (index == 1) return vec2(-0.325, -0.140);
    if (index == 2) return vec2(-0.280,  0.550);
    if (index == 3) return vec2( 0.280, -0.550);
    if (index == 4) return vec2( 0.720, -0.430);
    if (index == 5) return vec2(-0.720,  0.430);
    if (index == 6) return vec2( 0.420,  0.860);
    return vec2(-0.420, -0.860);
}

float shadow_raw_depth(vec2 uv)
{
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0))))
        return 1.0;
    return SHADOW_TEXTURE(shadow_map, uv).r;
}

float shadow_point_compare(vec2 sample_uv, vec2 receiver_uv, float receiver_depth, vec2 gradient)
{
    float reference = receiver_depth + dot(gradient, sample_uv - receiver_uv) - shadow_bias;
    return step(clamp(reference, 0.0, 1.0), shadow_raw_depth(sample_uv));
}

float shadow_bilinear_compare(vec2 sample_uv, vec2 receiver_uv, float receiver_depth, vec2 gradient)
{
    // Compare depths FIRST, then interpolate visibility. GL_LINEAR on raw
    // depths is not percentage-closer filtering.
    vec2 position = sample_uv / shadow_map_texel_size - 0.5;
    vec2 weight = fract(position);
    vec2 base = (floor(position) + 0.5) * shadow_map_texel_size;
    float a = shadow_point_compare(base, receiver_uv, receiver_depth, gradient);
    float b = shadow_point_compare(base + vec2(shadow_map_texel_size.x, 0.0), receiver_uv, receiver_depth, gradient);
    float c = shadow_point_compare(base + vec2(0.0, shadow_map_texel_size.y), receiver_uv, receiver_depth, gradient);
    float d = shadow_point_compare(base + shadow_map_texel_size, receiver_uv, receiver_depth, gradient);
    return mix(mix(a, b, weight.x), mix(c, d, weight.x), weight.y);
}

float shadow_hardware_compare(vec3 coordinates)
{
    return SHADOW_COMPARE(shadow_map_pcf, coordinates);
}

float pcss_shadow_factor(vec2 gradient)
{
    if (shadow_position.w <= 0.0)
        return 1.0;
    vec3 projected = shadow_position.xyz / shadow_position.w * 0.5 + 0.5;
    if (any(lessThan(projected, vec3(0.0))) || any(greaterThan(projected, vec3(1.0))))
        return 1.0;
    vec2 uv = projected.xy;
    float receiver_depth = projected.z;
    // Fixed screen-pixel seed: no time/frame index and no noise texture.
    float angle = 6.28318530718 * fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy), vec2(0.06711056, 0.00583715))));
    float c = cos(angle);
    float s = sin(angle);
    mat2 rotation = mat2(c, -s, s, c);
    float radius_limit = shadow_hardware_pcf ? 4.0 : 2.0;
    vec2 search_radius = clamp(receiver_depth * shadow_penumbra_scale / shadow_map_texel_size,
                               vec2(0.75), vec2(radius_limit)) * shadow_map_texel_size;
    float blocker_sum = 0.0;
    float blocker_count = 0.0;
    // Include the center so thin casters are not missed between four offsets.
    for (int i = 0; i < 5; ++i) {
        vec2 offset = vec2(0.0);
        if (i == 1) offset = vec2( 0.70710678, 0.0);
        if (i == 2) offset = vec2(-0.70710678, 0.0);
        if (i == 3) offset = vec2(0.0,  0.70710678);
        if (i == 4) offset = vec2(0.0, -0.70710678);
        vec2 sample_uv = uv + rotation * offset * search_radius;
        sample_uv = (floor(sample_uv / shadow_map_texel_size) + 0.5) * shadow_map_texel_size;
        float sample_depth = shadow_raw_depth(sample_uv);
        float plane_offset = dot(gradient, sample_uv - uv);
        if (sample_depth < 1.0 && sample_depth < receiver_depth + plane_offset - shadow_bias) {
            blocker_sum += sample_depth - plane_offset;
            blocker_count += 1.0;
        }
    }
    if (blocker_count < 0.5)
        return 1.0;
    float gap = max(receiver_depth - blocker_sum / blocker_count, 0.0);
    // Directional/orthographic: radiusWorld = separation * tan(angularRadius).
    // CPU scale = (far-near) * tan(angularRadius) / orthographicWidth.
    // Never divide by blocker depth measured from an arbitrary near plane.
    vec2 radius_texels = gap * shadow_penumbra_scale / shadow_map_texel_size;
    vec2 filter_radius = clamp(radius_texels, vec2(0.5), vec2(radius_limit)) * shadow_map_texel_size;
    float lit = 0.0;
    if (shadow_hardware_pcf) {
        // One texel of plane bias covers the hardware comparison footprint.
        float bias = shadow_bias + dot(abs(gradient), shadow_map_texel_size);
        // One bilinear footprint suffices for sub-texel penumbrae.
        if (max(radius_texels.x, radius_texels.y) <= 0.5)
            return shadow_hardware_compare(vec3(uv, clamp(receiver_depth - bias, 0.0, 1.0)));
        for (int i = 0; i < 8; ++i) {
            vec2 offset = rotation * shadow_disk_offset(i) * filter_radius;
            float reference = clamp(receiver_depth + dot(gradient, offset) - bias, 0.0, 1.0);
            lit += shadow_hardware_compare(vec3(uv + offset, reference));
        }
        return lit * 0.125;
    }
    // Legacy: four bilinear taps / 16 raw fetches, not 32 or 64 filter taps.
    if (max(radius_texels.x, radius_texels.y) <= 0.5)
        return shadow_bilinear_compare(uv, uv, receiver_depth, gradient);
    for (int i = 0; i < 4; ++i) {
        vec2 offset = rotation * shadow_disk_offset(i) * filter_radius;
        lit += shadow_bilinear_compare(uv + offset, uv, receiver_depth, gradient);
    }
    return lit * 0.25;
}
