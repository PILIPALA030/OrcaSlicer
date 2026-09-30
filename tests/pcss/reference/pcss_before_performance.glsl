// Shared fragment implementation; inserted after #version by GLShadersManager.
// Directional-light PCSS: orthographic, standard depth, millimeters, visibility 1 = lit.
uniform bool pcss_enabled;
uniform sampler2D pcss_depth;
uniform mat4 pcss_matrix;
uniform vec2 pcss_extent;
uniform float pcss_depth_span;
uniform float pcss_min_caster_depth;
uniform float pcss_tan_half_angle;
uniform float pcss_bias_mm;
uniform float pcss_max_radius_mm;
uniform float pcss_plate_strength;
uniform int pcss_blocker_samples;
uniform int pcss_filter_samples;

vec2 pcss_disk(int index, int count)
{
    // Deterministic Vogel disk, not a Poisson table and not temporal accumulation.
    float radius = sqrt((float(index) + 0.5) / float(count));
    float angle = float(index) * 2.399963229728653;
    return radius * vec2(cos(angle), sin(angle));
}

bool pcss_inside(vec2 uv)
{
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)));
}

float pcss_raw_depth(vec2 uv)
{
    ivec2 size = textureSize(pcss_depth, 0);
    return texelFetch(pcss_depth, clamp(ivec2(uv * vec2(size)), ivec2(0), size - ivec2(1)), 0).r;
}

float pcss_visibility(vec3 world_position)
{
    vec3 coord = (pcss_matrix * vec4(world_position, 1.0)).xyz * 0.5 + 0.5;
    // Evaluate derivatives before any nonuniform branch or the caller's clipping discard.
    vec3 dx = dFdx(coord);
    vec3 dy = dFdy(coord);
    float determinant = dx.x * dy.y - dx.y * dy.x;
    vec2 gradient = vec2(0.0);
    if (abs(determinant) > 1e-12)
        gradient = vec2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / determinant;
    if (!pcss_enabled || !pcss_inside(coord.xy) || coord.z <= 0.0 || coord.z >= 1.0 || pcss_depth_span <= 0.0)
        return 1.0;
    float bias = pcss_bias_mm / pcss_depth_span;
    vec2 texel = 1.0 / vec2(textureSize(pcss_depth, 0));
    // Bound the fractional-texel term near degenerate/grazing projections.
    bias += min(dot(abs(gradient), texel) * 0.5, 0.5 / pcss_depth_span);
    if (pcss_tan_half_angle <= 0.0)
        return coord.z - bias <= pcss_raw_depth(coord.xy) ? 1.0 : 0.0;

    // 1. Conservative blocker search from the nearest possible caster plane.
    float search_mm = min(max(0.0, (coord.z - pcss_min_caster_depth) * pcss_depth_span) *
                          pcss_tan_half_angle, pcss_max_radius_mm);
    vec2 search_uv = max(vec2(search_mm) / pcss_extent, texel);
    int blockers = 0;
    float gap_sum = 0.0;
    for (int i = 0; i < 64; ++i) {
        if (i >= pcss_blocker_samples)
            break;
        vec2 offset = pcss_disk(i, pcss_blocker_samples) * search_uv;
        vec2 uv = coord.xy + offset;
        if (!pcss_inside(uv))
            continue;
        float blocker = pcss_raw_depth(uv);
        float receiver = coord.z + dot(gradient, offset);
        if (blocker < 1.0 && blocker < receiver - bias) {
            // Normalize to the local receiver plane before averaging the blocker gap.
            gap_sum += max(0.0, receiver - blocker);
            ++blockers;
        }
    }
    if (blockers == 0)
        return 1.0;

    // 2. Restore metric separation before computing the filter radius.
    float gap_mm = gap_sum * pcss_depth_span / float(blockers);
    float radius_mm = min(gap_mm * pcss_tan_half_angle, pcss_max_radius_mm);
    vec2 radius_uv = vec2(radius_mm) / pcss_extent;

    // 3. Average comparisons, never a linearly interpolated depth value.
    float visible = 0.0;
    for (int i = 0; i < 64; ++i) {
        if (i >= pcss_filter_samples)
            break;
        vec2 offset = pcss_disk(i, pcss_filter_samples) * radius_uv;
        vec2 uv = coord.xy + offset;
        if (!pcss_inside(uv)) {
            visible += 1.0;
            continue;
        }
        visible += coord.z + dot(gradient, offset) - bias <= pcss_raw_depth(uv) ? 1.0 : 0.0;
    }
    return visible / float(pcss_filter_samples);
}
