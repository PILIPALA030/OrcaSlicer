#version 140

uniform vec4 uniform_color;
uniform float emission_factor;

// x = tainted, y = specular;
in vec2 intensity;
#ifdef ENABLE_PCSS
in float pcss_main_diffuse;
in vec3 pcss_world_position;
out vec4 pcss_out_color;
#endif

void main()
{
#ifdef ENABLE_PCSS
    float visibility = pcss_visibility(pcss_world_position);
    float diffuse = intensity.x - pcss_main_diffuse * (1.0 - visibility);
    pcss_out_color = vec4(vec3(intensity.y * visibility) + uniform_color.rgb * (diffuse + emission_factor), uniform_color.a);
#else
    gl_FragColor = vec4(vec3(intensity.y) + uniform_color.rgb * (intensity.x + emission_factor), uniform_color.a);
#endif
}
