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
in vec3 eye_position;
in vec3 world_normal;

// Debug view: world space normal as color (x -> red, y -> green, z -> blue).
uniform bool debug_normal_color;
// PBR (metallic-roughness) shading, enabled for Prepare/Assemble views by preference.
uniform bool use_pbr;
uniform float pbr_metallic;
uniform float pbr_roughness;

out vec4 out_color;

const float PI = 3.14159265;
// Eye space lights, same directions as the gouraud vertex shader.
const vec3 PBR_LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
const vec3 PBR_LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
// Light intensities include PI so a white diffuse surface gets intensity * NdotL.
const vec3 PBR_LIGHT_TOP_COLOR = vec3(0.45 * PI);
const vec3 PBR_LIGHT_FRONT_COLOR = vec3(0.2 * PI);
// Hemisphere ambient, blended by eye space up axis.
const vec3 PBR_SKY_COLOR = vec3(0.24);
const vec3 PBR_GROUND_COLOR = vec3(0.14);

vec3 srgb_to_linear(vec3 c)
{
    return pow(max(c, ZERO), vec3(2.2));
}

vec3 linear_to_srgb(vec3 c)
{
    return pow(max(c, ZERO), vec3(1.0 / 2.2));
}

vec3 hemisphere_light(vec3 dir)
{
    return mix(PBR_GROUND_COLOR, PBR_SKY_COLOR, dir.y * 0.5 + 0.5);
}

float distribution_ggx(float n_dot_h, float roughness)
{
    float a2 = roughness * roughness * roughness * roughness;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float geometry_schlick_ggx(float n_dot_x, float roughness)
{
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return n_dot_x / (n_dot_x * (1.0 - k) + k);
}

vec3 fresnel_schlick(float cos_theta, vec3 f0)
{
    return f0 + (vec3(1.0) - f0) * pow(1.0 - cos_theta, 5.0);
}

// Analytic approximation of the split-sum environment BRDF (Karis 2014).
vec2 env_brdf_approx(float roughness, float n_dot_v)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

vec3 direct_light(vec3 n, vec3 v, vec3 l, vec3 radiance, vec3 albedo, vec3 f0, float metallic, float roughness)
{
    float n_dot_l = max(dot(n, l), 0.0);
    if (n_dot_l <= 0.0)
        return ZERO;

    vec3 h = normalize(v + l);
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_h = max(dot(n, h), 0.0);
    vec3 f = fresnel_schlick(max(dot(h, v), 0.0), f0);
    float g = geometry_schlick_ggx(n_dot_v, roughness) * geometry_schlick_ggx(n_dot_l, roughness);
    vec3 specular = distribution_ggx(n_dot_h, roughness) * g * f / (4.0 * n_dot_v * n_dot_l + 1e-4);
    vec3 k_d = (vec3(1.0) - f) * (1.0 - metallic);
    return (k_d * albedo / PI + specular) * radiance * n_dot_l;
}

// Khronos PBR Neutral tone mapping, keeps base colors close to their sRGB values.
vec3 pbr_neutral_tone_mapping(vec3 color)
{
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression)
        return color;
    const float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(color, vec3(new_peak), g);
}

vec3 shade_pbr(vec3 base_color)
{
    vec3 albedo = srgb_to_linear(base_color);
    float metallic = clamp(pbr_metallic, 0.0, 1.0);
    float roughness = clamp(pbr_roughness, 0.045, 1.0);

    vec3 n = normalize(eye_normal);
    // Back faces are visible when culling is disabled, light them as front faces.
    if (!gl_FrontFacing)
        n = -n;
    vec3 v = normalize(-eye_position);
    float n_dot_v = max(dot(n, v), 1e-4);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);

    vec3 color = direct_light(n, v, PBR_LIGHT_TOP_DIR, PBR_LIGHT_TOP_COLOR, albedo, f0, metallic, roughness);
    color += direct_light(n, v, PBR_LIGHT_FRONT_DIR, PBR_LIGHT_FRONT_COLOR, albedo, f0, metallic, roughness);

    vec2 env_brdf = env_brdf_approx(roughness, n_dot_v);
    vec3 ambient_specular = hemisphere_light(reflect(-v, n)) * (f0 * env_brdf.x + env_brdf.y);
    vec3 ambient_diffuse = hemisphere_light(n) * albedo * (1.0 - metallic);
    color += ambient_diffuse + ambient_specular;

    return linear_to_srgb(pbr_neutral_tone_mapping(color));
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    if (debug_normal_color) {
        vec3 n = normalize(world_normal);
        if (!gl_FrontFacing)
            n = -n;
        out_color = vec4(n * 0.5 + 0.5, uniform_color.a);
        return;
    }

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

    if (use_pbr) {
        out_color = vec4(shade_pbr(color.rgb), color.a);
        return;
    }

#ifdef ENABLE_ENVIRONMENT_MAP
    if (use_environment_tex)
        out_color = vec4(0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + 0.8 * color.rgb * intensity.x, color.a);
    else
#endif
        out_color = vec4(vec3(intensity.y) + color.rgb * intensity.x, color.a);
}
