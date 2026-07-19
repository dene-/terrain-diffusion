shader_type spatial;
render_mode depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

instance uniform float lod_inner_radius = 0.0;
instance uniform float lod_outer_radius = 1000000.0;
instance uniform vec2 floating_origin_xz = vec2(0.0);

varying vec4 terrain_derived;
varying vec4 terrain_biome;
varying vec3 terrain_world_position;
varying vec2 terrain_absolute_xz;

float hash21(vec2 value) {
	value = fract(value * vec2(0.1031, 0.1030));
	value += dot(value, value.yx + 33.33);
	return fract((value.x + value.y) * value.x);
}

void vertex() {
	terrain_derived = CUSTOM0;
	terrain_biome = CUSTOM1;
	terrain_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	terrain_absolute_xz = terrain_world_position.xz + floating_origin_xz;
}

void fragment() {
	float surface_distance = distance(terrain_world_position.xz, CAMERA_POSITION_WORLD.xz);
	float outer_transition = max(32.0, min(lod_outer_radius * 0.08, 9000.0));
	float inner_transition = max(24.0, min(lod_inner_radius * 0.16, 1800.0));
	float inner_coverage = lod_inner_radius <= 0.5
		? 1.0
		: smoothstep(
			lod_inner_radius - inner_transition,
			lod_inner_radius + inner_transition,
			surface_distance);
	float outer_coverage = 1.0 - smoothstep(
		lod_outer_radius - outer_transition,
		lod_outer_radius,
		surface_distance);
	float ring_coverage = inner_coverage * outer_coverage;
	float coverage_threshold = 0.001 + hash21(floor(FRAGCOORD.xy * 0.5)) * 0.998;
	if (ring_coverage < coverage_threshold) discard;

	float slope = terrain_derived.r;
	float wetness = terrain_derived.b;
	float macro_variation = terrain_derived.a;
	float forest = terrain_biome.r;
	float rock = terrain_biome.g;
	float scree = terrain_biome.b;
	float snow = terrain_biome.a;

	vec3 ground = COLOR.rgb;
	vec3 forest_floor = vec3(0.105, 0.185, 0.080);
	vec3 rock_color = vec3(0.30, 0.31, 0.32);
	vec3 scree_color = vec3(0.38, 0.36, 0.31);
	vec3 snow_color = vec3(0.86, 0.89, 0.91);

	float macro_noise = hash21(floor(terrain_absolute_xz / 180.0));
	ground *= mix(0.91, 1.08, macro_variation * 0.65 + macro_noise * 0.35);
	ground = mix(ground, forest_floor, forest * 0.34);
	ground = mix(ground, scree_color, scree * (1.0 - rock));
	ground = mix(ground, rock_color, rock);
	ground = mix(ground, snow_color, snow);

	ALBEDO = ground;
	ROUGHNESS = clamp(0.90 - rock * 0.18 - wetness * 0.13 + slope * 0.04, 0.55, 0.96);
	SPECULAR = 0.18;
}
