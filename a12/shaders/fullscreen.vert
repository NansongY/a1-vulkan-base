#version 450

// Task 1.7: fullscreen triangle for the post-processing pass. The three
// vertices are generated from gl_VertexIndex, so no vertex buffer is needed.

layout(location = 0) out vec2 vUV;

void main()
{
	const vec2 positions[3] = vec2[3](
		vec2( -1.0, -1.0 ),
		vec2(  3.0, -1.0 ),
		vec2( -1.0,  3.0 )
	);

	const vec2 pos = positions[gl_VertexIndex];
	vUV = pos * 0.5 + 0.5; // NDC [-1,1] -> UV [0,1]
	gl_Position = vec4( pos, 0.0, 1.0 );
}
