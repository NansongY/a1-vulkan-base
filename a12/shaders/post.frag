#version 450

// Task 1.7: post-processing. Samples the intermediate scene-color texture and
// applies a 5x3 mosaic / pixelation effect when the push constant is non-zero.

layout(location = 0) in vec2 vUV;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;

layout(push_constant) uniform PC
{
	uint mosaicOn;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
	vec2 uv = vUV;

	if( 0u != pc.mosaicOn )
	{
		// Snap to the top-left pixel of each 5x3 block so the whole block
		// samples the same texel.
		vec2 block = floor( gl_FragCoord.xy / vec2(5.0, 3.0) );
		vec2 texSize = vec2( textureSize( sceneColor, 0 ) );
		uv = (block * vec2(5.0, 3.0) + vec2(0.5)) / texSize;
	}

	outColor = vec4( texture( sceneColor, uv ).rgb, 1.0 );
}
