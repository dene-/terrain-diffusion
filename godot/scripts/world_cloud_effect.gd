@tool
class_name WorldCloudEffect
extends CompositorEffect

## Rendering-thread implementation of the world cloud compositor. Public
## mutation is restricted to immutable snapshots protected by a mutex.

const RAYMARCH_SHADER_FILE: RDShaderFile = preload(
	"res://materials/world_cloud_raymarch.glsl")
const COMPOSITE_SHADER_FILE: RDShaderFile = preload(
	"res://materials/world_cloud_composite.glsl")
const WEATHER_TEXTURE: Texture2D = preload(
	"res://config/world_cloud_weather_noise.tres")
const SHAPE_NOISE_TEXTURE: Texture3D = preload(
	"res://config/world_cloud_shape_noise.tres")
const DETAIL_NOISE_TEXTURE: Texture3D = preload(
	"res://config/world_cloud_detail_noise.tres")

const BUFFER_CONTEXT := &"world_clouds"
const CLOUD_TEXTURE_NAME := &"cloud_radiance"
const DEPTH_TEXTURE_NAME := &"cloud_scene_depth"
const FRAME_DATA_FLOAT_COUNT := 68
const FRAME_DATA_BYTE_COUNT := FRAME_DATA_FLOAT_COUNT * 4

var _rendering_device: RenderingDevice
var _raymarch_shader := RID()
var _raymarch_pipeline := RID()
var _composite_shader := RID()
var _composite_pipeline := RID()
var _nearest_clamp_sampler := RID()
var _linear_repeat_sampler := RID()
var _view_uniform_buffers: Array[RID] = []

var _state_mutex := Mutex.new()
var _settings := {
	"base_altitude_meters": 1800.0,
	"top_altitude_meters": 6200.0,
	"maximum_distance_meters": 180000.0,
	"distance_fade_start_meters": 120000.0,
	"coverage": 0.56,
	"density": 1.05,
	"weather_scale_meters": 320000.0,
	"shape_scale_meters": 9000.0,
	"detail_scale_meters": 1050.0,
	"detail_erosion": 0.38,
	"extinction_per_meter": 0.00115,
	"ambient_strength": 0.34,
	"forward_scattering": 0.72,
	"backward_scattering": -0.28,
	"backward_scattering_weight": 0.18,
	"resolution_divisor": 4,
	"maximum_ray_steps": 64,
	"light_steps": 3,
	"ray_jitter": 1.0,
}
var _floating_origin := Vector2.ZERO
var _weather_offset_meters := Vector2.ZERO
var _runtime_coverage := 0.56
var _sea_level_meters := -0.08
var _sun_direction := Vector3(0.25, 0.93, 0.25).normalized()
var _sun_color := Vector3.ONE
var _sun_energy := 1.0
var _frame_index := 0
var _reported_initialization_failure := false


func _init() -> void:
	effect_callback_type = CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	access_resolved_color = true
	access_resolved_depth = true
	RenderingServer.call_on_render_thread(_initialize_render_resources)


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE or _rendering_device == null:
		return
	for uniform_buffer in _view_uniform_buffers:
		if uniform_buffer.is_valid():
			_rendering_device.free_rid(uniform_buffer)
	_view_uniform_buffers.clear()
	var owned_resources: Array[RID] = [
		_nearest_clamp_sampler,
		_linear_repeat_sampler,
		_raymarch_pipeline,
		_composite_pipeline,
		_raymarch_shader,
		_composite_shader,
	]
	for resource in owned_resources:
		if resource.is_valid():
			_rendering_device.free_rid(resource)


func set_profile_settings(settings: Dictionary) -> void:
	_state_mutex.lock()
	_settings = settings.duplicate()
	_state_mutex.unlock()


func set_runtime_state(
	floating_origin: Vector2,
	weather_offset_meters: Vector2,
	coverage: float,
	sea_level_meters: float,
	sun_direction: Vector3,
	sun_color: Vector3,
	sun_energy: float,
) -> void:
	_state_mutex.lock()
	_floating_origin = floating_origin
	_weather_offset_meters = weather_offset_meters
	_runtime_coverage = clampf(coverage, 0.0, 1.0)
	_sea_level_meters = sea_level_meters
	_sun_direction = sun_direction.normalized()
	_sun_color = sun_color
	_sun_energy = maxf(sun_energy, 0.0)
	_state_mutex.unlock()


func _initialize_render_resources() -> void:
	_rendering_device = RenderingServer.get_rendering_device()
	if _rendering_device == null:
		# Headless imports and compatibility rendering intentionally have no RD.
		return

	_raymarch_shader = _rendering_device.shader_create_from_spirv(
		RAYMARCH_SHADER_FILE.get_spirv())
	_composite_shader = _rendering_device.shader_create_from_spirv(
		COMPOSITE_SHADER_FILE.get_spirv())
	if not (
		_raymarch_shader.is_valid()
		and _composite_shader.is_valid()
	):
		_report_initialization_failure("cloud compute shader compilation failed")
		_free_render_resources()
		return

	_raymarch_pipeline = _rendering_device.compute_pipeline_create(_raymarch_shader)
	_composite_pipeline = _rendering_device.compute_pipeline_create(_composite_shader)
	if not (
		_raymarch_pipeline.is_valid()
		and _composite_pipeline.is_valid()
	):
		_report_initialization_failure("cloud compute pipeline creation failed")
		_free_render_resources()
		return

	var nearest_state := RDSamplerState.new()
	nearest_state.min_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	nearest_state.mag_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	nearest_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	nearest_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	nearest_state.repeat_w = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_nearest_clamp_sampler = _rendering_device.sampler_create(nearest_state)

	var linear_repeat_state := RDSamplerState.new()
	linear_repeat_state.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	linear_repeat_state.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	linear_repeat_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_REPEAT
	linear_repeat_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_REPEAT
	linear_repeat_state.repeat_w = RenderingDevice.SAMPLER_REPEAT_MODE_REPEAT
	_linear_repeat_sampler = _rendering_device.sampler_create(linear_repeat_state)


func _free_render_resources() -> void:
	if _rendering_device == null:
		return
	for uniform_buffer in _view_uniform_buffers:
		_free_rid(uniform_buffer)
	_view_uniform_buffers.clear()
	_free_rid(_nearest_clamp_sampler)
	_free_rid(_linear_repeat_sampler)
	_free_rid(_raymarch_pipeline)
	_free_rid(_composite_pipeline)
	_free_rid(_raymarch_shader)
	_free_rid(_composite_shader)
	_nearest_clamp_sampler = RID()
	_linear_repeat_sampler = RID()
	_raymarch_pipeline = RID()
	_composite_pipeline = RID()
	_raymarch_shader = RID()
	_composite_shader = RID()


func _free_rid(resource: RID) -> void:
	if resource.is_valid():
		_rendering_device.free_rid(resource)


func _report_initialization_failure(reason: String) -> void:
	if _reported_initialization_failure:
		return
	_reported_initialization_failure = true
	push_error("[WorldCloudEffect] %s; the cloud compositor was disabled." % reason)
	enabled = false


func _render_callback(_callback_type: int, render_data: RenderData) -> void:
	if not _render_resources_are_ready():
		return
	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	var scene_data := render_data.get_render_scene_data() as RenderSceneDataRD
	if buffers == null or scene_data == null:
		return

	var full_size := buffers.get_internal_size()
	var view_count := buffers.get_view_count()
	if full_size.x <= 0 or full_size.y <= 0 or view_count <= 0:
		return

	var state := _copy_state()
	var settings: Dictionary = state["settings"]
	var resolution_divisor := int(settings["resolution_divisor"])
	var low_size := Vector2i(
		ceili(float(full_size.x) / float(resolution_divisor)),
		ceili(float(full_size.y) / float(resolution_divisor)))
	_ensure_intermediate_textures(buffers, low_size, view_count)
	_ensure_uniform_buffers(view_count)
	if _view_uniform_buffers.size() < view_count:
		return

	var weather_rid := RenderingServer.texture_get_rd_texture(WEATHER_TEXTURE.get_rid())
	var shape_rid := RenderingServer.texture_get_rd_texture(SHAPE_NOISE_TEXTURE.get_rid())
	var detail_rid := RenderingServer.texture_get_rd_texture(DETAIL_NOISE_TEXTURE.get_rid())
	if not weather_rid.is_valid() or not shape_rid.is_valid() or not detail_rid.is_valid():
		return

	_frame_index = (_frame_index + 1) % 1000000
	for view in view_count:
		_render_view(
			buffers,
			scene_data,
			view,
			full_size,
			low_size,
			state,
			weather_rid,
			shape_rid,
			detail_rid)


func _render_resources_are_ready() -> bool:
	return (
		_rendering_device != null
		and _raymarch_pipeline.is_valid()
		and _composite_pipeline.is_valid()
		and _nearest_clamp_sampler.is_valid()
		and _linear_repeat_sampler.is_valid())


func _copy_state() -> Dictionary:
	_state_mutex.lock()
	var state := {
		"settings": _settings.duplicate(),
		"floating_origin": _floating_origin,
		"weather_offset_meters": _weather_offset_meters,
		"runtime_coverage": _runtime_coverage,
		"sea_level_meters": _sea_level_meters,
		"sun_direction": _sun_direction,
		"sun_color": _sun_color,
		"sun_energy": _sun_energy,
	}
	_state_mutex.unlock()
	return state


func _ensure_intermediate_textures(
	buffers: RenderSceneBuffersRD,
	low_size: Vector2i,
	view_count: int,
) -> void:
	var recreate := not (
		buffers.has_texture(BUFFER_CONTEXT, CLOUD_TEXTURE_NAME)
		and buffers.has_texture(BUFFER_CONTEXT, DEPTH_TEXTURE_NAME))
	if not recreate:
		var cloud_format := buffers.get_texture_format(BUFFER_CONTEXT, CLOUD_TEXTURE_NAME)
		recreate = (
			buffers.get_texture_slice_size(BUFFER_CONTEXT, CLOUD_TEXTURE_NAME, 0) != low_size
			or cloud_format.array_layers != view_count)
	if not recreate:
		return

	buffers.clear_context(BUFFER_CONTEXT)
	var usage := (
		RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
		| RenderingDevice.TEXTURE_USAGE_STORAGE_BIT)
	buffers.create_texture(
		BUFFER_CONTEXT,
		CLOUD_TEXTURE_NAME,
		RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT,
		usage,
		RenderingDevice.TEXTURE_SAMPLES_1,
		low_size,
		view_count,
		1,
		false,
		false)
	buffers.create_texture(
		BUFFER_CONTEXT,
		DEPTH_TEXTURE_NAME,
		RenderingDevice.DATA_FORMAT_R32_SFLOAT,
		usage,
		RenderingDevice.TEXTURE_SAMPLES_1,
		low_size,
		view_count,
		1,
		false,
		false)


func _ensure_uniform_buffers(view_count: int) -> void:
	while _view_uniform_buffers.size() < view_count:
		var uniform_buffer := _rendering_device.uniform_buffer_create(FRAME_DATA_BYTE_COUNT)
		if not uniform_buffer.is_valid():
			return
		_view_uniform_buffers.append(uniform_buffer)
	while _view_uniform_buffers.size() > view_count:
		var unused_buffer: RID = _view_uniform_buffers.pop_back()
		_free_rid(unused_buffer)


func _render_view(
	buffers: RenderSceneBuffersRD,
	scene_data: RenderSceneDataRD,
	view: int,
	full_size: Vector2i,
	low_size: Vector2i,
	state: Dictionary,
	weather_rid: RID,
	shape_rid: RID,
	detail_rid: RID,
) -> void:
	var color_rid := buffers.get_color_layer(view, false)
	var scene_depth_rid := buffers.get_depth_layer(view, false)
	var cloud_rid := buffers.get_texture_slice(
		BUFFER_CONTEXT, CLOUD_TEXTURE_NAME, view, 0, 1, 1)
	var low_depth_rid := buffers.get_texture_slice(
		BUFFER_CONTEXT, DEPTH_TEXTURE_NAME, view, 0, 1, 1)
	if not (
		color_rid.is_valid()
		and scene_depth_rid.is_valid()
		and cloud_rid.is_valid()
		and low_depth_rid.is_valid()):
		return

	var frame_data := _build_frame_data(scene_data, view, full_size, low_size, state)
	var frame_bytes := frame_data.to_byte_array()
	if frame_bytes.size() != FRAME_DATA_BYTE_COUNT:
		return
	var uniform_buffer := _view_uniform_buffers[view]
	_rendering_device.buffer_update(
		uniform_buffer, 0, FRAME_DATA_BYTE_COUNT, frame_bytes)

	var raymarch_uniforms: Array[RDUniform] = [
		_make_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 0, [cloud_rid]),
		_make_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 1, [low_depth_rid]),
		_make_uniform(
			RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE,
			2,
			[_nearest_clamp_sampler, scene_depth_rid]),
		_make_uniform(
			RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE,
			3,
			[_linear_repeat_sampler, weather_rid]),
		_make_uniform(
			RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE,
			4,
			[_linear_repeat_sampler, shape_rid]),
		_make_uniform(
			RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE,
			5,
			[_linear_repeat_sampler, detail_rid]),
		_make_uniform(RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER, 6, [uniform_buffer]),
	]
	var raymarch_set := UniformSetCacheRD.get_cache(
		_raymarch_shader, 0, raymarch_uniforms)
	var raymarch_list := _rendering_device.compute_list_begin()
	_rendering_device.compute_list_bind_compute_pipeline(raymarch_list, _raymarch_pipeline)
	_rendering_device.compute_list_bind_uniform_set(raymarch_list, raymarch_set, 0)
	_rendering_device.compute_list_dispatch(
		raymarch_list,
		ceili(float(low_size.x) / 8.0),
		ceili(float(low_size.y) / 8.0),
		1)
	_rendering_device.compute_list_end()

	var composite_uniforms: Array[RDUniform] = [
		_make_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 0, [color_rid]),
		_make_uniform(
			RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE,
			1,
			[_nearest_clamp_sampler, cloud_rid]),
		_make_uniform(
			RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE,
			2,
			[_nearest_clamp_sampler, low_depth_rid]),
		_make_uniform(
			RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE,
			3,
			[_nearest_clamp_sampler, scene_depth_rid]),
		_make_uniform(RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER, 4, [uniform_buffer]),
	]
	var composite_set := UniformSetCacheRD.get_cache(
		_composite_shader, 0, composite_uniforms)
	var composite_list := _rendering_device.compute_list_begin()
	_rendering_device.compute_list_bind_compute_pipeline(composite_list, _composite_pipeline)
	_rendering_device.compute_list_bind_uniform_set(composite_list, composite_set, 0)
	_rendering_device.compute_list_dispatch(
		composite_list,
		ceili(float(full_size.x) / 8.0),
		ceili(float(full_size.y) / 8.0),
		1)
	_rendering_device.compute_list_end()


func _make_uniform(
	type: RenderingDevice.UniformType,
	binding: int,
	resources: Array,
) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = type
	uniform.binding = binding
	for resource in resources:
		uniform.add_id(resource as RID)
	return uniform


func _build_frame_data(
	scene_data: RenderSceneDataRD,
	view: int,
	full_size: Vector2i,
	low_size: Vector2i,
	state: Dictionary,
) -> PackedFloat32Array:
	var settings: Dictionary = state["settings"]
	var projection := scene_data.get_view_projection(view)
	var inverse_projection := projection.inverse()
	var camera_transform := scene_data.get_cam_transform()
	var eye_offset := scene_data.get_view_eye_offset(view)
	camera_transform.origin += camera_transform.basis * eye_offset
	var floating_origin: Vector2 = state["floating_origin"]
	camera_transform.origin.x += floating_origin.x
	camera_transform.origin.z += floating_origin.y

	var data := PackedFloat32Array()
	data.append_array(_projection_to_float_array(inverse_projection))
	data.append_array(_transform_to_float_array(camera_transform))
	data.append_array(PackedFloat32Array([
		float(full_size.x), float(full_size.y), float(low_size.x), float(low_size.y)]))
	data.append_array(PackedFloat32Array([
		float(settings["base_altitude_meters"]),
		float(settings["top_altitude_meters"]),
		float(state["sea_level_meters"]),
		float(settings["maximum_distance_meters"]),
	]))
	data.append_array(PackedFloat32Array([
		float(settings["distance_fade_start_meters"]),
		float(settings["maximum_distance_meters"]),
		float(settings["weather_scale_meters"]),
		float(state["runtime_coverage"]),
	]))
	data.append_array(PackedFloat32Array([
		float(settings["density"]),
		float(settings["shape_scale_meters"]),
		float(settings["detail_scale_meters"]),
		float(settings["detail_erosion"]),
	]))
	var sun_direction: Vector3 = state["sun_direction"]
	data.append_array(PackedFloat32Array([
		sun_direction.x,
		sun_direction.y,
		sun_direction.z,
		float(state["sun_energy"]),
	]))
	var sun_color: Vector3 = state["sun_color"]
	data.append_array(PackedFloat32Array([
		sun_color.x,
		sun_color.y,
		sun_color.z,
		float(settings["ambient_strength"]),
	]))
	var weather_offset: Vector2 = state["weather_offset_meters"]
	data.append_array(PackedFloat32Array([
		weather_offset.x,
		weather_offset.y,
		float(settings["extinction_per_meter"]),
		float(_frame_index),
	]))
	data.append_array(PackedFloat32Array([
		float(settings["maximum_ray_steps"]),
		float(settings["light_steps"]),
		float(settings["ray_jitter"]),
		1.0 if projection.is_orthogonal() else 0.0,
	]))
	data.append_array(PackedFloat32Array([
		float(settings["forward_scattering"]),
		float(settings["backward_scattering"]),
		float(settings["backward_scattering_weight"]),
		0.0,
	]))
	return data


func _projection_to_float_array(projection: Projection) -> PackedFloat32Array:
	return PackedFloat32Array([
		projection.x.x, projection.x.y, projection.x.z, projection.x.w,
		projection.y.x, projection.y.y, projection.y.z, projection.y.w,
		projection.z.x, projection.z.y, projection.z.z, projection.z.w,
		projection.w.x, projection.w.y, projection.w.z, projection.w.w,
	])


func _transform_to_float_array(transform: Transform3D) -> PackedFloat32Array:
	return PackedFloat32Array([
		transform.basis.x.x, transform.basis.x.y, transform.basis.x.z, 0.0,
		transform.basis.y.x, transform.basis.y.y, transform.basis.y.z, 0.0,
		transform.basis.z.x, transform.basis.z.y, transform.basis.z.z, 0.0,
		transform.origin.x, transform.origin.y, transform.origin.z, 1.0,
	])
