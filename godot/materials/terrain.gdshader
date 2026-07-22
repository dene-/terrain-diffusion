shader_type spatial;
render_mode depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

// TerrainWorld3D selects a non-overlapping resident quadtree. LOD is therefore
// geometry-level; no parent/child ring masking or fragment discard is needed.
instance uniform float lod_level = 0.0;
instance uniform vec2 floating_origin_xz = vec2(0.0);
uniform vec2 camera_absolute_xz = vec2(0.0);

// These defaults mirror WorldVisualConfig and are refreshed by TerrainWorld3D.
uniform float albedo_gain : hint_range(0.1, 2.0, 0.01) = 0.56;
uniform float macro_texture_scale_meters : hint_range(100.0, 5000.0, 10.0) = 1200.0;
uniform float detail_fade_start_meters : hint_range(0.0, 10000.0, 10.0) = 1200.0;
uniform float detail_fade_end_meters : hint_range(1000.0, 100000.0, 100.0) = 18000.0;
uniform float atmosphere_distance_density : hint_range(0.0, 0.001, 0.000001) = 0.000085;
uniform float atmosphere_height_falloff : hint_range(0.0, 0.002, 0.000001) = 0.000118;
uniform float atmosphere_base_height_meters = 0.0;
uniform int debug_view : hint_range(0, 12, 1) = 0;
uniform float realtime_shadow_range_meters = 3000.0;
uniform sampler2D detail_texture : filter_linear_mipmap_anisotropic, repeat_enable;

varying vec4 terrain_derived;
varying vec4 terrain_biome;
varying vec3 terrain_world_position;
varying vec3 terrain_world_normal;
varying vec3 terrain_absolute_position;
varying float terrain_surface_distance;
// These fields vary over tens to thousands of metres. Evaluating them once
// per terrain vertex preserves their visible frequency at each mesh LOD while
// avoiding six value-noise evaluations for every covered screen pixel.
varying vec4 terrain_color_noise;

float hash21(vec2 value) {
	value = fract(value * vec2(0.1031, 0.1030));
	value += dot(value, value.yx + 33.33);
	return fract((value.x + value.y) * value.x);
}

float value_noise(vec2 position) {
	vec2 cell = floor(position);
	vec2 blend = fract(position);
	blend = blend * blend * (3.0 - 2.0 * blend);
	float a = hash21(cell);
	float b = hash21(cell + vec2(1.0, 0.0));
	float c = hash21(cell + vec2(0.0, 1.0));
	float d = hash21(cell + vec2(1.0, 1.0));
	return mix(mix(a, b, blend.x), mix(c, d, blend.x), blend.y);
}

float macro_noise(vec2 position) {
	return value_noise(position) * 0.68
		+ value_noise(position * 2.07 + vec2(19.1, 7.7)) * 0.32;
}

float triplanar_noise(vec3 position, vec3 world_normal) {
	vec3 weights = pow(abs(world_normal), vec3(4.0));
	weights /= max(weights.x + weights.y + weights.z, 0.0001);
	return value_noise(position.yz) * weights.x
		+ value_noise(position.xz) * weights.y
		+ value_noise(position.xy) * weights.z;
}

float triplanar_detail_texture(vec3 position, vec3 world_normal) {
	vec3 weights = pow(abs(world_normal), vec3(4.0));
	weights /= max(weights.x + weights.y + weights.z, 0.0001);
	return texture(detail_texture, position.yz).r * weights.x
		+ texture(detail_texture, position.xz).r * weights.y
		+ texture(detail_texture, position.xy).r * weights.z;
}

vec3 lod_debug_color(float level) {
	if (level < 0.5) return vec3(0.12, 0.78, 0.30);
	if (level < 1.5) return vec3(0.85, 0.76, 0.16);
	if (level < 2.5) return vec3(0.94, 0.43, 0.12);
	if (level < 3.5) return vec3(0.86, 0.16, 0.22);
	if (level < 4.5) return vec3(0.72, 0.19, 0.72);
	if (level < 5.5) return vec3(0.24, 0.34, 0.92);
	if (level < 6.5) return vec3(0.10, 0.72, 0.88);
	if (level < 7.5) return vec3(0.20, 0.84, 0.74);
	if (level < 8.5) return vec3(0.58, 0.72, 0.94);
	if (level < 9.5) return vec3(0.82, 0.54, 0.94);
	return vec3(0.92, 0.92, 0.92);
}

void vertex() {
	terrain_derived = CUSTOM0;
	terrain_biome = CUSTOM1;
	vec3 flat_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	terrain_absolute_position = flat_world_position
		+ vec3(floating_origin_xz.x, 0.0, floating_origin_xz.y);
	float macro_scale = max(macro_texture_scale_meters, 1.0);
	terrain_color_noise = vec4(
		macro_noise(terrain_absolute_position.xz / (macro_scale * 4.2)),
		macro_noise(terrain_absolute_position.xz / macro_scale),
		value_noise(terrain_absolute_position.xz / 34.0),
		value_noise(terrain_absolute_position.xz / 92.0));
	vec2 surface_offset = terrain_absolute_position.xz - camera_absolute_xz;
	terrain_surface_distance = length(surface_offset);
	terrain_world_normal = normalize(mat3(MODEL_MATRIX) * NORMAL);
	NORMAL = terrain_world_normal;
	terrain_world_position = flat_world_position;
}

void fragment() {
	float surface_distance = terrain_surface_distance;
	float slope = terrain_derived.r;
	float curvature = terrain_derived.g;
	float wetness = terrain_derived.b;
	float macro_variation = terrain_derived.a;
	float forest = terrain_biome.r;
	float rock = terrain_biome.g;
	float scree = terrain_biome.b;
	float snow = terrain_biome.a;
	float concavity = smoothstep(0.50, 0.82, curvature);

	// Debug views bypass the production material so inspecting a streamed
	// mask does not execute the complete shading path.
	if (debug_view > 0) {
		vec3 debug_color = vec3(0.0);
		if (debug_view == 1) {
			float elevation = clamp((terrain_absolute_position.y + 250.0) / 5000.0, 0.0, 1.0);
			debug_color = vec3(elevation);
		} else if (debug_view == 2) {
			debug_color = terrain_world_normal * 0.5 + 0.5;
		} else if (debug_view == 3) {
			debug_color = vec3(slope);
		} else if (debug_view == 4) {
			debug_color = curvature < 0.5
				? mix(vec3(0.08, 0.22, 0.82), vec3(0.5), curvature * 2.0)
				: mix(vec3(0.5), vec3(0.92, 0.33, 0.08), (curvature - 0.5) * 2.0);
		} else if (debug_view == 5) {
			debug_color = vec3(0.05, 0.18, 0.30) + wetness * vec3(0.05, 0.62, 0.70);
		} else if (debug_view == 6) {
			debug_color = vec3(0.02, forest, 0.04);
		} else if (debug_view == 7) {
			debug_color = vec3(rock);
		} else if (debug_view == 8) {
			debug_color = scree * vec3(0.95, 0.54, 0.12);
		} else if (debug_view == 9) {
			debug_color = snow * vec3(0.72, 0.88, 1.0);
		} else if (debug_view == 10) {
			debug_color = lod_debug_color(lod_level);
		} else if (debug_view == 11) {
			float altitude_density = exp(
				-max(terrain_absolute_position.y - atmosphere_base_height_meters, 0.0)
				* atmosphere_height_falloff);
			float fog_amount = 1.0 - exp(
				-surface_distance * atmosphere_distance_density * altitude_density);
			debug_color = vec3(fog_amount);
		} else if (debug_view == 12) {
			// Camera-stable visualization of the single orthogonal shadow map.
			// Green is fully covered by realtime shadows, amber is the fade
			// interval, and blue uses terrain-form/macro lighting only.
			float safe_shadow_range = max(realtime_shadow_range_meters, 1.0);
			float fade = smoothstep(
				safe_shadow_range * 0.84,
				safe_shadow_range,
				surface_distance);
			vec3 covered = vec3(0.10, 0.78, 0.28);
			vec3 transition = vec3(0.96, 0.64, 0.10);
			vec3 macro_only = vec3(0.12, 0.30, 0.88);
			debug_color = fade < 0.5
				? mix(covered, transition, fade * 2.0)
				: mix(transition, macro_only, (fade - 0.5) * 2.0);
		}
		ALBEDO = debug_color * 0.44;
		EMISSION = debug_color * 0.56;
		ROUGHNESS = 1.0;
		SPECULAR = 0.0;
		AO = 1.0;
	} else {

	float detail_visibility = 1.0 - smoothstep(
		detail_fade_start_meters,
		max(detail_fade_end_meters, detail_fade_start_meters + 1.0),
		surface_distance);
	float continental = terrain_color_noise.x;
	float macro_value = terrain_color_noise.y;
	// The 34/92 m procedural fields are only Nyquist-safe on L0. Sampling
	// them per vertex on 15-480 m rings produced contour-like stripes that
	// looked like bad geometry. Coarser LODs retain only macro material fields.
	float fine_vertex_visibility = 1.0 - smoothstep(0.0, 1.0, lod_level);
	float local_value = mix(macro_value, terrain_color_noise.z, fine_vertex_visibility);
	// Compressed mipmapped texture samples restore fragment-scale material
	// breakup near the viewer without the former procedural hash cost. The
	// branch is spatially coherent and disappears before becoming subpixel.
	float texture_detail_visibility = 1.0 - smoothstep(120.0, 320.0, surface_distance);
	float surface_texture_detail = 0.5;
	if (texture_detail_visibility > 0.0) {
		surface_texture_detail = triplanar_detail_texture(
			terrain_absolute_position / 128.0, terrain_world_normal);
	}
	float terrain_detail = mix(macro_value, surface_texture_detail, texture_detail_visibility);
	float material_breakup = mix(
		macro_value,
		mix(terrain_detail, surface_texture_detail, texture_detail_visibility),
		detail_visibility);
	float canopy_macro = mix(macro_value, terrain_color_noise.w, fine_vertex_visibility);

	vec3 dry_grass = vec3(0.255, 0.285, 0.105);
	vec3 moist_grass = vec3(0.085, 0.215, 0.060);
	vec3 climate_grass = mix(dry_grass, moist_grass, wetness);
	vec3 grass_color = mix(COLOR.rgb, climate_grass, 0.72);
	float broad_variation = macro_variation * 0.46 + macro_value * 0.34 + continental * 0.20;
	grass_color *= mix(0.82, 1.16, broad_variation);
	grass_color *= mix(0.84, 1.17, mix(macro_value, local_value, detail_visibility));
	grass_color *= mix(0.90, 1.11, mix(0.5, terrain_detail, detail_visibility));
	float dry_meadow = smoothstep(0.58, 0.82, macro_value)
		* (1.0 - wetness) * (1.0 - rock) * (1.0 - snow);
	grass_color = mix(grass_color, vec3(0.30, 0.275, 0.105), dry_meadow * 0.38);

	vec3 wet_soil_color = mix(vec3(0.16, 0.125, 0.075), vec3(0.105, 0.115, 0.075), wetness);
	float wet_soil = wetness * concavity * (1.0 - rock) * (1.0 - snow) * 0.92;

	vec3 forest_floor = mix(vec3(0.075, 0.135, 0.055), vec3(0.115, 0.205, 0.075), canopy_macro);
	forest_floor *= mix(0.91, 1.06, continental);
	float forest_ground = forest * (1.0 - snow) * (1.0 - rock * 0.82);

	vec3 rock_dark = vec3(0.205, 0.215, 0.215);
	vec3 rock_light = vec3(0.39, 0.38, 0.355);
	vec3 rock_color = mix(rock_dark, rock_light, material_breakup * 0.72 + macro_value * 0.28);
	rock_color *= mix(0.88, 1.06, terrain_detail * detail_visibility);

	vec3 scree_dark = vec3(0.255, 0.235, 0.195);
	vec3 scree_light = vec3(0.46, 0.42, 0.34);
	vec3 scree_color = mix(scree_dark, scree_light, material_breakup * 0.76 + macro_value * 0.24);
	scree_color *= mix(0.92, 1.07, local_value * detail_visibility);

	vec3 snow_color = mix(vec3(0.72, 0.77, 0.80), vec3(0.94, 0.965, 0.97), macro_value);
	snow_color *= mix(0.96, 1.035, terrain_detail * detail_visibility);

	vec3 ground = mix(grass_color, wet_soil_color, wet_soil);
	ground = mix(ground, forest_floor, clamp(forest_ground * 0.76, 0.0, 1.0));
	ground = mix(ground, scree_color, scree * (1.0 - rock) * (1.0 - snow));
	ground = mix(ground, rock_color, rock * (1.0 - snow));
	ground = mix(ground, snow_color, snow);

	// Nearby exposed ground needs readable physical scale even where grass
	// geometry is sparse. Two global-coordinate frequencies create seamless
	// soil/vegetation grain, then disappear well before becoming subpixel noise.
	float micro_visibility = 1.0 - smoothstep(100.0, 380.0, surface_distance);
	float coarse_grain = mix(
		0.5, mix(terrain_detail, surface_texture_detail, texture_detail_visibility), micro_visibility);
	float fine_grain = mix(
		0.5, mix(local_value, surface_texture_detail, texture_detail_visibility * 0.72), micro_visibility);
	float ground_grain = coarse_grain * 0.72 + fine_grain * 0.28;
	float grain_strength = micro_visibility
		* (1.0 - snow * 0.82)
		* mix(1.0, 0.62, clamp(rock + scree * 0.55, 0.0, 1.0));
	ground *= mix(1.0, mix(0.90, 1.08, ground_grain), grain_strength);
	vec3 exposed_soil = mix(vec3(0.095, 0.082, 0.052), vec3(0.13, 0.115, 0.072), wetness);
	float soil_fleck = smoothstep(0.74, 0.91, coarse_grain * 0.66 + fine_grain * 0.34)
		* grain_strength * (1.0 - rock) * (1.0 - snow) * 0.10;
	ground = mix(ground, exposed_soil, soil_fleck);

	// Concavity and exposed ridges retain broad terrain form outside the shadow range.
	float macro_occlusion = clamp(1.0 - concavity * 0.27 - forest * 0.055 + (0.5 - curvature) * 0.08, 0.64, 1.0);
	AO = macro_occlusion;
	AO_LIGHT_AFFECT = 0.24;
	// The project atmosphere provides a physically bright sky/sun combination. Keep the
	// authored biome ratios while reserving diffuse headroom for that lighting
	// instead of compensating by reducing global exposure.
	ALBEDO = ground * albedo_gain;
	// A small diffuse ground-bounce floor preserves material identity beneath
	// dense alpha-tested canopies without weakening the sun's direct shadows.
	EMISSION = ground * albedo_gain * 0.018;
	ROUGHNESS = clamp(
		clamp(
			0.91 - rock * 0.17 - wetness * 0.15 - snow * 0.08 + scree * 0.04,
			0.50,
			0.97) * mix(1.0, mix(0.94, 1.04, ground_grain), micro_visibility),
		0.0,
		1.0);
	SPECULAR = mix(0.18, 0.30, wetness * (1.0 - snow));

	}
}
