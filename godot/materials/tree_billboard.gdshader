shader_type spatial;
render_mode cull_disabled, depth_draw_opaque, diffuse_burley, specular_schlick_ggx;

uniform sampler2D tree_atlas : source_color, filter_linear_mipmap_anisotropic, repeat_disable;
uniform float size_multiplier = 1.0;
uniform vec3 render_camera_position = vec3(0.0);
uniform bool debug_lod = false;
instance uniform float camera_facing = 0.0;

varying float camera_distance;

vec2 tree_dimensions(float species) {
	if (species < 0.5) return vec2(8.8, 22.0);
	if (species < 1.5) return vec2(8.2, 18.5);
	if (species < 2.5) return vec2(13.5, 18.0);
	if (species < 3.5) return vec2(15.5, 13.0);
	return vec2(12.5, 19.5);
}

void vertex() {
	float species = clamp(floor(INSTANCE_CUSTOM.x + 0.5), 0.0, 4.0);
	vec2 dimensions = tree_dimensions(species) * INSTANCE_CUSTOM.z * size_multiplier;
	vec3 instance_position = MODEL_MATRIX[3].xyz;
	mat4 billboard_model_matrix = MODEL_MATRIX;
	camera_distance = distance(instance_position, render_camera_position);
	bool use_camera_facing = camera_facing > 0.5;
	if (use_camera_facing) {
		vec3 camera_right = normalize(vec3(INV_VIEW_MATRIX[0].x, 0.0, INV_VIEW_MATRIX[0].z));
		vec3 camera_forward = normalize(cross(vec3(0.0, 1.0, 0.0), camera_right));
		MODELVIEW_MATRIX = VIEW_MATRIX * mat4(
			vec4(camera_right, 0.0),
			vec4(0.0, 1.0, 0.0, 0.0),
			vec4(camera_forward, 0.0),
			billboard_model_matrix[3]);
	} else {
		MODELVIEW_MATRIX = VIEW_MATRIX * billboard_model_matrix;
	}
	float crown_weight = smoothstep(0.10, 1.0, VERTEX.y);
	float sway = sin(INSTANCE_CUSTOM.y * 6.28318 + TIME * 0.42);
	VERTEX.xz *= dimensions.x;
	VERTEX.y *= dimensions.y;
	VERTEX.x += sway * crown_weight * 0.08;
	UV.x = (species + UV.x) * 0.2;
}

void fragment() {
	vec4 tree = texture(tree_atlas, UV);
	vec3 foliage = tree.rgb * COLOR.rgb;
	ALBEDO = debug_lod ? vec3(0.18, 0.95, 0.32) : foliage * 1.08;
	// Alpha-tested cards otherwise lose nearly all internal colour when their
	// broad geometric normal faces away from the sun. This small indirect term
	// stands in for light scattered through a real canopy.
	EMISSION = debug_lod ? ALBEDO * 0.28 : foliage * 0.13;
	ALPHA = tree.a;
	ALPHA_SCISSOR_THRESHOLD = mix(0.42, 0.31, smoothstep(450.0, 2400.0, camera_distance));
	ROUGHNESS = 0.88;
	SPECULAR = 0.10;
	AO = 0.90;
	AO_LIGHT_AFFECT = 0.16;
}
