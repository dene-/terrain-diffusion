shader_type spatial;
render_mode blend_mix, cull_front, unshaded, fog_disabled, depth_draw_never;

#include "res://materials/physical_atmosphere_common.gdshaderinc"

// Depth is geometric data, not color. Filtering or mipmapping it blends
// unrelated surfaces across silhouettes and invents camera-relative depths,
// which the physical integral turns into large black patches.
uniform sampler2D DEPTH_TEXTURE : hint_depth_texture, filter_nearest, repeat_disable;
uniform vec3 sun_direction = vec3(0.0, 0.75, -0.66);
uniform float viewer_altitude_meters = 0.0;
uniform float viewer_height_signed_meters = 0.0;
uniform float maximum_distance_meters = 200000.0;
uniform float atmosphere_height_meters = 100000.0;
uniform float rayleigh_scale_height_meters = 8400.0;
uniform float mie_scale_height_meters = 1250.0;
uniform vec3 beta_rayleigh_per_meter = vec3(5.804544e-6, 1.3562913e-5, 3.311258e-5);
uniform float beta_mie_per_meter = 2.1e-5;
uniform float mie_eccentricity = 0.76;
uniform float solar_radiance = 16.0;
uniform float aerial_perspective_start_meters = 0.0;
uniform float aerial_perspective_strength = 1.0;

void vertex() {
	POSITION = vec4(VERTEX.xy, 1.0, 1.0);
}

void fragment() {
	float raw_depth = texture(DEPTH_TEXTURE, SCREEN_UV).r;
	#ifndef CURRENT_RENDERER
		raw_depth = PROJECTION_MATRIX[3][2] > PROJECTION_MATRIX[2][2]
			? raw_depth : raw_depth * 2.0 - 1.0;
	#else
		#if CURRENT_RENDERER == RENDERER_COMPATIBILITY
			raw_depth = raw_depth * 2.0 - 1.0;
		#endif
	#endif
	if (viewer_height_signed_meters < 0.0) {
		float water_distance = 240.0;
		if (raw_depth > 0.0) {
			vec3 underwater_ndc = vec3(SCREEN_UV * 2.0 - 1.0, raw_depth);
			vec4 underwater_view_h = INV_PROJECTION_MATRIX * vec4(underwater_ndc, 1.0);
			vec3 underwater_view = underwater_view_h.xyz / underwater_view_h.w;
			water_distance = min(length(underwater_view), 240.0);
		}
		vec3 water_absorption = vec3(0.018, 0.0065, 0.0032);
		vec3 water_transmittance = exp(-water_distance * water_absorption);
		float water_opacity = clamp(
			1.0 - dot(water_transmittance, vec3(0.2126, 0.7152, 0.0722)),
			0.0,
			0.997);
		float depth_darkening = smoothstep(
			0.0, 500.0, -viewer_height_signed_meters);
		vec3 underwater_scatter = mix(
			vec3(0.012, 0.095, 0.145),
			vec3(0.003, 0.018, 0.030),
			depth_darkening);
		ALBEDO = underwater_scatter;
		ALPHA = raw_depth <= 0.0 ? 1.0 : max(water_opacity, 0.35);
	} else if (raw_depth <= 0.0) {
		// The physical sky shader already integrates clear-air scattering for
		// rays without opaque geometry. Do not paint a camera-space horizon
		// strip over it; geometry below is fogged by the same coefficients.
		discard;
	} else {

	vec3 ndc = vec3(SCREEN_UV * 2.0 - 1.0, raw_depth);
	vec4 view_position_h = INV_PROJECTION_MATRIX * vec4(ndc, 1.0);
	vec3 view_position = view_position_h.xyz / view_position_h.w;
	vec4 world_position_h = INV_VIEW_MATRIX * vec4(view_position, 1.0);
	vec3 world_position = world_position_h.xyz / world_position_h.w;
	vec3 intersection_ray = world_position - CAMERA_POSITION_WORLD;
	float ray_distance = min(length(intersection_ray), maximum_distance_meters);
	vec3 ray_direction = normalize(intersection_ray);
	vec3 camera_position = vec3(0.0, max(viewer_altitude_meters, 0.0), 0.0);

	if (ray_distance <= 1.0) {
		discard;
	}
	float integration_start = clamp(
		aerial_perspective_start_meters,
		0.0,
		max(ray_distance - 1.0, 0.0));
	camera_position += ray_direction * integration_start;
	ray_distance -= integration_start;
	float strength = max(aerial_perspective_strength, 0.0);
	vec3 effective_beta_rayleigh = beta_rayleigh_per_meter * strength;
	float effective_beta_mie = beta_mie_per_meter * strength;

	const int VIEW_SAMPLES = 8;
	float step_length = ray_distance / float(VIEW_SAMPLES);
	vec2 view_columns = vec2(0.0);
	vec3 rayleigh_sum = vec3(0.0);
	vec3 mie_sum = vec3(0.0);
	float cosine_angle = dot(ray_direction, sun_direction);
	float rayleigh_phase = atmosphere_rayleigh_phase(cosine_angle);
	float mie_phase = atmosphere_mie_phase(cosine_angle, mie_eccentricity);

	for (int index = 0; index < VIEW_SAMPLES; index++) {
		float distance_along_ray = (float(index) + 0.5) * step_length;
		vec3 sample_position = camera_position + ray_direction * distance_along_ray;
		vec2 local_density = atmosphere_density(
			sample_position,
			rayleigh_scale_height_meters,
			mie_scale_height_meters);
		vec2 sample_view_columns = view_columns + local_density * (0.5 * step_length);
		vec2 light_columns = atmosphere_light_columns(
			sample_position,
			sun_direction,
			atmosphere_height_meters,
			rayleigh_scale_height_meters,
			mie_scale_height_meters);
		vec3 transmittance = atmosphere_transmittance(
			sample_view_columns + light_columns,
			effective_beta_rayleigh,
			effective_beta_mie);
		rayleigh_sum += transmittance * local_density.x * step_length;
		mie_sum += transmittance * local_density.y * step_length;
		view_columns += local_density * step_length;
	}

	vec3 view_transmittance = atmosphere_transmittance(
		view_columns, effective_beta_rayleigh, effective_beta_mie);
	vec3 in_scatter = solar_radiance * (
		effective_beta_rayleigh * rayleigh_phase * rayleigh_sum
		+ vec3(effective_beta_mie * mie_phase) * mie_sum);
	float opacity = clamp(
		1.0 - dot(view_transmittance, vec3(0.2126, 0.7152, 0.0722)),
		0.0,
		0.995);
	if (opacity <= 0.0001) {
		discard;
	}
	ALBEDO = in_scatter / opacity;
	ALPHA = opacity;
	}
}
