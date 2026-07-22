shader_type spatial;
render_mode depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

// Camera state is global so every streamed tile can keep its own height and
// classification textures without duplicating per-frame material updates.
global uniform vec2 terrain_camera_absolute_xz;

uniform sampler2D heightmap : filter_linear, repeat_disable;
uniform sampler2D terrain_fields : filter_linear, repeat_disable;
uniform sampler2D biome_fields : filter_linear, repeat_disable;
uniform vec2 height_uv_offset;
uniform vec2 height_uv_scale;
uniform float grid_segments = 32.0;
uniform float morph_grid_ratio = 2.0;
// North, south, west, east ratios between this grid spacing and the actual
// selected neighbor. Values above one stitch this fine edge to that coarser
// grid; interiors retain their full source detail.
uniform vec4 edge_grid_ratios = vec4(1.0);
uniform float tile_world_size = 480.0;
uniform vec2 tile_absolute_origin;
uniform float morph_start_meters = 0.0;
uniform float morph_end_meters = 1.0;
uniform float albedo_gain = 0.56;
uniform int debug_view = 0;
instance uniform float lod_level = 0.0;

varying vec3 terrain_absolute_position;
varying vec3 terrain_world_normal;
varying vec4 terrain_derived;
varying vec4 terrain_biome;
varying float terrain_surface_distance;

vec2 height_uv(vec2 grid_uv) {
	return height_uv_offset + grid_uv * height_uv_scale;
}

float terrain_height(vec2 grid_uv) {
	return textureLod(heightmap, height_uv(clamp(grid_uv, vec2(0.0), vec2(1.0))), 0.0).r;
}

float terrain_height_with_border(vec2 grid_uv) {
	// Streamed height textures retain three real source samples around every
	// visible edge. Normal derivatives must use them; clamping to the visible
	// tile made each side flatten independently and drew dark seam lines even
	// though the shared edge positions were watertight.
	return textureLod(heightmap, height_uv(grid_uv), 0.0).r;
}

float coarser_height(vec2 grid_uv) {
	float ratio = max(morph_grid_ratio, 2.0);
	vec2 coarse_grid = grid_uv * grid_segments / ratio;
	vec2 base = floor(coarse_grid);
	vec2 blend = fract(coarse_grid);
	vec2 inverse_segments = vec2(1.0 / grid_segments);
	vec2 uv00 = base * ratio * inverse_segments;
	vec2 uv10 = uv00 + vec2(ratio * inverse_segments.x, 0.0);
	vec2 uv01 = uv00 + vec2(0.0, ratio * inverse_segments.y);
	vec2 uv11 = uv00 + ratio * inverse_segments;
	return mix(
		mix(terrain_height(uv00), terrain_height(uv10), blend.x),
		mix(terrain_height(uv01), terrain_height(uv11), blend.x),
		blend.y);
}

float stitched_edge_height(vec2 grid_uv, float ratio, bool along_x) {
	float safe_ratio = max(ratio, 1.0);
	float coordinate = (along_x ? grid_uv.x : grid_uv.y)
		* grid_segments / safe_ratio;
	float base = floor(coordinate);
	float blend = fract(coordinate);
	float first = base * safe_ratio / grid_segments;
	float second = (base + 1.0) * safe_ratio / grid_segments;
	vec2 uv_first = along_x
		? vec2(first, grid_uv.y) : vec2(grid_uv.x, first);
	vec2 uv_second = along_x
		? vec2(second, grid_uv.y) : vec2(grid_uv.x, second);
	return mix(terrain_height(uv_first), terrain_height(uv_second), blend);
}

void vertex() {
	vec2 absolute_xz = tile_absolute_origin + UV * tile_world_size;
	terrain_surface_distance = distance(absolute_xz, terrain_camera_absolute_xz);
	float morph = smoothstep(
		morph_start_meters,
		max(morph_end_meters, morph_start_meters + 1.0),
		terrain_surface_distance);
	float height = mix(terrain_height(UV), coarser_height(UV), morph);
	// Readiness-aware quadtree selection can place a fine/coarse boundary
	// before the radial morph reaches one. Stitch the real selected boundary,
	// not the theoretical ring, so T-junction vertices lie on the exact coarse
	// edge segment. Skirt marker vertices follow the same stitched top edge.
	if (UV.y < 0.0001 && edge_grid_ratios.x > 1.001) {
		height = stitched_edge_height(UV, edge_grid_ratios.x, true);
	} else if (UV.y < 0.0001 && edge_grid_ratios.x < 0.999) {
		height = terrain_height(UV);
	}
	if (UV.y > 0.9999 && edge_grid_ratios.y > 1.001) {
		height = stitched_edge_height(UV, edge_grid_ratios.y, true);
	} else if (UV.y > 0.9999 && edge_grid_ratios.y < 0.999) {
		height = terrain_height(UV);
	}
	if (UV.x < 0.0001 && edge_grid_ratios.z > 1.001) {
		height = stitched_edge_height(UV, edge_grid_ratios.z, false);
	} else if (UV.x < 0.0001 && edge_grid_ratios.z < 0.999) {
		height = terrain_height(UV);
	}
	if (UV.x > 0.9999 && edge_grid_ratios.w > 1.001) {
		height = stitched_edge_height(UV, edge_grid_ratios.w, false);
	} else if (UV.x > 0.9999 && edge_grid_ratios.w < 0.999) {
		height = terrain_height(UV);
	}

	float sample_spacing = tile_world_size / grid_segments;
	// The API's authoritative elevation samples are 30 m apart. Differencing
	// interpolated 7.5 m vertices exposes integer source quantization as bands;
	// measure the gradient across at least one native sample instead.
	float normal_radius = min(3.0, max(1.0, ceil(30.0 / sample_spacing)));
	vec2 normal_step = vec2(normal_radius / grid_segments);
	float left = terrain_height_with_border(UV - vec2(normal_step.x, 0.0));
	float right = terrain_height_with_border(UV + vec2(normal_step.x, 0.0));
	float down = terrain_height_with_border(UV - vec2(0.0, normal_step.y));
	float up = terrain_height_with_border(UV + vec2(0.0, normal_step.y));
	vec3 flat_world_normal = normalize(vec3(
		left - right,
		sample_spacing * normal_radius * 2.0,
		down - up));

	VERTEX = vec3(UV.x * tile_world_size, height, UV.y * tile_world_size);
	terrain_world_normal = flat_world_normal;
	NORMAL = terrain_world_normal;
	terrain_absolute_position = vec3(absolute_xz.x, height, absolute_xz.y);
	terrain_derived = textureLod(terrain_fields, UV, 0.0);
	terrain_biome = textureLod(biome_fields, UV, 0.0);
}

vec3 lod_color(float level) {
	if (level < 0.5) return vec3(0.12, 0.78, 0.30);
	if (level < 1.5) return vec3(0.86, 0.74, 0.12);
	if (level < 2.5) return vec3(0.94, 0.40, 0.10);
	if (level < 3.5) return vec3(0.82, 0.14, 0.22);
	if (level < 4.5) return vec3(0.66, 0.18, 0.76);
	return vec3(0.16, 0.42, 0.92);
}

void fragment() {
	if (debug_view == 10) {
		ALBEDO = lod_color(lod_level) * 0.45;
		EMISSION = lod_color(lod_level) * 0.30;
		ROUGHNESS = 1.0;
	} else if (debug_view == 2) {
		ALBEDO = terrain_world_normal * 0.5 + 0.5;
		EMISSION = ALBEDO * 0.25;
		ROUGHNESS = 1.0;
	} else {

	float slope = terrain_derived.r;
	float curvature = terrain_derived.g;
	float wetness = terrain_derived.b;
	float variation = terrain_derived.a;
	float forest = terrain_biome.r;
	float rock = terrain_biome.g;
	float scree = terrain_biome.b;
	float snow = terrain_biome.a;

	vec3 dry_grass = vec3(0.31, 0.34, 0.14);
	vec3 wet_grass = vec3(0.10, 0.24, 0.065);
	vec3 grass = mix(dry_grass, wet_grass, wetness);
	grass *= mix(0.82, 1.16, variation);
	vec3 soil = mix(vec3(0.16, 0.12, 0.07), vec3(0.095, 0.11, 0.065), wetness);
	vec3 stone = mix(vec3(0.20, 0.21, 0.22), vec3(0.43, 0.42, 0.39), variation);
	vec3 scree_color = mix(vec3(0.25, 0.23, 0.19), vec3(0.47, 0.43, 0.35), variation);
	vec3 snow_color = mix(vec3(0.76, 0.81, 0.84), vec3(0.96, 0.98, 0.99), variation);

	float concavity = smoothstep(0.54, 0.78, curvature);
	vec3 ground = mix(grass, soil, wetness * concavity * 0.62);
	ground = mix(ground, ground * vec3(0.58, 0.78, 0.54), forest * 0.28);
	ground = mix(ground, scree_color, scree * (1.0 - rock) * (1.0 - snow));
	ground = mix(ground, stone, rock * (1.0 - snow));
	ground = mix(ground, snow_color, snow);

	ALBEDO = ground * albedo_gain;
	ROUGHNESS = clamp(0.93 - rock * 0.19 - wetness * 0.14 - snow * 0.07, 0.52, 0.98);
	SPECULAR = mix(0.16, 0.28, wetness);
	AO = clamp(1.0 - concavity * 0.16 - slope * 0.05, 0.72, 1.0);
	}
}
