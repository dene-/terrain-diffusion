shader_type spatial;
render_mode unshaded, depth_draw_opaque, cull_disabled, fog_disabled;

// Flat 1:1-scale ocean contract. This opaque camera-relative surface never
// reads scene color/depth and never exposes the seabed by blending. Every
// distance, wavelength and amplitude is expressed in metres.
uniform vec3 deep_water_color : source_color = vec3(0.035, 0.22, 0.34);
uniform vec3 surface_water_color : source_color = vec3(0.06, 0.38, 0.52);
uniform vec3 horizon_sky_color : source_color = vec3(0.46, 0.64, 0.76);
uniform vec3 sun_light_color : source_color = vec3(1.0, 0.96, 0.88);
uniform vec3 sun_direction_world = vec3(0.0, 0.75, -0.66);
uniform float daylight = 1.0;

uniform float macro_wavelength_meters = 260.0;
uniform float macro_height_meters = 1.15;
uniform float detail_wavelength_meters = 13.0;
uniform float detail_normal_strength = 0.72;
uniform float wave_time_scale = 0.42;
uniform float wave_fade_start_meters = 450.0;
uniform float wave_fade_end_meters = 3200.0;

uniform vec2 floating_origin_xz = vec2(0.0);
uniform vec2 camera_absolute_xz = vec2(0.0);
uniform float sea_level_meters = -0.08;
uniform float ocean_extent_scale = 1.0;
uniform float ocean_view_distance_meters = 240000.0;
uniform float viewer_underwater = 0.0;

varying vec2 ocean_absolute_xz;
varying vec3 ocean_world_position;
varying vec3 ocean_world_normal;
varying float ocean_surface_distance;
varying float ocean_camera_distance;
varying float ocean_crest;

const float GRAVITY = 9.80665;
const float TAU_METRIC = 6.28318530718;

// height, dH/dX, dH/dZ for a deep-water gravity wave.
vec3 metric_wave(
	vec2 position,
	vec2 direction,
	float wavelength,
	float amplitude,
	float phase_offset
) {
	vec2 heading = normalize(direction);
	float k = TAU_METRIC / max(wavelength, 0.25);
	float omega = sqrt(GRAVITY * k);
	float phase = k * dot(position, heading)
		- omega * TIME * wave_time_scale + phase_offset;
	float slope = amplitude * k * cos(phase);
	return vec3(
		amplitude * sin(phase),
		slope * heading.x,
		slope * heading.y);
}

vec3 macro_wave_field(vec2 position) {
	float wavelength = max(macro_wavelength_meters, 25.0);
	float height = max(macro_height_meters, 0.0);
	vec3 wave = metric_wave(
		position, vec2(0.94, 0.34), wavelength, height * 0.60, 0.0);
	wave += metric_wave(
		position, vec2(-0.38, 0.92), wavelength * 0.46, height * 0.27, 2.1);
	wave += metric_wave(
		position, vec2(0.57, 0.82), wavelength * 0.24, height * 0.13, 4.7);
	return wave;
}

vec3 detail_wave_field(vec2 position) {
	float wavelength = max(detail_wavelength_meters, 1.0);
	vec3 wave = metric_wave(
		position, vec2(0.88, 0.48), wavelength, 0.18, 0.8);
	wave += metric_wave(
		position, vec2(-0.52, 0.85), wavelength * 0.48, 0.065, 2.9);
	wave += metric_wave(
		position, vec2(0.18, 0.98), wavelength * 0.25, 0.024, 5.2);
	return wave;
}

void vertex() {
	const float BASE_HALF_EXTENT_METERS = 32768.0;
	vec2 normalized = VERTEX.xz / BASE_HALF_EXTENT_METERS;
	float extent_scale = max(ocean_extent_scale, 1.0);
	// Keep roughly 16 metre cells near the camera as the far extent grows.
	float near_fraction = 0.065 / extent_scale;
	vec2 distributed = normalized * (
		vec2(near_fraction) + vec2(1.0 - near_fraction) * abs(normalized));
	vec2 surface_offset = distributed
		* BASE_HALF_EXTENT_METERS * extent_scale;
	ocean_surface_distance = length(surface_offset);
	ocean_absolute_xz = camera_absolute_xz + surface_offset;

	vec2 camera_local_xz = camera_absolute_xz - floating_origin_xz;
	vec3 flat_world_position = vec3(
		camera_local_xz.x + surface_offset.x,
		sea_level_meters,
		camera_local_xz.y + surface_offset.y);
	ocean_camera_distance = length(flat_world_position - CAMERA_POSITION_WORLD);

	float wave_visibility = 1.0 - smoothstep(
		wave_fade_start_meters,
		max(wave_fade_end_meters, wave_fade_start_meters + 1.0),
		ocean_camera_distance);
	vec3 wave = macro_wave_field(ocean_absolute_xz) * wave_visibility;
	ocean_world_position = flat_world_position + vec3(0.0, wave.x, 0.0);
	vec3 original_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	VERTEX += ocean_world_position - original_world_position;

	ocean_world_normal = normalize(vec3(-wave.y, 1.0, -wave.z));
	NORMAL = ocean_world_normal;
	ocean_crest = macro_height_meters > 0.001
		? clamp(0.5 + 0.5 * wave.x / macro_height_meters, 0.0, 1.0)
		: 0.0;
}

void fragment() {
	float detail_visibility = 1.0 - smoothstep(
		wave_fade_start_meters,
		max(wave_fade_end_meters, wave_fade_start_meters + 1.0),
		ocean_camera_distance);
	vec3 detail_wave = detail_wave_field(ocean_absolute_xz);
	vec3 world_normal = normalize(ocean_world_normal + vec3(
		-detail_wave.y,
		0.0,
		-detail_wave.z) * detail_normal_strength * detail_visibility);
	if (viewer_underwater > 0.5) {
		world_normal = -world_normal;
	}

	vec3 world_view = normalize(CAMERA_POSITION_WORLD - ocean_world_position);
	float normal_to_view = clamp(dot(world_normal, world_view), 0.0, 1.0);
	float fresnel = 0.02 + 0.98 * pow(1.0 - normal_to_view, 5.0);
	vec3 reflection_direction = reflect(-world_view, world_normal);
	float reflected_height = clamp(reflection_direction.y * 0.5 + 0.5, 0.0, 1.0);
	vec3 reflected_sky = mix(
		horizon_sky_color,
		horizon_sky_color * vec3(0.52, 0.70, 1.02),
		pow(reflected_height, 0.65));

	float crest = smoothstep(0.84, 0.99, ocean_crest) * detail_visibility;
	// Opaque water must retain visible body radiance at close range; otherwise
	// the physically small F0 reflection leaves a near-normal view almost black.
	vec3 water_body = mix(deep_water_color, surface_water_color, 0.38 + crest * 0.12);
	vec3 color = mix(water_body, reflected_sky, fresnel * 0.92);
	float sun_alignment = max(dot(reflection_direction, normalize(sun_direction_world)), 0.0);
	float sun_glint = pow(sun_alignment, 900.0) * daylight;
	color += sun_light_color * sun_glint * 2.4;

	float horizon_ratio = ocean_surface_distance / max(ocean_view_distance_meters, 1.0);
	color = mix(color, horizon_sky_color, smoothstep(0.82, 0.995, horizon_ratio) * 0.82);
	if (viewer_underwater > 0.5) {
		color = deep_water_color * 0.34;
	}
	ALBEDO = color;
}
