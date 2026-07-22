shader_type spatial;
render_mode cull_disabled, depth_draw_opaque, diffuse_burley, specular_schlick_ggx;

void vertex() {
	float scale = INSTANCE_CUSTOM.z;
	float camera_distance = length((MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz);
	// Sub-pixel blades alias into terrain-following contour lines. Collapse the
	// geometry before that point; the terrain material carries the far grass
	// colour and avoids paying fragment/depth cost for invisible vegetation.
	scale *= 1.0 - smoothstep(58.0, 105.0, camera_distance);
	float wind_weight = smoothstep(0.08, 0.72, VERTEX.y);
	float sway = sin(INSTANCE_CUSTOM.x + TIME * 1.15);
	VERTEX.xz *= scale;
	VERTEX.y *= scale;
	VERTEX.x += sway * wind_weight * 0.075;
}

void fragment() {
	ALBEDO = COLOR.rgb;
	ROUGHNESS = 0.94;
	SPECULAR = 0.08;
	AO = 0.96;
}
