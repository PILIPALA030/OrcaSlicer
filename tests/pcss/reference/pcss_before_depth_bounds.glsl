// Shared fragment implementation; inserted after #version by GLShadersManager.
// Directional-light PCSS: orthographic, standard depth, millimeters, visibility 1 = lit.
uniform bool pcss_enabled;
uniform sampler2D pcss_depth;
uniform mat4 pcss_matrix;
uniform vec2 pcss_extent;
uniform vec4 pcss_caster_uv_bounds;
uniform float pcss_depth_span;
uniform float pcss_min_caster_depth;
uniform float pcss_tan_half_angle;
uniform float pcss_bias_mm;
uniform float pcss_max_radius_mm;
uniform float pcss_plate_strength;
uniform int pcss_blocker_samples;
uniform int pcss_filter_samples;
uniform vec2 pcss_blocker_disk[64];
uniform vec2 pcss_filter_disk[64];

bool pcss_inside(vec2 uv)
{
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)));
}

float pcss_raw_depth(vec2 uv)
{
    ivec2 size = textureSize(pcss_depth, 0);
    return texelFetch(pcss_depth, clamp(ivec2(uv * vec2(size)), ivec2(0), size - ivec2(1)), 0).r;
}

struct PCSSReceiver
{
    vec3 coord;
    vec2 gradient;
};

// The inexpensive derivative setup is separate from texture sampling: callers can then discard clipped
// fragments or skip a light with zero contribution without evaluating derivatives in divergent control flow.
PCSSReceiver pcss_prepare_receiver(vec3 world_position)
{
    PCSSReceiver receiver;
    receiver.coord = (pcss_matrix * vec4(world_position, 1.0)).xyz * 0.5 + 0.5;
    vec3 dx = dFdx(receiver.coord);
    vec3 dy = dFdy(receiver.coord);
    float determinant = dx.x * dy.y - dx.y * dy.x;
    receiver.gradient = vec2(0.0);
    if (abs(determinant) > 1e-12)
        receiver.gradient = vec2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / determinant;
    return receiver;
}

float pcss_visibility_prepared(PCSSReceiver prepared)
{
    vec3 coord = prepared.coord;
    vec2 gradient = prepared.gradient;
    if (!pcss_enabled || !pcss_inside(coord.xy) || coord.z <= 0.0 || coord.z >= 1.0 || pcss_depth_span <= 0.0)
        return 1.0;
    float bias = pcss_bias_mm / pcss_depth_span;
    vec2 texel = 1.0 / vec2(textureSize(pcss_depth, 0));
    bias += min(dot(abs(gradient), texel) * 0.5, 0.5 / pcss_depth_span);
    if (pcss_tan_half_angle <= 0.0)
        return coord.z - bias <= pcss_raw_depth(coord.xy) ? 1.0 : 0.0;

    // 1. Conservative blocker search from the nearest possible caster plane.
    float search_mm = min(max(0.0, (coord.z - pcss_min_caster_depth) * pcss_depth_span) *
                          pcss_tan_half_angle, pcss_max_radius_mm);
    vec2 search_uv = max(vec2(search_mm) / pcss_extent, texel);
    // A whole search footprint outside all possible caster texels is provably unoccluded.
    // Include rasterization/rounding and nearest-texel quantization; never infer visibility from four random taps.
    vec2 guard = 2.0 * texel;
    if (any(lessThan(coord.xy + search_uv, pcss_caster_uv_bounds.xy - guard)) ||
        any(greaterThan(coord.xy - search_uv, pcss_caster_uv_bounds.zw + guard)))
        return 1.0;
    int blockers = 0;
    float gap_sum = 0.0;
    for (int i = 0; i < 64; ++i) {
        if (i >= pcss_blocker_samples)
            break;
        vec2 offset = pcss_blocker_disk[i] * search_uv;
        vec2 uv = coord.xy + offset;
        if (!pcss_inside(uv))
            continue;
        float blocker = pcss_raw_depth(uv);
        float receiver = coord.z + dot(gradient, offset);
        if (blocker < 1.0 && blocker < receiver - bias) {
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

    // 3. Average comparisons, using the same samples as the previous Vogel implementation.
    float visible = 0.0;
    for (int i = 0; i < 64; ++i) {
        if (i >= pcss_filter_samples)
            break;
        vec2 offset = pcss_filter_disk[i] * radius_uv;
        vec2 uv = coord.xy + offset;
        if (!pcss_inside(uv)) {
            visible += 1.0;
            continue;
        }
        visible += coord.z + dot(gradient, offset) - bias <= pcss_raw_depth(uv) ? 1.0 : 0.0;
    }
    return visible / float(pcss_filter_samples);
}

float pcss_visibility(vec3 world_position)
{
    return pcss_visibility_prepared(pcss_prepare_receiver(world_position));
}
