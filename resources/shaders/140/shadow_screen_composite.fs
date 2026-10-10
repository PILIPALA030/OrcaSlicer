#version 140

// Full resolution scene depth (background = 1.0)
uniform sampler2D scene_depth;
// Possibly lower resolution result: R = shadow amount, G = linear view depth (0 = no object), BA = octahedral normal
uniform sampler2D object_shadow;
uniform mat4 inv_projection;
uniform mat4 inv_view_projection;
uniform vec3 to_camera;
// Full resolution pixels per shadow pixel (1 or 2)
uniform int resolution_scale;
// Neighbours whose depth differs more than this fraction are ignored, so shadows do not bleed over silhouettes
uniform float depth_tolerance;

in vec2 tex_coord;

out vec4 frag_color;

// Gaussian falloff in shadow pixels; 3x3 taps remove isolated acne pixels and soften noise
const float BLUR_SIGMA = 1.0;
// Normal agreement range: samples from another face (e.g. across a cube edge) are dropped
const float NORMAL_REJECT_LOW = 0.8;
const float NORMAL_REJECT_HIGH = 0.95;

ivec2 g_depth_size;

float FetchDepth(ivec2 c)
{
    return texelFetch(scene_depth, clamp(c, ivec2(0), g_depth_size - ivec2(1)), 0).r;
}

vec3 WorldFromDepth(ivec2 c, float d)
{
    vec2 uv = (vec2(c) + 0.5) / vec2(g_depth_size);
    vec4 p = inv_view_projection * vec4(vec3(uv, d) * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

float LinearDepth(float d)
{
    vec4 v = inv_projection * vec4(0.0, 0.0, d * 2.0 - 1.0, 1.0);
    return -v.z / v.w;
}

// Same reconstruction as the shading pass, so both sides agree on which face a pixel belongs to
vec3 ReconstructNormal(ivec2 c, float d, float dl, float dr, float db, float dt)
{
    vec3 pos = WorldFromDepth(c, d);
    ivec2 cx = ivec2(1, 0);
    ivec2 cy = ivec2(0, 1);
    vec3 ddx = (abs(dr - d) < abs(d - dl)) ? WorldFromDepth(c + cx, dr) - pos : pos - WorldFromDepth(c - cx, dl);
    vec3 ddy = (abs(dt - d) < abs(d - db)) ? WorldFromDepth(c + cy, dt) - pos : pos - WorldFromDepth(c - cy, db);
    vec3 n = normalize(cross(ddx, ddy));
    return (dot(n, to_camera) < 0.0) ? -n : n;
}

vec3 OctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (vec2(1.0) - abs(e.yx)) * vec2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

void main()
{
    g_depth_size = textureSize(scene_depth, 0);
    ivec2 c = clamp(ivec2(tex_coord * vec2(g_depth_size)), ivec2(0), g_depth_size - ivec2(1));
    float d = FetchDepth(c);
    if (d >= 1.0)
        discard;

    float view_depth = LinearDepth(d);
    float dl = FetchDepth(c - ivec2(1, 0));
    float dr = FetchDepth(c + ivec2(1, 0));
    float db = FetchDepth(c - ivec2(0, 1));
    float dt = FetchDepth(c + ivec2(0, 1));
    vec3 n = ReconstructNormal(c, d, dl, dr, db, dt);
    ivec2 size = textureSize(object_shadow, 0);
    // Shadow pixel i was evaluated at full resolution pixel i * resolution_scale
    vec2 p = vec2(c) / float(resolution_scale);
    ivec2 center = ivec2(floor(p + 0.5));
    float inv_two_sigma2 = 1.0 / (2.0 * BLUR_SIGMA * BLUR_SIGMA);

    // Depth and normal aware 3x3 Gaussian; falls back to the best matching neighbour
    float sum = 0.0;
    float weight_sum = 0.0;
    float best_score = -1.0e30;
    float best_shadow = 0.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            ivec2 s_coord = clamp(center + ivec2(i, j), ivec2(0), size - ivec2(1));
            vec4 s = texelFetch(object_shadow, s_coord, 0);
            if (s.g <= 0.0)
                continue;

            float rel_diff = abs(s.g - view_depth) / view_depth;
            float normal_dot = dot(n, OctDecode(s.ba));
            float score = normal_dot - rel_diff / depth_tolerance;
            if (score > best_score) {
                best_score = score;
                best_shadow = s.r;
            }
            if (rel_diff <= depth_tolerance) {
                vec2 delta = vec2(s_coord) - p;
                float normal_weight = smoothstep(NORMAL_REJECT_LOW, NORMAL_REJECT_HIGH, normal_dot);
                float w = exp(-dot(delta, delta) * inv_two_sigma2) * normal_weight;
                sum += s.r * w;
                weight_sum += w;
            }
        }
    }

    float shadow = (weight_sum > 1.0e-4) ? sum / weight_sum : best_shadow;
    if (shadow < 0.004)
        discard;

    // Multiplied into the frame buffer (blend DST_COLOR, ZERO)
    frag_color = vec4(vec3(1.0 - shadow), 1.0);
}
