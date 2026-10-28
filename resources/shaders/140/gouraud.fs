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
// Image based lighting fixed in the world: eye space directions are turned into environment space.
// Cube map mips are GGX prefiltered (lod = roughness * max lod), SH gives irradiance / pi.
uniform mat3 pbr_view_to_env;
uniform samplerCube pbr_env_map;
uniform bool pbr_env_ready;
uniform float pbr_env_max_lod;
uniform float pbr_sh[9];
// Lighting, tunable from the PBR debug panel.
uniform float pbr_exposure;
uniform float pbr_env_intensity;
uniform float pbr_env_rotation;
uniform float pbr_diffuse;
uniform float pbr_reflection;
uniform float pbr_reflection_saturation;

out vec4 out_color;

vec3 srgb_to_linear(vec3 c)
{
    return pow(max(c, ZERO), vec3(2.2));
}

vec3 linear_to_srgb(vec3 c)
{
    return pow(max(c, ZERO), vec3(1.0 / 2.2));
}

// Turn the environment around its up axis (world +Z, degrees), same convention as the bake script.
vec3 rotate_environment(vec3 dir)
{
    float angle = radians(pbr_env_rotation);
    float c = cos(angle);
    float s = sin(angle);
    return vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
}

// Prefiltered radiance around 'dir', blended from gray to the studio color by the reflection saturation;
// a plain sky / ground gradient if the environment failed to load.
vec3 environment_radiance(vec3 dir, float roughness)
{
    if (!pbr_env_ready)
        return vec3(mix(0.15, 0.45, dir.y * 0.5 + 0.5));
    vec3 radiance = textureLod(pbr_env_map, rotate_environment(dir), roughness * pbr_env_max_lod).rgb;
    float luma = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(luma), radiance, clamp(pbr_reflection_saturation, 0.0, 1.0));
}

// Irradiance / pi from the 9 SH coefficients (basis matches the bake script).
float environment_irradiance(vec3 n)
{
    if (!pbr_env_ready)
        return mix(0.25, 0.6, n.y * 0.5 + 0.5);
    vec3 d = rotate_environment(n);
    return pbr_sh[0] * 0.282095
         + pbr_sh[1] * 0.488603 * d.y + pbr_sh[2] * 0.488603 * d.z + pbr_sh[3] * 0.488603 * d.x
         + pbr_sh[4] * 1.092548 * d.x * d.y + pbr_sh[5] * 1.092548 * d.y * d.z
         + pbr_sh[6] * 0.315392 * (3.0 * d.z * d.z - 1.0)
         + pbr_sh[7] * 1.092548 * d.x * d.z + pbr_sh[8] * 0.546274 * (d.x * d.x - d.y * d.y);
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

// Khronos PBR Neutral highlight shoulder, keeps base colors close to their sRGB values.
vec3 pbr_neutral_tone_mapping(vec3 color)
{
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;
    // Toe offset of the reference curve is skipped: it crushes the dark studio reflections to black.
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

    vec2 env_brdf = env_brdf_approx(roughness, n_dot_v);
    vec3 env_reflect = pbr_view_to_env * reflect(-v, n);
    vec3 env_normal = pbr_view_to_env * n;
    vec3 specular = environment_radiance(env_reflect, roughness) * (f0 * env_brdf.x + env_brdf.y) * pbr_reflection;
    vec3 diffuse = environment_irradiance(env_normal) * albedo * (1.0 - metallic) * pbr_diffuse;
    vec3 color = (diffuse + specular) * pbr_env_intensity;

    return linear_to_srgb(pbr_neutral_tone_mapping(color * pbr_exposure));
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
