#version 140

// Same depth texture bound twice: raw depth for blocker search, compare mode for hardware PCF
uniform sampler2D shadow_depth;
uniform sampler2DShadow shadow_depth_cmp;
uniform mat4 light_view_projection;
uniform float ground_z;
// World length of the light depth range and world size of the light ortho XY
uniform float depth_span;
uniform vec2 light_extent;
// Tangent of the light angular radius
uniform float light_spread;
uniform float min_penumbra;
uniform float max_search;
uniform float depth_bias;
uniform int blocker_samples;
uniform int pcf_samples;

in vec2 world_xy;

out vec4 frag_color;

const float GOLDEN_ANGLE = 2.39996323;
const float TWO_PI = 6.28318531;
// Squared radius of the inner blocker ring (half of the search radius)
const float INNER_RING_R2 = 0.25;

float InterleavedGradientNoise(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec2 VogelDisk(int i, int n, float phi)
{
    float r = sqrt((float(i) + 0.5) / float(n));
    float theta = float(i) * GOLDEN_ANGLE + phi;
    return r * vec2(cos(theta), sin(theta));
}

void main()
{
    vec4 lp = light_view_projection * vec4(world_xy, ground_z, 1.0);
    vec3 p = lp.xyz / lp.w * 0.5 + 0.5;
    // Outside the light frustum (including depth) nothing can occlude the point
    if (any(lessThan(p, vec3(0.0))) || any(greaterThan(p, vec3(1.0)))) {
        frag_color = vec4(1.0);
        return;
    }

    float receiver = p.z - depth_bias;
    float phi = InterleavedGradientNoise(gl_FragCoord.xy) * TWO_PI;

    // Step 1: blocker search
    vec2 search_uv = vec2(min(p.z * depth_span * light_spread, max_search)) / light_extent;
    // Blockers near the search center weigh more; when the inner ring finds any, only those are used,
    // so contact points stay hard instead of being softened by distant tall blockers
    float blocker_sum = 0.0;
    float blocker_weight = 0.0;
    float inner_sum = 0.0;
    float inner_weight = 0.0;
    int blocker_count = 0;
    for (int i = 0; i < blocker_samples; ++i) {
        float d = texture(shadow_depth, p.xy + VogelDisk(i, blocker_samples, phi) * search_uv).r;
        if (d < receiver) {
            float r2 = (float(i) + 0.5) / float(blocker_samples);
            float w = 1.0 - r2;
            blocker_sum += d * w;
            blocker_weight += w;
            if (r2 < INNER_RING_R2) {
                inner_sum += d * w;
                inner_weight += w;
            }
            ++blocker_count;
        }
    }
    if (blocker_count == 0) {
        frag_color = vec4(1.0);
        return;
    }
    // The PCF region never exceeds the search region, so a fully blocked search means umbra
    if (blocker_count == blocker_samples) {
        frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Step 2: penumbra estimation (directional light, parallel planes)
    float avg_blocker = (inner_weight > 0.0) ? inner_sum / inner_weight : blocker_sum / blocker_weight;
    float penumbra = clamp((p.z - avg_blocker) * depth_span * light_spread, min_penumbra, max_search);
    vec2 filter_uv = vec2(penumbra) / light_extent;

    // Step 3: PCF, each tap is a hardware 2x2 compare
    float lit = 0.0;
    for (int i = 0; i < pcf_samples; ++i)
        lit += texture(shadow_depth_cmp, vec3(p.xy + VogelDisk(i, pcf_samples, phi) * filter_uv, receiver));

    frag_color = vec4(vec3(lit / float(pcf_samples)), 1.0);
}
