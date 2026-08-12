#version 450

// Task 1.4 debug visualization fragment shader.
// Renders one of three visualizations based on ubo.params.z (renderMode):
//   2 = texture mip level usage
//   3 = linearized fragment depth
//   4 = partial derivatives of per-fragment depth

layout(location = 0) in vec2 vUV;

layout(set = 0, binding = 0) uniform SceneUBO
{
	mat4 viewProj;
	vec4 params; // x = near, y = far, z = renderMode
} ubo;

layout(set = 1, binding = 0) uniform sampler2D baseColor;

layout(location = 0) out vec4 outColor;

void main()
{
	const int mode = int( ubo.params.z + 0.5 );

	if( 2 == mode )
	{
		// Mip level visualization: show which mip level would be sampled.
		const float mip = textureQueryLod( baseColor, vUV ).x;
		const float maxMip = max( 1.0, textureQueryLevels( baseColor ) );
		const float t = clamp( mip / maxMip, 0.0, 1.0 );
		outColor = vec4( t, 1.0 - t, 0.0, 1.0 );
	}
	else if( 3 == mode )
	{
		// Linearized fragment depth (grayscale; brighter = farther).
		const float ndcZ = gl_FragCoord.z * 2.0 - 1.0;
		const float near = ubo.params.x;
		const float far  = ubo.params.y;
		const float lin = (2.0 * near * far) / (far + near - ndcZ * (far - near));
		const float d = clamp( lin / far, 0.0, 1.0 );
		outColor = vec4( d, d, d, 1.0 );
	}
	else
	{
		// Partial derivatives of per-fragment depth, scaled to be visible.
		const float dzx = abs( dFdx( gl_FragCoord.z ) );
		const float dzy = abs( dFdy( gl_FragCoord.z ) );
		const float scale = 100.0;
		outColor = vec4( clamp( dzx * scale, 0.0, 1.0 ), clamp( dzy * scale, 0.0, 1.0 ), 0.0, 1.0 );
	}
}
