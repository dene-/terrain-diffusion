const float ATM_PI = 3.14159265358979323846;

// Distance through a flat, horizontally stratified atmosphere. Upward rays
// end at the atmosphere ceiling, downward rays at ground, and horizontal rays
// are bounded by the renderer's view distance.
float atmosphere_flat_path_distance(
	vec3 origin,
	vec3 direction,
	float atmosphere_height,
	float maximum_distance
) {
	if (direction.y > 1e-5) {
		return clamp(
			(atmosphere_height - origin.y) / direction.y,
			0.0,
			maximum_distance);
	}
	if (direction.y < -1e-5) {
		return clamp(origin.y / -direction.y, 0.0, maximum_distance);
	}
	return maximum_distance;
}

vec2 atmosphere_density(
	vec3 position,
	float rayleigh_scale_height,
	float mie_scale_height
) {
	float altitude = max(position.y, 0.0);
	return vec2(
		exp(-altitude / rayleigh_scale_height),
		exp(-altitude / mie_scale_height));
}

vec2 atmosphere_light_columns(
	vec3 position,
	vec3 light_direction,
	float atmosphere_height,
	float rayleigh_scale_height,
	float mie_scale_height
) {
	if (light_direction.y <= 1e-5) {
		return vec2(1e20);
	}
	float path_length = max(
		(atmosphere_height - position.y) / light_direction.y,
		0.0);
	const int LIGHT_SAMPLES = 2;
	float step_length = path_length / float(LIGHT_SAMPLES);
	vec2 columns = vec2(0.0);
	for (int index = 0; index < LIGHT_SAMPLES; index++) {
		float distance_along_ray = (float(index) + 0.5) * step_length;
		columns += atmosphere_density(
			position + light_direction * distance_along_ray,
			rayleigh_scale_height,
			mie_scale_height);
	}
	return columns * step_length;
}

float atmosphere_rayleigh_phase(float cosine_angle) {
	return 3.0 * (1.0 + cosine_angle * cosine_angle) / (16.0 * ATM_PI);
}

float atmosphere_mie_phase(float cosine_angle, float eccentricity) {
	float g2 = eccentricity * eccentricity;
	float denominator = max(
		pow(1.0 + g2 - 2.0 * eccentricity * cosine_angle, 1.5),
		1e-4);
	return 3.0 * (1.0 - g2) * (1.0 + cosine_angle * cosine_angle)
		/ (8.0 * ATM_PI * (2.0 + g2) * denominator);
}

vec3 atmosphere_transmittance(
	vec2 columns,
	vec3 beta_rayleigh,
	float beta_mie
) {
	return exp(-min(
		beta_rayleigh * columns.x + vec3(beta_mie * columns.y),
		vec3(50.0)));
}
