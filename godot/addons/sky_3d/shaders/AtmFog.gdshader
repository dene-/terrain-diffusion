// Copyright (c) 2023-2025 Cory Petkovsek and Contributors
// Copyright (c) 2021 J. Cuellar

shader_type spatial;
render_mode blend_mix, cull_front, unshaded, fog_disabled, depth_draw_never;

global uniform float terrain_camera_altitude_meters;


uniform vec2 color_correction;

uniform float sea_level: hint_range(-2048,2048,.01) = 0.0;
uniform float fog_density = .0001;
uniform float fog_rayleigh_depth = .115;
uniform float fog_mie_depth = 0.;
uniform float fog_falloff = 3.;
uniform float fog_start = 0.;
uniform float fog_end = 1000.;
uniform float fog_height_density = .000118;
uniform vec3 sun_direction = vec3(.25, -.25, .25);
uniform vec3 moon_direction = vec3(.45, -.25, .45);

uniform float atm_darkness = .5;
uniform float atm_sun_intensity = 30.;
uniform vec4 atm_day_tint: source_color = vec4(.78, .85, .98, 1.);
uniform vec4 atm_horizon_light_tint: source_color = vec4(.98, .73, .49, 1.);
uniform vec4 atm_night_tint: source_color = vec4(.16, .2, .25, 1.);
uniform vec3 atm_level_params = vec3(1., 0., 0.);
uniform float atm_thickness = .7;

uniform vec3 atm_beta_ray;
uniform vec3 atm_beta_mie;

uniform vec3 atm_sun_partial_mie_phase;
uniform vec4 atm_sun_mie_tint: source_color = vec4(1.);
uniform float atm_sun_mie_intensity = 1.;

uniform vec3 atm_moon_partial_mie_phase;
uniform vec4 atm_moon_mie_tint: source_color = vec4(.13, .18, .29, 1.);
uniform float atm_moon_mie_intensity = .7;

uniform sampler2D DEPTH_TEXTURE : hint_depth_texture, filter_linear_mipmap;

varying vec4 v_angle_mult;

#include "Common.gdshaderinc"

// Atmosphere
vec3 calc_atmospheric_scatter(float sr, float sm, vec2 mu, vec3 mult, float depth) {
	vec3 beta_mie = atm_beta_mie;
	vec3 beta_ray = atm_beta_ray * atm_thickness;

	vec3 extc_factor = clamp_rgb_to_unit(exp(-(beta_ray * sr + beta_mie * sm)));

	float extc_ff = mix(clamp_to_unit(atm_thickness * 0.5), 1.0, mult.x);
	vec3 final_extc_factor = mix(1.0 - extc_factor, (1.0 - extc_factor) * extc_factor, extc_ff);

	float rayleigh_phase = calc_rayleigh_phase(mu.x);
	vec3 BRT = beta_ray * rayleigh_phase * clamp_to_unit(depth * fog_rayleigh_depth);
	vec3 BMT = beta_mie * calc_mie_phase(mu.x, atm_sun_partial_mie_phase);
	BMT *= atm_sun_mie_intensity * atm_sun_mie_tint.rgb * clamp_to_unit(depth * fog_mie_depth);

	vec3 BRMT = (BRT + BMT) / (beta_ray + beta_mie);
	vec3 scatter = atm_sun_intensity * (BRMT * final_extc_factor) * atm_day_tint.rgb * mult.y;
	scatter = mix(scatter, scatter * (1.0 - extc_factor), atm_darkness);

	vec3 lcol = mix(atm_day_tint.rgb, atm_horizon_light_tint.rgb, mult.x);
	vec3 nscatter = (1.0 - extc_factor) * atm_night_tint.rgb * clamp_to_unit(depth * fog_rayleigh_depth);
	nscatter += calc_mie_phase(mu.y, atm_moon_partial_mie_phase) *
		atm_moon_mie_tint.rgb * atm_moon_mie_intensity * 0.005 * clamp_to_unit(depth * fog_mie_depth);

	return (scatter * lcol) + nscatter;
}

// Fog
float calc_fog_exp(float depth, float density) {
	return 1.0 - clamp_to_unit(exp2(-depth * density));
}

float calc_fog_falloff(float y, float zeroLevel, float falloff) {
	return clamp_to_unit(exp(-(y + zeroLevel) * falloff));
}

float calc_fog_height_density(float altitude) {
	return exp(-max(altitude - sea_level, 0.0) * fog_height_density);
}

// Exact mean density for an exponential atmosphere along a linearly changing
// altitude. Using a clamped logarithmic mean overestimated the optical depth
// of orbital rays after the upper endpoint underflowed toward zero.
float calc_average_fog_height_density(float start_altitude, float end_altitude) {
	float start_density = calc_fog_height_density(start_altitude);
	float end_density = calc_fog_height_density(end_altitude);
	float altitude_delta = end_altitude - start_altitude;
	if (fog_height_density <= 1e-9 || abs(altitude_delta) < 0.1) {
		return 0.5 * (start_density + end_density);
	}
	return clamp_to_unit(
		(start_density - end_density) /
		(fog_height_density * altitude_delta));
}

// Integrate molecular and aerosol columns through a spherical atmosphere.
// A flat, endpoint-averaged density makes a real horizon either disappear or
// turn into a uniform distance tint. Midpoint integration follows the curved
// ray's physical altitude and naturally clears above the dense lower air.
vec2 integrate_air_columns(vec3 ray_direction, float ray_distance) {
	const float planet_radius = 6371000.0;
	const float rayleigh_scale_height = 8400.0;
	const float mie_scale_height = 1250.0;
	const int sample_count = 8;
	float segment_length = ray_distance / float(sample_count);
	float rayleigh_column = 0.0;
	float mie_column = 0.0;
	for (int sample_index = 0; sample_index < sample_count; sample_index++) {
		float distance_along_ray = (float(sample_index) + 0.5) * segment_length;
		vec3 relative_position = ray_direction * distance_along_ray;
		float altitude = length(vec3(
			relative_position.x,
			planet_radius + terrain_camera_altitude_meters + relative_position.y,
			relative_position.z)) - planet_radius;
		altitude = max(altitude - sea_level, 0.0);
		rayleigh_column += exp(-altitude / rayleigh_scale_height);
		mie_column += exp(-altitude / mie_scale_height);
	}
	return vec2(rayleigh_column, mie_column) * segment_length;
}


// Main

void vertex() {
	POSITION = vec4(VERTEX.xy, 1.0, 1.0);
	v_angle_mult = vec4(
		clamp_to_unit(1.0 - sun_direction.y),
		clamp_to_unit(sun_direction.y + 0.45),
		clamp_to_unit(-sun_direction.y + 0.30),
		clamp_to_unit(-sun_direction.y + 0.60));
}

void fragment() {
	// Calculate world pixel position from depth
	float depth_raw = texture(DEPTH_TEXTURE, SCREEN_UV).r;
	#ifndef CURRENT_RENDERER
		// For 4.3 or earlier, will break if far plane is less than 1m.
		depth_raw = PROJECTION_MATRIX[3][2] > PROJECTION_MATRIX[2][2] ? depth_raw : depth_raw * 2.0 - 1.0;
	#else
		#if CURRENT_RENDERER == RENDERER_COMPATIBILITY
			depth_raw = depth_raw * 2.0 - 1.0;
		#endif
	#endif
	// Reconstructing Forward+'s exact zero background depth produces an
	// infinite endpoint and an undefined direction. Reject it before any ray
	// math; Sky3D already renders atmospheric scattering for the sky itself.
	if (depth_raw <= 0.0) discard;
	vec3 ndc = vec3(SCREEN_UV * 2.0 - 1.0, depth_raw);
	vec4 clip_pos = INV_PROJECTION_MATRIX * vec4(ndc, 1.0);
	vec3 view_pos = clip_pos.xyz / clip_pos.w;
	vec4 world_pos_hom = INV_VIEW_MATRIX * vec4(view_pos, 1.0);
	vec3 world_position = world_pos_hom.xyz / world_pos_hom.w;
	
	// Ray from camera to world intersection point
	vec3 intersection_ray = world_position - CAMERA_POSITION_WORLD;
	// Unit ray direction
	vec3 ray_direction = normalize(intersection_ray);
	//float linear_depth = -view_pos.z; // Approximate ray distance
	//float ray_distance = length(intersection_ray); // Exact ray distance

	// Cap ray if there's a sea_level plane intersection
	float original_distance = length(intersection_ray);
	// Y difference from camera to sea_level
	float dy = sea_level - CAMERA_POSITION_WORLD.y;
	// Distance along ray to sea_level plane
	float plane_distance = dy / (ray_direction.y + 1e-6 * sign(ray_direction.y));  // avoid divide by zero
	float plane_hit = float(ray_direction.y < 0.) *
					  float(plane_distance > 0.) *
					  float(plane_distance < original_distance);
	
	// Use final distance or t_plane distance Final distance: mix original vs plane distance
	float ray_distance = mix(original_distance, plane_distance, plane_hit);
	
	// Capped world position
	//world_position = CAMERA_POSITION_WORLD + ray_distance * ray_direction;

	// fog_start removes a near-camera interval and fog_end is only the physical
	// integration cap. Extinction comes from wavelength-dependent Rayleigh and
	// low-altitude aerosol columns, not an arbitrary screen-space fog ramp.
	float optical_distance = clamp(
		ray_distance - fog_start,
		0.0,
		max(fog_end - fog_start, 0.0));
	vec2 air_columns = integrate_air_columns(ray_direction, optical_distance);
	vec3 beta_rayleigh = atm_beta_ray * atm_thickness;
	vec3 beta_aerosol = max(atm_beta_mie, vec3(max(fog_density, 0.0)));
	vec3 transmittance = exp(-(
		beta_rayleigh * air_columns.x + beta_aerosol * air_columns.y));
	float fog_factor = clamp_to_unit(1.0 - dot(
		transmittance, vec3(0.2126, 0.7152, 0.0722)));
	vec2 mu = vec2(
		dot(sun_direction, ray_direction),
		dot(moon_direction, ray_direction)
	);
	float horizon_amount = 1.0 - smoothstep(0.0, 0.65, abs(ray_direction.y));
	float sun_forward = pow(clamp_to_unit(mu.x), 12.0);
	float day_amount = v_angle_mult.y;
	vec3 scatter = mix(atm_night_tint.rgb, atm_day_tint.rgb, day_amount);
	// Aerial in-scattering approaches the visible sky radiance at long optical
	// paths. Keeping this source artificially dark left a navy ocean/sky band
	// even when the extinction itself was physically correct.
	scatter *= mix(0.42, 0.82, horizon_amount);
	scatter += atm_horizon_light_tint.rgb * sun_forward
		* horizon_amount * day_amount * 0.22;

	vec4 fog_color = vec4(scatter, fog_factor);
	fog_color.a = clamp(fog_color.a, 0.0, 1.0);
	fog_color.rgb = apply_photo_tonemap(fog_color.rgb, color_correction.y, color_correction.x);

	ALBEDO = fog_color.rgb;
	ALPHA = fog_color.a;

	// View depth map
	//ALPHA = 1.;
	//ALBEDO = vec3(ray_distance*.0001);
}
