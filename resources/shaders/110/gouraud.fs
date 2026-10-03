#version 110

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

varying vec3 clipping_planes_dots;
varying float color_clip_plane_dot;

// x = diffuse, y = specular;
varying vec2 intensity;

varying vec4 world_pos;
varying float world_normal_z;
varying vec3 eye_normal;
varying vec4 shadow_position;
varying float top_diffuse;

vec2 shadow_poisson_offset(int index)
{
    if (index == 0) return vec2(-0.94201624, -0.39906216);
    if (index == 1) return vec2(0.94558609, -0.76890725);
    if (index == 2) return vec2(-0.09418410, -0.92938870);
    if (index == 3) return vec2(0.34495938, 0.29387760);
    if (index == 4) return vec2(-0.91588581, 0.45771432);
    if (index == 5) return vec2(-0.81544232, -0.87912464);
    if (index == 6) return vec2(-0.38277543, 0.27676845);
    return vec2(0.97484398, 0.75648379);
}

float pcss_shadow_factor()
{
    if (shadow_position.w <= 0.0)
        return 1.0;
    vec3 projected = shadow_position.xyz / shadow_position.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;

    float receiverDepth = projected.z * 0.5 + 0.5;
    float searchRadius = shadow_light_size * (1.0 + receiverDepth * 2.0) * 32.0;
    float blockerDepth = 0.0;
    float blockerCount = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 sampleUv = uv + shadow_poisson_offset(i) * shadow_map_texel_size * searchRadius;
        float sampleDepth = texture2D(shadow_map, sampleUv).r;
        if (sampleDepth + shadow_bias < receiverDepth) {
            blockerDepth += sampleDepth;
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5)
        return 1.0;

    blockerDepth /= blockerCount;
    float penumbra = (receiverDepth - blockerDepth) / max(blockerDepth, 0.05);
    float filterRadius = clamp(penumbra * shadow_light_size * 32.0, 1.0, 6.0);
    float lit = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 sampleUv = uv + shadow_poisson_offset(i) * shadow_map_texel_size * filterRadius;
        lit += texture2D(shadow_map, sampleUv).r + shadow_bias >= receiverDepth ? 1.0 : 0.0;
    }
    return lit / 4.0;
}

void main()
{
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

    float shadowFactor = (shadow_enabled && top_diffuse > 0.0) ? pcss_shadow_factor() : 1.0;
    float lighting = intensity.x - top_diffuse * (1.0 - shadowFactor);

#ifdef ENABLE_ENVIRONMENT_MAP
    if (use_environment_tex)
        gl_FragColor = vec4(0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz +
                            0.8 * color.rgb * lighting, color.a);
    else
#endif
        gl_FragColor = vec4(vec3(intensity.y) + color.rgb * lighting, color.a);
}
