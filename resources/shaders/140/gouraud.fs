#version 140

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
//BBS: add grey and orange
//const vec3 GREY = vec3(0.9, 0.9, 0.9);
const vec3 ORANGE = vec3(0.8, 0.4, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
const float EPSILON = 0.0001;

struct PrintVolumeDetection
{
	// 0 = rectangle, 1 = circle, 2 = custom, 3 = invalid
	int type;
    // type = 0 (rectangle):
    // x = min.x, y = min.y, z = max.x, w = max.y
    // type = 1 (circle):
    // x = center.x, y = center.y, z = radius
	vec4 xy_data;
    // x = min z, y = max z
	vec2 z_data;
};

struct SlopeDetection
{
    bool actived;
	float normal_z;
    mat3 volume_world_normal_matrix;
};

uniform vec4 uniform_color;
uniform bool use_color_clip_plane;
uniform vec4 uniform_color_clip_plane_1;
uniform vec4 uniform_color_clip_plane_2;
uniform SlopeDetection slope;
uniform sampler2D shadow_map;
uniform mat4 shadow_matrix;
uniform bool shadow_enabled;
uniform vec2 shadow_map_texel_size;
uniform float shadow_light_size;
uniform float shadow_bias;

#ifdef ENABLE_ENVIRONMENT_MAP
    uniform sampler2D environment_tex;
    uniform bool use_environment_tex;
#endif // ENABLE_ENVIRONMENT_MAP

uniform PrintVolumeDetection print_volume;

in vec3 clipping_planes_dots;
in float color_clip_plane_dot;

// x = diffuse, y = specular;
in vec2 intensity;

in vec4 world_pos;
in float world_normal_z;
in vec3 eye_normal;
in vec4 shadow_position;
in float top_diffuse;

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
    // Derivatives must precede discard and non-uniform lighting branches.
    vec2 shadowGradient = shadow_enabled ? shadow_receiver_gradient() : vec2(0.0);
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    vec4 color;
	if (use_color_clip_plane) {
		color.rgb = (color_clip_plane_dot < 0.0) ? uniform_color_clip_plane_1.rgb : uniform_color_clip_plane_2.rgb;
		color.a = uniform_color.a;
    }
    else
	    color = uniform_color;

    if (slope.actived) {
         if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
                color.rgb = LightBlue;
                color.a = 0.8;
         }
         else if( world_normal_z < slope.normal_z - EPSILON)
         {
                color.rgb = color.rgb * 0.5 + LightRed * 0.5;
                color.a = 0.8;
         }
    }
    // if the fragment is outside the print volume -> use darker color
	vec3 pv_check_min = ZERO;
	vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {
		// rectangle
		pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
		pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
	}
	else if (print_volume.type == 1) {
		// circle
		float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
		pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
		pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
	}
	color.rgb = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color.rgb, ZERO, 0.3333) : color.rgb;

    float shadowFactor = (shadow_enabled && top_diffuse > 0.0) ? pcss_shadow_factor(shadowGradient) : 1.0;
    float lighting = intensity.x - top_diffuse * (1.0 - shadowFactor);

#ifdef ENABLE_ENVIRONMENT_MAP
    if (use_environment_tex)
        out_color = vec4(0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz +
                         0.8 * color.rgb * lighting, color.a);
    else
#endif
        out_color = vec4(vec3(intensity.y) + color.rgb * lighting, color.a);
}
