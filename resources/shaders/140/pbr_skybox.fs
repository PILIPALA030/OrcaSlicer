#version 140

const float PI = 3.14159265;

// Equirect color panorama (sRGB, already exposed), top row first.
uniform sampler2D skybox_tex;
// tan(vertical fov / 2) and width / height of the viewport.
uniform float view_tan;
uniform float aspect;
// Eye space to environment space rotation, the environment is fixed in the world.
uniform mat3 view_to_env;
// Same rotation as the PBR environment lighting (degrees), so reflections match the background.
uniform float env_rotation;
uniform float brightness;
uniform float saturation;
// Mip level used to blur the background.
uniform float blur;

in vec2 ndc;

out vec4 out_color;

void main()
{
    // Eye space view direction turned into the world fixed environment, like the PBR lighting.
    vec3 dir = view_to_env * normalize(vec3(ndc.x * view_tan * aspect, ndc.y * view_tan, -1.0));
    float angle = radians(env_rotation);
    float c = cos(angle);
    float s = sin(angle);
    dir = vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);

    vec2 uv = vec2(atan(dir.x, dir.z) / (2.0 * PI) + 0.5, 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / PI);
    vec3 color = textureLod(skybox_tex, uv, blur).rgb;
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    out_color = vec4(mix(vec3(luma), color, saturation) * brightness, 1.0);
}
