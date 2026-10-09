#version 140

uniform vec4 uniform_color;
uniform sampler2D shadow_map;
uniform mat4 shadow_matrix;
uniform bool shadow_enabled;
uniform vec2 shadow_map_texel_size;
uniform float shadow_light_size;
uniform float shadow_bias;
uniform float emission_factor;

// x = tainted, y = specular;
in vec2 intensity;
in float top_diffuse;

in vec4 shadow_position;

out vec4 out_color;

// Bounded-cost PCSS. Keep this block identical in all six receiver shaders.
// Nine NEAREST depth reads serve both blocker search and variable-width PCF.
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

vec3 shadow_depth_row(vec2 uv)
{
    return vec3(shadow_raw_depth(uv - vec2(shadow_map_texel_size.x, 0.0)),
                shadow_raw_depth(uv),
                shadow_raw_depth(uv + vec2(shadow_map_texel_size.x, 0.0)));
}

vec3 shadow_area_weights(float fraction, float radius)
{
    // Exact overlap of the filter interval with three adjacent texel cells.
    // radius=0.5 is bilinear PCF; radius<=1.0 fits entirely in this 3x3 grid.
    // Interpolate comparison results, NEVER interpolate raw depths first.
    return max(min(vec3(0.0, 1.0, 2.0), vec3(fraction + radius)) -
               max(vec3(-1.0, 0.0, 1.0), vec3(fraction - radius)), vec3(0.0)) / (2.0 * radius);
}

float pcss_shadow_factor(vec2 gradient)
{
    if (shadow_position.w <= 0.0)
        return 1.0;
    vec3 projected = shadow_position.xyz / shadow_position.w * 0.5 + 0.5;
    if (any(lessThan(projected, vec3(0.0))) || any(greaterThan(projected, vec3(1.0))))
        return 1.0;

    // Recover orthographic units from the EXISTING light matrix. Its rows
    // have lengths 2/width, 2/height, 2/(far-near), independent of rotation.
    vec3 row_x = vec3(shadow_matrix[0][0], shadow_matrix[1][0], shadow_matrix[2][0]);
    vec3 row_y = vec3(shadow_matrix[0][1], shadow_matrix[1][1], shadow_matrix[2][1]);
    vec3 row_z = vec3(shadow_matrix[0][2], shadow_matrix[1][2], shadow_matrix[2][2]);
    float depth_scale = length(row_z);
    if (depth_scale < 0.00000001)
        return 1.0;
    vec2 penumbra_scale = max(shadow_light_size, 0.0) * vec2(length(row_x), length(row_y)) / depth_scale;
    // At most the existing bias, with a 0.02 mm base in linear light depth.
    float bias = min(shadow_bias, max(0.000001, 0.01 * depth_scale));
    vec2 texel_position = projected.xy / shadow_map_texel_size;
    vec2 center = (floor(texel_position) + 0.5) * shadow_map_texel_size;
    vec3 depth0 = shadow_depth_row(center - vec2(0.0, shadow_map_texel_size.y));
    vec3 depth1 = shadow_depth_row(center);
    vec3 depth2 = shadow_depth_row(center + vec2(0.0, shadow_map_texel_size.y));

    // Correct the receiver plane at every fetched texel center, before both
    // blocker classification and PCF. This avoids slope-dependent acne.
    float plane_center = dot(gradient, center - projected.xy);
    vec3 plane1 = vec3(plane_center) + gradient.x * shadow_map_texel_size.x * vec3(-1.0, 0.0, 1.0);
    vec3 plane0 = plane1 - vec3(gradient.y * shadow_map_texel_size.y);
    vec3 plane2 = plane1 + vec3(gradient.y * shadow_map_texel_size.y);
    vec3 lit0 = step(clamp(vec3(projected.z - bias) + plane0, vec3(0.0), vec3(1.0)), depth0);
    vec3 lit1 = step(clamp(vec3(projected.z - bias) + plane1, vec3(0.0), vec3(1.0)), depth1);
    vec3 lit2 = step(clamp(vec3(projected.z - bias) + plane2, vec3(0.0), vec3(1.0)), depth2);
    vec3 blocked0 = vec3(1.0) - lit0;
    vec3 blocked1 = vec3(1.0) - lit1;
    vec3 blocked2 = vec3(1.0) - lit2;
    float count = dot(blocked0 + blocked1 + blocked2, vec3(1.0));
    if (count < 0.5)
        return 1.0;
    if (count > 8.5)
        return 0.0;
    float average_blocker = (dot(depth0 - plane0, blocked0) + dot(depth1 - plane1, blocked1) +
                             dot(depth2 - plane2, blocked2)) / count;
    // Directional light: radiusWorld = separationWorld * tan(angularRadius).
    // Dividing by normalized blocker depth would depend on an arbitrary near plane.
    float gap = max(projected.z - average_blocker, 0.0);
    vec2 radius = clamp(gap * penumbra_scale / shadow_map_texel_size, vec2(0.5), vec2(1.0));
    vec2 fraction = fract(texel_position);
    vec3 wx = shadow_area_weights(fraction.x, radius.x);
    vec3 wy = shadow_area_weights(fraction.y, radius.y);
    return clamp(dot(lit0, wx) * wy.x + dot(lit1, wx) * wy.y + dot(lit2, wx) * wy.z, 0.0, 1.0);
}

void main()
{
    // Evaluate derivatives before non-uniform lighting branches.
    vec2 shadowGradient = shadow_enabled ? shadow_receiver_gradient() : vec2(0.0);
    float shadowFactor = (shadow_enabled && top_diffuse > 0.0) ? pcss_shadow_factor(shadowGradient) : 1.0;
    float lighting = intensity.x - top_diffuse * (1.0 - shadowFactor);
    out_color = vec4(vec3(intensity.y) + uniform_color.rgb * (lighting + emission_factor), uniform_color.a);
}
