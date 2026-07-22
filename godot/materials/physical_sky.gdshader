shader_type sky;

#include "res://materials/physical_atmosphere_common.gdshaderinc"

uniform vec3 sun_direction = vec3(0.0, 0.75, -0.66);
uniform float viewer_altitude_meters = 0.0;
uniform float atmosphere_height_meters = 100000.0;
uniform float maximum_distance_meters = 240000.0;
uniform float rayleigh_scale_height_meters = 8400.0;
uniform float mie_scale_height_meters = 1250.0;
uniform vec3 beta_rayleigh_per_meter = vec3(5.804544e-6, 1.3562913e-5, 3.311258e-5);
uniform float beta_mie_per_meter = 2.1e-5;
uniform float mie_eccentricity = 0.76;
uniform float solar_radiance = 16.0;
uniform float sun_angular_radius_radians = 0.004675;
uniform vec3 ground_albedo = vec3(0.10, 0.12, 0.085);
uniform vec3 night_radiance = vec3(0.00008, 0.00012, 0.00022);

vec3 integrate_sky_radiance(vec3 view_direction) {
	vec3 camera_position = vec3(0.0, max(viewer_altitude_meters, 0.0), 0.0);
	float path_length = atmosphere_flat_path_distance(
		camera_position,
		view_direction,
		atmosphere_height_meters,
		maximum_distance_meters);
	bool hits_ground = view_direction.y < -1e-5;

	const int VIEW_SAMPLES = 8;
	float step_length = path_length / float(VIEW_SAMPLES);
	vec2 view_columns = vec2(0.0);
	vec3 rayleigh_sum = vec3(0.0);
	vec3 mie_sum = vec3(0.0);
	float cosine_angle = dot(view_direction, sun_direction);
	float rayleigh_phase = atmosphere_rayleigh_phase(cosine_angle);
	float mie_phase = atmosphere_mie_phase(cosine_angle, mie_eccentricity);

	for (int index = 0; index < VIEW_SAMPLES; index++) {
		float distance_along_ray = (float(index) + 0.5) * step_length;
		vec3 sample_position = camera_position + view_direction * distance_along_ray;
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
			beta_rayleigh_per_meter,
			beta_mie_per_meter);
		rayleigh_sum += transmittance * local_density.x * step_length;
		mie_sum += transmittance * local_density.y * step_length;
		view_columns += local_density * step_length;
	}

	vec3 radiance = solar_radiance * (
		beta_rayleigh_per_meter * rayleigh_phase * rayleigh_sum
		+ vec3(beta_mie_per_meter * mie_phase) * mie_sum);

	if (hits_ground) {
		vec3 view_transmittance = atmosphere_transmittance(
			view_columns, beta_rayleigh_per_meter, beta_mie_per_meter);
		float ground_light = max(sun_direction.y, 0.0);
		radiance += ground_albedo * ground_light * view_transmittance * 0.08;
	}

	float sun_cosine = dot(view_direction, sun_direction);
	float sun_threshold = cos(sun_angular_radius_radians);
	if (!hits_ground && sun_cosine > sun_threshold) {
		vec2 sun_columns = atmosphere_light_columns(
			camera_position,
			sun_direction,
			atmosphere_height_meters,
			rayleigh_scale_height_meters,
			mie_scale_height_meters);
		vec3 sun_transmittance = atmosphere_transmittance(
			sun_columns, beta_rayleigh_per_meter, beta_mie_per_meter);
		float disc = smoothstep(sun_threshold, mix(sun_threshold, 1.0, 0.18), sun_cosine);
		radiance += sun_transmittance * solar_radiance * disc;
	}

	return radiance + night_radiance;
}

void sky() {
	COLOR = integrate_sky_radiance(normalize(EYEDIR));
}
