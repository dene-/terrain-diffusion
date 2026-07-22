#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform image2D scene_color_image;
layout(set = 0, binding = 1) uniform sampler2D cloud_texture;
layout(set = 0, binding = 2) uniform sampler2D low_depth_texture;
layout(set = 0, binding = 3) uniform sampler2D scene_depth_texture;

layout(std140, set = 0, binding = 4) uniform FrameData {
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
	ivec2 full_size = imageSize(scene_color_image);
	if (any(greaterThanEqual(pixel, full_size))) {
		return;
	}

	ivec2 low_size = textureSize(cloud_texture, 0);
	vec2 uv = (vec2(pixel) + 0.5) / vec2(full_size);
	float full_depth = reconstruct_scene_distance(
		uv,
		textureLod(scene_depth_texture, uv, 0.0).r);
	vec2 low_position = uv * vec2(low_size) - 0.5;
	ivec2 low_base = ivec2(floor(low_position));
	vec2 fraction = fract(low_position);

	vec4 cloud_sum = vec4(0.0);
	float weight_sum = 0.0;
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			ivec2 coordinate = clamp(low_base + ivec2(x, y), ivec2(0), low_size - 1);
			float bilinear_weight = mix(1.0 - fraction.x, fraction.x, float(x))
				* mix(1.0 - fraction.y, fraction.y, float(y));
			float low_depth = texelFetch(low_depth_texture, coordinate, 0).r;
			float relative_depth_delta = abs(low_depth - full_depth)
				/ max(min(low_depth, full_depth), 50.0);
			float depth_weight = exp(-relative_depth_delta * 8.0);
			float weight = bilinear_weight * max(depth_weight, 0.0001);
			cloud_sum += texelFetch(cloud_texture, coordinate, 0) * weight;
			weight_sum += weight;
		}
	}

	vec4 cloud = cloud_sum / max(weight_sum, 1.0e-5);
	cloud.a = clamp(cloud.a, 0.0, 1.0);
	vec4 scene = imageLoad(scene_color_image, pixel);
	vec3 composited = cloud.rgb + scene.rgb * (1.0 - cloud.a);
	imageStore(scene_color_image, pixel, vec4(composited, scene.a));
}
