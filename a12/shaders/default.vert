#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(set = 0, binding = 0) uniform SceneUBO
{
	mat4 viewProj;
} ubo;

layout(location = 0) out vec2 vUV;

void main()
{
	vUV = aUV;
	gl_Position = ubo.viewProj * vec4(aPos, 1.0);
}
