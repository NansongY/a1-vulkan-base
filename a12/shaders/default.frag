#version 450

// Task 1.5: physically-inspired PBR shading, computed per fragment in world
// space. Model from the assignment:
//   Lo = Lambient + fr * clight * (n.l)+
//   fr = diffuse + D*F*G / (4 (n.v)+ (n.l)+)
// with a Lambertian diffuse, the Beckmann NDF, the Cook-Torrance masking term
// and the Schlick Fresnel approximation. Beckmann alpha = roughness^2.

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) in vec3 vNormal;

layout(set = 0, binding = 0) uniform SceneUBO
{
	mat4 viewProj;
	vec4 params; // x = near, y = far, z = renderMode
	vec4 cameraPos;
	vec4 lightPos;
	vec4 lightColor;
	vec4 ambientColor;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D baseColor;
layout(set = 1, binding = 1) uniform sampler2D roughnessTex;
layout(set = 1, binding = 2) uniform sampler2D metalnessTex;

layout(location = 0) out vec4 outColor;

void main()
{
	// Material parameters (linear space: base color is sRGB and gets decoded
	// by the hardware when sampled; roughness/metalness are plain unorm).
	const vec3 cmat = texture( baseColor, vUV ).rgb;
	const float roughness = texture( roughnessTex, vUV ).r;
	const float metalness = texture( metalnessTex, vUV ).r;

	// Beckmann roughness alpha = texture roughness squared.
	const float alpha = roughness * roughness;
	const float a2 = alpha * alpha;

	// Shading vectors in world space (all normalized).
	const vec3 n = normalize( vNormal );
	const vec3 l = normalize( ubo.lightPos.xyz - vWorldPos );
	const vec3 v = normalize( ubo.cameraPos.xyz - vWorldPos );
	const vec3 h = normalize( l + v );

	const float ndl = max( dot( n, l ), 0.0 );
	const float ndv = max( dot( n, v ), 0.0 );
	const float ndh = max( dot( n, h ), 0.0 );
	const float vdh = max( dot( v, h ), 1e-4 ); // guard divide-by-zero

	// F0: dielectrics reflect ~4% of light; metals are tinted by base color.
	const vec3 F0 = (1.0 - metalness) * vec3(0.04) + metalness * cmat;

	// Schlick Fresnel approximation.
	const vec3 F = F0 + (1.0 - F0) * pow( max( 1.0 - vdh, 0.0 ), 5.0 );

	// Lambertian diffuse: only the light not reflected specularly (1-F), and
	// none for metals (1-M).
	const vec3 diffuse = (cmat / 3.14159265359) * (vec3(1.0) - F) * (1.0 - metalness);

	// Beckmann normal distribution function.
	const float ndh2 = ndh * ndh;
	const float D = exp( (ndh2 - 1.0) / max( a2 * ndh2, 1e-6 ) )
	              / ( 3.14159265359 * max( a2 * ndh2 * ndh2, 1e-6 ) );

	// Cook-Torrance geometric masking term.
	const float G = min( 1.0, min( (2.0 * ndh * ndv) / vdh, (2.0 * ndh * ndl) / vdh ) );

	// Microfacet specular BRDF (epsilon guards the denominator against 0).
	const vec3 specular = (D * F * G) / max( 4.0 * ndv * ndl, 1e-4 );

	const vec3 fr = diffuse + specular;

	// Constant ambient + one direct point light (no distance falloff).
	const vec3 ambient = ubo.ambientColor.rgb * cmat;
	const vec3 Lo = ambient + fr * ubo.lightColor.rgb * ndl;

	outColor = vec4( Lo, 1.0 );
}
