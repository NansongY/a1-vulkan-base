#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(set = 0, binding = 0) uniform SceneUBO
{
	mat4 viewProj;
	vec4 params; // x = near, y = far, z = renderMode (unused here)
} ubo;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vWorldPos; // fragment position in world space
layout(location = 2) out vec3 vNormal;   // vertex normal in world space

void main()
{
	vUV = aUV;
	vWorldPos = aPos;  // the model is defined directly in world space
	vNormal = aNormal;
	gl_Position = ubo.viewProj * vec4(aPos, 1.0);
}
