shader_type spatial;
render_mode depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

varying vec4 terrain_derived;
varying vec4 terrain_biome;
varying vec3 terrain_world_position;

float hash21(vec2 value) {
	value = fract(value * vec2(0.1031, 0.1030));
	value += dot(value, value.yx + 33.33);
	return fract((value.x + value.y) * value.x);
}

void vertex() {
	terrain_derived = CUSTOM0;
	terrain_biome = CUSTOM1;
	terrain_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
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

	float macro_noise = hash21(floor(terrain_world_position.xz / 180.0));
	ground *= mix(0.91, 1.08, macro_variation * 0.65 + macro_noise * 0.35);
	ground = mix(ground, forest_floor, forest * 0.34);
	ground = mix(ground, scree_color, scree * (1.0 - rock));
	ground = mix(ground, rock_color, rock);
	ground = mix(ground, snow_color, snow);

	ALBEDO = ground;
	ROUGHNESS = clamp(0.90 - rock * 0.18 - wetness * 0.13 + slope * 0.04, 0.55, 0.96);
	SPECULAR = 0.18;
}
