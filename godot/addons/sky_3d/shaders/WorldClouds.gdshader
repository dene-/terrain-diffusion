shader_type spatial;
render_mode blend_mix, cull_front, unshaded, fog_disabled, depth_draw_never;

// Depth-aware cumulus volume in a real Earth-radius altitude shell. Unlike
// the sky material, this pass can appear in front of terrain when the camera
// flies above the clouds and remains anchored in absolute map coordinates.
global uniform vec2 terrain_camera_absolute_xz;
global uniform float terrain_camera_altitude_meters;

uniform sampler2D DEPTH_TEXTURE : hint_depth_texture, filter_linear_mipmap;
uniform sampler2D cloud_noise : repeat_enable, filter_linear_mipmap;
uniform float cloud_base_meters = 2200.0;
uniform float cloud_top_meters = 4500.0;
uniform float cloud_scale_meters = 24000.0;
uniform float cloud_coverage = 0.55;
uniform float cloud_absorption = 2.0;
uniform float cloud_intensity = 0.6;
uniform vec2 wind_offset = vec2(0.0);
uniform vec3 sun_direction = vec3(0.25, 0.65, 0.25);
uniform vec4 night_color : source_color = vec4(0.09, 0.094, 0.129, 1.0);

const float PLANET_RADIUS_METERS = 6371000.0;
const int CLOUD_STEPS = 12;

float first_positive_sphere_hit(vec3 ray_origin, vec3 ray_direction, float radius) {
	float b = dot(ray_origin, ray_direction);
	float c = dot(ray_origin, ray_origin) - radius * radius;
	float discriminant = b * b - c;
	if (discriminant < 0.0) return -1.0;
	float root = sqrt(discriminant);
	float near_hit = -b - root;
	float far_hit = -b + root;
	return near_hit > 0.0 ? near_hit : (far_hit > 0.0 ? far_hit : -1.0);
}

float cloud_noise_field(vec3 absolute_position, float normalized_height) {
	float scale = max(cloud_scale_meters, 1000.0);
	vec2 base_uv = absolute_position.xz / scale + wind_offset;
	base_uv += vec2(normalized_height * 0.005, -normalized_height * 0.004);
	// The bundled cloud texture is intentionally noisy at its top mip. Sample
	// coarse mips here so it becomes kilometre-scale weather structure rather
	// than screen-space salt-and-pepper stippling over the terrain.
	float broad = textureLod(cloud_noise, base_uv, 5.0).r;
	float medium = textureLod(cloud_noise, base_uv * 2.11 + vec2(0.19, 0.43), 4.0).r;
	float fine = textureLod(cloud_noise, base_uv * 4.73 + vec2(0.61, 0.17), 3.0).r;
	return broad * 0.72 + medium * 0.21 + fine * 0.07;
}

void vertex() {
	POSITION = vec4(VERTEX.xy, 1.0, 1.0);
}

void fragment() {
	float depth_raw = texture(DEPTH_TEXTURE, SCREEN_UV).r;
	#ifndef CURRENT_RENDERER
		depth_raw = PROJECTION_MATRIX[3][2] > PROJECTION_MATRIX[2][2]
			? depth_raw : depth_raw * 2.0 - 1.0;
	#else
		#if CURRENT_RENDERER == RENDERER_COMPATIBILITY
			depth_raw = depth_raw * 2.0 - 1.0;
		#endif
	#endif
	vec3 ndc = vec3(SCREEN_UV * 2.0 - 1.0, depth_raw);
	vec4 view_h = INV_PROJECTION_MATRIX * vec4(ndc, 1.0);
	vec3 view_position = view_h.xyz / view_h.w;
	vec4 scene_h = INV_VIEW_MATRIX * vec4(view_position, 1.0);
	vec3 scene_position = scene_h.xyz / scene_h.w;
	vec3 ray_vector = scene_position - CAMERA_POSITION_WORLD;
	float scene_distance = length(ray_vector);
	vec3 ray_direction = ray_vector / max(scene_distance, 0.001);

	float camera_altitude = terrain_camera_altitude_meters;
	float cloud_top = max(cloud_top_meters, cloud_base_meters + 100.0);
	vec3 shell_origin = vec3(0.0, PLANET_RADIUS_METERS + camera_altitude, 0.0);
	float base_hit = first_positive_sphere_hit(
		shell_origin, ray_direction, PLANET_RADIUS_METERS + cloud_base_meters);
	float top_hit = first_positive_sphere_hit(
		shell_origin, ray_direction, PLANET_RADIUS_METERS + cloud_top);
	float start_distance;
	float end_distance;
	if (camera_altitude < cloud_base_meters || camera_altitude > cloud_top) {
		if (base_hit < 0.0 || top_hit < 0.0) discard;
		start_distance = min(base_hit, top_hit);
		end_distance = max(base_hit, top_hit);
	} else {
		start_distance = 0.0;
		if (base_hit < 0.0 && top_hit < 0.0) discard;
		end_distance = base_hit < 0.0 ? top_hit
			: top_hit < 0.0 ? base_hit : min(base_hit, top_hit);
	}
	end_distance = min(end_distance, scene_distance);
	if (end_distance <= start_distance) discard;

	float step_length = (end_distance - start_distance) / float(CLOUD_STEPS);
	float transmittance = 1.0;
	vec3 accumulated_color = vec3(0.0);
	float sun_height = clamp(sun_direction.y * 0.5 + 0.5, 0.08, 1.0);
	float coverage_threshold = mix(0.72, 0.34, cloud_coverage);
	for (int step_index = 0; step_index < CLOUD_STEPS; ++step_index) {
		float distance_along_ray = start_distance
			+ (float(step_index) + 0.5) * step_length;
		vec3 shell_position = shell_origin + ray_direction * distance_along_ray;
		float altitude = length(shell_position) - PLANET_RADIUS_METERS;
		float normalized_height = clamp(
			(altitude - cloud_base_meters)
			/ max(cloud_top - cloud_base_meters, 1.0), 0.0, 1.0);
		float vertical_shape = smoothstep(0.0, 0.16, normalized_height)
			* (1.0 - smoothstep(0.72, 1.0, normalized_height));
		vec3 absolute_position = vec3(
			terrain_camera_absolute_xz.x + ray_direction.x * distance_along_ray,
			altitude,
			terrain_camera_absolute_xz.y + ray_direction.z * distance_along_ray);
		float noise_value = cloud_noise_field(absolute_position, normalized_height);
		float density = smoothstep(coverage_threshold, coverage_threshold + 0.16, noise_value)
			* vertical_shape;
		float opacity = 1.0 - exp(
			-density * step_length * 0.00032 * max(cloud_absorption, 0.05));
		vec3 lit_color = mix(vec3(0.50, 0.54, 0.58), vec3(1.0),
			clamp(normalized_height * 0.72 + sun_height * 0.45, 0.0, 1.0));
		lit_color = mix(night_color.rgb, lit_color, sun_height);
		accumulated_color += transmittance * opacity * lit_color;
		transmittance *= 1.0 - opacity;
		if (transmittance < 0.02) break;
	}
	float alpha = (1.0 - transmittance) * clamp(cloud_intensity, 0.0, 2.0);
	if (alpha < 0.002) discard;
	ALBEDO = accumulated_color / max(1.0 - transmittance, 0.001);
	ALPHA = clamp(alpha, 0.0, 1.0);
}
