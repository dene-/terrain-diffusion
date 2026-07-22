#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform writeonly image2D cloud_image;
layout(r32f, set = 0, binding = 1) uniform writeonly image2D scene_depth_image;
layout(set = 0, binding = 2) uniform sampler2D scene_depth_texture;
layout(set = 0, binding = 3) uniform sampler2D weather_texture;
layout(set = 0, binding = 4) uniform sampler3D shape_noise_texture;
layout(set = 0, binding = 5) uniform sampler3D detail_noise_texture;

layout(std140, set = 0, binding = 6) uniform FrameData {
	mat4 inverse_projection;
	mat4 camera_to_world;
	vec4 full_and_low_size;
	vec4 layer;
	vec4 fade_weather;
	vec4 density_shape;
	vec4 sun_direction_energy;
	vec4 sun_color_ambient;
	vec4 weather_offset_extinction;
	vec4 march;
	vec4 phase;
} frame;

const int MAX_RAY_STEPS = 96;
const int MAX_LIGHT_STEPS = 6;
const float LARGE_DISTANCE = 1.0e20;
const float PI = 3.14159265359;


float remap_clamped(float value, float old_min, float old_max) {
	return clamp((value - old_min) / max(old_max - old_min, 1.0e-5), 0.0, 1.0);
}


float henyey_greenstein(float cosine, float eccentricity) {
	float g2 = eccentricity * eccentricity;
	float denominator = pow(max(1.0 + g2 - 2.0 * eccentricity * cosine, 1.0e-3), 1.5);
	return (1.0 - g2) / (4.0 * PI * denominator);
}


float interleaved_gradient_noise(vec2 pixel) {
	// Stable low-discrepancy sampling. The spatial resolve removes this pattern
	// without the crawling caused by a frame-varying random seed.
	return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}


float height_profile(float normalized_height, float weather_value) {
	float soft_base = smoothstep(0.0, 0.13, normalized_height);
	float top_start = mix(0.58, 0.76, weather_value);
	float soft_top = 1.0 - smoothstep(top_start, 1.0, normalized_height);
	return soft_base * soft_top;
}


void sample_weather(
	vec2 advected_xz,
	out float weather_value,
	out float local_coverage
) {
	weather_value = textureLod(
		weather_texture, advected_xz / frame.fade_weather.z, 0.0).r;
	weather_value = smoothstep(0.12, 0.88, weather_value);
	local_coverage = clamp(
		frame.fade_weather.w * mix(0.42, 1.42, weather_value),
		0.01,
		0.98);
}


float sample_cloud_density(vec3 world_position, float detail_weight) {
	float base_altitude = frame.layer.x;
	float top_altitude = frame.layer.y;
	float normalized_height = remap_clamped(world_position.y, base_altitude, top_altitude);
	if (normalized_height <= 0.0 || normalized_height >= 1.0) {
		return 0.0;
	}

	// Sampling opposite the accumulated wind offset makes visible cloud motion
	// follow the authored wind direction in world space.
	vec2 advected_xz = world_position.xz - frame.weather_offset_extinction.xy;
	float weather_value;
	float local_coverage;
	sample_weather(advected_xz, weather_value, local_coverage);
	float shape_scale = frame.density_shape.y;
	vec3 shape_coordinate = vec3(
		advected_xz.x / shape_scale,
		world_position.y / shape_scale,
		advected_xz.y / shape_scale);
	float shape_a = textureLod(shape_noise_texture, shape_coordinate, 0.0).r;
	float shape_b = textureLod(
		shape_noise_texture,
		shape_coordinate * 1.91 + vec3(0.173, 0.417, 0.731),
		0.0).r;
	float shape = mix(shape_a, shape_b, 0.31);
	float cloud = remap_clamped(shape, 1.0 - local_coverage, 1.0);
	cloud *= height_profile(normalized_height, weather_value);

	if (detail_weight > 0.001 && cloud > 0.001) {
		float detail_scale = frame.density_shape.z;
		vec3 detail_coordinate = vec3(
			advected_xz.x / detail_scale,
			world_position.y / detail_scale,
			advected_xz.y / detail_scale);
		float detail = textureLod(detail_noise_texture, detail_coordinate, 0.0).r;
		float erosion = (1.0 - detail) * frame.density_shape.w * detail_weight;
		cloud = max(cloud - erosion * (0.18 + 0.32 * (1.0 - cloud)), 0.0);
	}

	return clamp(cloud * frame.density_shape.x, 0.0, 1.0);
}


float sample_light_density(
	vec3 world_position,
	float weather_value,
	float local_coverage
) {
	float normalized_height = remap_clamped(
		world_position.y, frame.layer.x, frame.layer.y);
	if (normalized_height <= 0.0 || normalized_height >= 1.0) {
		return 0.0;
	}

	vec2 advected_xz = world_position.xz - frame.weather_offset_extinction.xy;
	vec3 shape_coordinate = vec3(
		advected_xz.x / frame.density_shape.y,
		world_position.y / frame.density_shape.y,
		advected_xz.y / frame.density_shape.y);
	// Sun transmittance only needs the low-frequency body of the cloud. Using
	// one shape octave here produces soft physical self-shadowing while avoiding
	// the expensive detail work repeated inside every nested light march.
	float shape = textureLod(shape_noise_texture, shape_coordinate, 0.0).r;
	float cloud = remap_clamped(shape, 1.0 - local_coverage, 1.0);
	cloud *= height_profile(normalized_height, weather_value);
	return clamp(cloud * frame.density_shape.x, 0.0, 1.0);
}


float sample_sun_transmittance(vec3 world_position) {
	vec3 sun_direction = normalize(frame.sun_direction_energy.xyz);
	int light_step_count = clamp(int(frame.march.y + 0.5), 1, MAX_LIGHT_STEPS);
	vec2 advected_xz = world_position.xz - frame.weather_offset_extinction.xy;
	float weather_value;
	float local_coverage;
	sample_weather(advected_xz, weather_value, local_coverage);
	float layer_boundary = sun_direction.y >= 0.0 ? frame.layer.y : frame.layer.x;
	float distance_to_boundary;
	if (abs(sun_direction.y) > 1.0e-4) {
		distance_to_boundary = max(
			(layer_boundary - world_position.y) / sun_direction.y,
			0.0);
	} else {
		// The flat cloud layer is intentionally finite for near-horizontal light;
		// beyond this distance its remaining contribution is negligible.
		distance_to_boundary = 12000.0;
	}
	distance_to_boundary = min(distance_to_boundary, 12000.0);
	float interval = distance_to_boundary / float(light_step_count);
	float optical_depth = 0.0;
	for (int index = 0; index < MAX_LIGHT_STEPS; index++) {
		if (index >= light_step_count) {
			break;
		}
		float sample_distance = (float(index) + 0.5) * interval;
		optical_depth += sample_light_density(
			world_position + sun_direction * sample_distance,
			weather_value,
			local_coverage) * interval;
	}
	return exp(-optical_depth * frame.weather_offset_extinction.z);
}


void make_view_ray(vec2 uv, out vec3 ray_origin, out vec3 ray_direction) {
	vec2 ndc = uv * 2.0 - 1.0;
	vec4 near_homogeneous = frame.inverse_projection * vec4(ndc, 1.0, 1.0);
	vec4 far_homogeneous = frame.inverse_projection * vec4(ndc, 0.0, 1.0);
	vec3 near_view = near_homogeneous.xyz / max(abs(near_homogeneous.w), 1.0e-6);
	vec3 far_view = far_homogeneous.xyz / max(abs(far_homogeneous.w), 1.0e-6);
	bool orthogonal = frame.march.w > 0.5;
	if (orthogonal) {
		ray_origin = (frame.camera_to_world * vec4(near_view, 1.0)).xyz;
		ray_direction = normalize(mat3(frame.camera_to_world) * (far_view - near_view));
	} else {
		ray_origin = frame.camera_to_world[3].xyz;
		ray_direction = normalize(mat3(frame.camera_to_world) * far_view);
	}
}


float reconstruct_scene_distance(vec2 uv, float raw_depth) {
	float maximum_distance = frame.layer.w;
	if (raw_depth <= 1.0e-7) {
		return maximum_distance;
	}
	vec2 ndc = uv * 2.0 - 1.0;
	vec4 view_homogeneous = frame.inverse_projection * vec4(ndc, raw_depth, 1.0);
	vec3 view_position = view_homogeneous.xyz / max(abs(view_homogeneous.w), 1.0e-6);
	if (frame.march.w > 0.5) {
		vec4 near_homogeneous = frame.inverse_projection * vec4(ndc, 1.0, 1.0);
		vec3 near_view = near_homogeneous.xyz / max(abs(near_homogeneous.w), 1.0e-6);
		return min(length(view_position - near_view), maximum_distance);
	}
	return min(length(view_position), maximum_distance);
}


void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 low_size = imageSize(cloud_image);
	if (any(greaterThanEqual(pixel, low_size))) {
		return;
	}

	vec2 uv = (vec2(pixel) + 0.5) / vec2(low_size);
	float raw_depth = textureLod(scene_depth_texture, uv, 0.0).r;
	float scene_distance = reconstruct_scene_distance(uv, raw_depth);
	imageStore(scene_depth_image, pixel, vec4(scene_distance));

	vec3 ray_origin;
	vec3 ray_direction;
	make_view_ray(uv, ray_origin, ray_direction);
	float sea_level = frame.layer.z;
	if (ray_origin.y < sea_level - 0.25) {
		imageStore(cloud_image, pixel, vec4(0.0));
		return;
	}
	if (ray_direction.y < -1.0e-5) {
		float ocean_distance = (sea_level - ray_origin.y) / ray_direction.y;
		if (ocean_distance > 0.0) {
			scene_distance = min(scene_distance, ocean_distance);
		}
	}

	float inverse_vertical_direction = 1.0 / max(abs(ray_direction.y), 1.0e-5);
	float layer_distance_a = (frame.layer.x - ray_origin.y) * inverse_vertical_direction;
	float layer_distance_b = (frame.layer.y - ray_origin.y) * inverse_vertical_direction;
	if (ray_direction.y < 0.0) {
		layer_distance_a = -layer_distance_a;
		layer_distance_b = -layer_distance_b;
	}
	float entry_distance = max(min(layer_distance_a, layer_distance_b), 0.0);
	float exit_distance = min(
		max(layer_distance_a, layer_distance_b),
		min(scene_distance, frame.layer.w));
	if (exit_distance <= entry_distance) {
		imageStore(cloud_image, pixel, vec4(0.0));
		return;
	}

	float segment_length = exit_distance - entry_distance;
	int configured_steps = clamp(int(frame.march.x + 0.5), 16, MAX_RAY_STEPS);
	int step_count = clamp(int(ceil(segment_length / 220.0)), 12, configured_steps);
	float step_length = segment_length / float(step_count);
	float jitter = (interleaved_gradient_noise(vec2(pixel)) - 0.5) * frame.march.z;
	float travel = entry_distance + step_length * (0.5 + jitter);
	float transmittance = 1.0;
	vec3 radiance = vec3(0.0);
	vec3 sun_direction = normalize(frame.sun_direction_energy.xyz);
	float view_sun_cosine = dot(ray_direction, sun_direction);
	float phase_forward = henyey_greenstein(view_sun_cosine, frame.phase.x);
	float phase_backward = henyey_greenstein(view_sun_cosine, frame.phase.y);
	float phase_value = mix(phase_forward, phase_backward, frame.phase.z) * 4.0 * PI;
	float extinction = frame.weather_offset_extinction.z;

	for (int index = 0; index < MAX_RAY_STEPS; index++) {
		if (index >= step_count || transmittance < 0.015) {
			break;
		}
		vec3 sample_position = ray_origin + ray_direction * travel;
		// Fine erosion cannot be sampled reliably once a ray step spans a large
		// fraction of its wavelength. Fade it out instead of aliasing at the horizon.
		float detail_distance_weight = 1.0 - smoothstep(18000.0, 65000.0, travel);
		float detail_sampling_weight = clamp(320.0 / max(step_length, 1.0), 0.0, 1.0);
		float cloud_density = sample_cloud_density(
			sample_position,
			detail_distance_weight * detail_sampling_weight);
		if (cloud_density > 0.001) {
			float step_opacity = 1.0 - exp(-cloud_density * extinction * step_length);
			float sun_transmittance = sample_sun_transmittance(sample_position);
			float normalized_height = remap_clamped(
				sample_position.y, frame.layer.x, frame.layer.y);
			vec3 ambient = vec3(0.68, 0.74, 0.82)
				* frame.sun_color_ambient.w
				* mix(0.72, 1.05, normalized_height);
			// Water droplets have a near-unity single-scattering albedo. Dense clouds
			// rapidly isotropize part of the sunlight, so a phase-only model makes
			// cloud tops unrealistically gray when viewed away from the sun.
			float multiple_scattering_weight = mix(
				0.55, 0.78, 1.0 - sun_transmittance);
			float effective_phase = mix(
				phase_value, 1.0, multiple_scattering_weight);
			vec3 direct = frame.sun_color_ambient.xyz
				* frame.sun_direction_energy.w
				* sqrt(max(sun_transmittance, 0.0))
				* effective_phase
				* 0.86;
			radiance += transmittance * step_opacity * (ambient + direct);
			transmittance *= 1.0 - step_opacity;
		}
		travel += step_length;
	}

	float distance_fade = 1.0 - smoothstep(
		frame.fade_weather.x,
		frame.fade_weather.y,
		entry_distance + segment_length * 0.5);
	float opacity = (1.0 - transmittance) * distance_fade;
	radiance *= distance_fade;
	imageStore(cloud_image, pixel, vec4(radiance, opacity));
}
