#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(binding = 0) uniform SceneUBO
{
	mat4 viewProj;
} ubo;

void main()
{
	gl_Position = ubo.viewProj * vec4(aPos, 1.0);
}
