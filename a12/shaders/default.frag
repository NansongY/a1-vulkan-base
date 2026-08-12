#version 450

layout(location = 0) in vec2 vUV;

layout(set = 1, binding = 0) uniform sampler2D baseColor;

layout(location = 0) out vec4 outColor;

void main()
{
	outColor = vec4(texture(baseColor, vUV).rgb, 1.0);
}
