@tool
extends Node3D

const TERRAIN_MATERIAL_PATH := "res://materials/terrain.gdshader"
const VISUAL_SETTINGS_PATH := "res://config/world_visuals.tres"
const EDITOR_PREVIEW_NODE_NAME := &"EditorMapPreview"
const EDITOR_OVERVIEW_WATER_NODE_NAME := &"EditorOverviewWater"
const EDITOR_STREAMING_NODE_NAME := &"EditorStreamingWorld"
const EDITOR_OCEAN_SUBDIVISIONS := 128

@export_category("Terrain Map")
@export var terrain_map: TerrainMapDefinition:
	set(value):
		if terrain_map == value:
			return
		_disconnect_map_changed()
		terrain_map = value
		_connect_map_changed()
		_queue_editor_preview_refresh()
@export var show_editor_preview := true:
	set(value):
		show_editor_preview = value
		_queue_editor_preview_refresh()
@export var auto_frame_editor_preview := true
@export_range(0.001, 0.1, 0.001) var editor_preview_scale := 0.005:
	set(value):
		editor_preview_scale = value
		_queue_editor_preview_refresh()
@export_tool_button("Frame Map Preview", "Camera3D") var frame_map_preview_action := _frame_editor_preview
@export_category("Local 1:1 Editor World")
@export var stream_local_map_in_editor := false:
	set(value):
		stream_local_map_in_editor = value
		_queue_editor_preview_refresh()
@export var auto_focus_local_map := true
@export_tool_button("Focus Local 1:1 Map", "Camera3D") var focus_local_map_action := _focus_local_map
@export_tool_button("Reload Local Map", "Reload") var reload_local_map_action := _reload_local_map

var _activation_attempts := 0
var _editor_archive_poll_seconds := 0.0
var _editor_local_focus_applied := false
var _editor_extension_api_unavailable := false
var _editor_extension_warning_shown := false


func _ready() -> void:
	if Engine.is_editor_hint():
		_connect_map_changed()
		set_process(true)
		_set_scene_ocean_visible(false)
		call_deferred("_refresh_editor_preview")
		return
	call_deferred("_activate_native_controller")


func _notification(what: int) -> void:
	if not Engine.is_editor_hint():
		return
	if what == NOTIFICATION_EDITOR_PRE_SAVE:
		# Transient preview nodes have no scene owner and therefore are not
		# serialized. Do not remove/free them while Godot is traversing this
		# scene tree for a save: launching the game performs an editor save, and
		# synchronous native-controller destruction here corrupts that traversal.
		_set_scene_ocean_visible(true)
		var preview := get_node_or_null(NodePath(String(EDITOR_PREVIEW_NODE_NAME))) as MeshInstance3D
		if preview != null:
			preview.mesh = null
			preview.visible = false
	elif what == NOTIFICATION_EDITOR_POST_SAVE:
		_set_scene_ocean_visible(false)
		call_deferred("_refresh_editor_preview")


func _process(delta: float) -> void:
	if not Engine.is_editor_hint() or not stream_local_map_in_editor:
		return
	var controller := _get_editor_streaming_controller()
	if controller == null:
		_editor_archive_poll_seconds += delta
		if _editor_archive_poll_seconds >= 1.0:
			_editor_archive_poll_seconds = 0.0
			if _ensure_editor_streaming_controller():
				call_deferred("_refresh_editor_preview")
		return
	if not _update_editor_streaming_observer(controller):
		_destroy_editor_streaming_controller(true)


func _activate_native_controller() -> void:
	_set_scene_ocean_visible(true)
	_activation_attempts += 1
	if not ClassDB.class_exists("TerrainWorld3D"):
		if _activation_attempts < 120:
			call_deferred("_activate_native_controller")
			return
		push_error("TerrainWorld3D GDExtension class is unavailable; rebuild the extension and restart Godot")
		return

	var player := get_node_or_null("../../Player")
	if player == null or not player.is_class("PlayerController"):
		if _activation_attempts < 120:
			call_deferred("_activate_native_controller")
			return
		push_error("PlayerController did not activate before TerrainWorld3D")
		return

	var controller := ClassDB.instantiate("TerrainWorld3D") as Node3D
	if controller == null:
		push_error("Could not instantiate TerrainWorld3D")
		return
	if not _configure_native_controller(controller):
		controller.free()
		return
	var selected_map_path := _selected_map_path()
	if not selected_map_path.is_empty():
		controller.set("map_path", selected_map_path)
	controller.name = name
	controller.transform = transform
	var editor_preview := get_node_or_null(NodePath(String(EDITOR_PREVIEW_NODE_NAME)))
	if editor_preview != null:
		remove_child(editor_preview)
		editor_preview.queue_free()

	for child: Node in get_children():
		remove_child(child)
		controller.add_child(child)

	var parent_node := get_parent()
	name = &"TerrainWorldBootstrapRetired"
	parent_node.add_child(controller)

	var random_button := get_node_or_null("../../HUD/RandomTerrain") as Button
	if random_button != null:
		if selected_map_path.is_empty():
			random_button.pressed.connect(controller.request_random_world)
		else:
			random_button.text = "FIXED MAP"
			random_button.disabled = true

	queue_free()


func _configure_native_controller(controller: Node3D, editor_preview: bool = false) -> bool:
	var shader := load(TERRAIN_MATERIAL_PATH) as Shader
	var settings := load(VISUAL_SETTINGS_PATH) as Resource
	if shader == null or settings == null:
		push_error("Terrain material or visual settings could not be loaded")
		return false
	if editor_preview:
		# The game can absorb several uploads per frame on an otherwise empty
		# viewport. The editor is already rendering its own UI, gizmos and
		# previews, so use a private conservative copy instead of mutating the
		# runtime settings resource shared by the scene.
		settings = settings.duplicate(true) as Resource
		settings.set("max_concurrent_requests", 1)
		settings.set("max_gpu_uploads_per_frame", 1)
		settings.set("max_gpu_upload_megabytes_per_frame", 4)
		settings.set("terrain_memory_megabytes", 256)
		settings.set("vegetation_memory_megabytes", 32)
		settings.set("max_resident_tiles", 160)
	var material := ShaderMaterial.new()
	material.shader = shader
	material.set_shader_parameter("detail_texture", _create_terrain_detail_texture())
	controller.set("terrain_material", material)
	controller.set("visual_settings", settings)
	return true


func _create_terrain_detail_texture() -> NoiseTexture2D:
	var noise := FastNoiseLite.new()
	noise.seed = 20260731
	noise.noise_type = FastNoiseLite.TYPE_SIMPLEX_SMOOTH
	noise.frequency = 0.035
	noise.fractal_type = FastNoiseLite.FRACTAL_FBM
	noise.fractal_octaves = 5
	noise.fractal_lacunarity = 2.0
	noise.fractal_gain = 0.52
	var texture := NoiseTexture2D.new()
	texture.width = 512
	texture.height = 512
	texture.seamless = true
	texture.normalize = true
	texture.noise = noise
	return texture


func _selected_map_path() -> String:
	const command_line_prefix := "--terrain-map="
	for argument: String in OS.get_cmdline_user_args():
		if argument.begins_with(command_line_prefix):
			var override_path := argument.trim_prefix(command_line_prefix).strip_edges()
			if not override_path.is_empty():
				print("[TerrainWorldBootstrap] Command-line terrain map: %s" % override_path)
				return override_path
	if terrain_map == null or terrain_map.archive_path.is_empty():
		return ""
	var global_path := _globalize_path(terrain_map.archive_path)
	if FileAccess.file_exists(global_path):
		print("[TerrainWorldBootstrap] Selected terrain map: %s" % terrain_map.map_id)
		return global_path
	push_warning(
		"Selected terrain map archive is not ready yet: %s; using the Terrain Diffusion server"
		% global_path
	)
	return ""


func _connect_map_changed() -> void:
	if terrain_map != null and not terrain_map.changed.is_connected(_queue_editor_preview_refresh):
		terrain_map.changed.connect(_queue_editor_preview_refresh)


func _disconnect_map_changed() -> void:
	if terrain_map != null and terrain_map.changed.is_connected(_queue_editor_preview_refresh):
		terrain_map.changed.disconnect(_queue_editor_preview_refresh)


func _queue_editor_preview_refresh() -> void:
	if Engine.is_editor_hint() and is_inside_tree():
		call_deferred("_refresh_editor_preview")


func _refresh_editor_preview() -> void:
	if not Engine.is_editor_hint() or not is_inside_tree():
		return
	_set_scene_ocean_visible(false)
	if stream_local_map_in_editor and _ensure_editor_streaming_controller():
		var local_preview := get_node_or_null(NodePath(String(EDITOR_PREVIEW_NODE_NAME))) as MeshInstance3D
		if local_preview != null:
			local_preview.mesh = null
			local_preview.visible = false
		_destroy_editor_overview_water(false)
		editor_description = (
			"Local editor map: %s at 1:1 metre scale. The editor camera drives the same "
			+ "wandering clipmap and LOD streamer as the game with editor-safe pacing. "
			+ "Vegetation remains runtime-only. Generated terrain is preview-only; scene "
			+ "objects keep stable map-centred coordinates."
		) % terrain_map.display_name
		return
	_destroy_editor_streaming_controller(false)
	var preview := get_node_or_null(NodePath(String(EDITOR_PREVIEW_NODE_NAME))) as MeshInstance3D
	if preview == null:
		preview = MeshInstance3D.new()
		preview.name = EDITOR_PREVIEW_NODE_NAME
		preview.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(preview)
	preview.mesh = null
	preview.visible = false
	if not show_editor_preview or terrain_map == null:
		return
	var heightmap_path := _globalize_path(terrain_map.editor_heightmap_path)
	if heightmap_path.is_empty() or not FileAccess.file_exists(heightmap_path):
		push_warning("Editor terrain preview heightmap is missing: %s" % heightmap_path)
		return
	var heightmap := Image.new()
	if heightmap.load(heightmap_path) != OK:
		push_warning("Could not load editor terrain preview heightmap: %s" % heightmap_path)
		return
	if heightmap.get_width() < 2 or heightmap.get_height() < 2:
		push_warning("Editor terrain preview heightmap is too small")
		return

	var mesh := _build_editor_preview_mesh(heightmap)
	var material := StandardMaterial3D.new()
	material.roughness = 1.0
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	var albedo_path := _globalize_path(terrain_map.editor_albedo_path)
	if not albedo_path.is_empty() and FileAccess.file_exists(albedo_path):
		var albedo_image := Image.new()
		if albedo_image.load(albedo_path) == OK:
			material.albedo_texture = ImageTexture.create_from_image(albedo_image)
	mesh.surface_set_material(0, material)

	preview.mesh = mesh
	preview.visible = true
	_build_editor_overview_water()
	editor_description = (
		(
			"Map overview: %s at 1:%.0f scale. Select EditorMapPreview and press F, "
			+ "or click Frame Map Preview. Enable Stream Local Map In Editor for the "
			+ "1:1 authoring view when the selected .tmap archive is available."
		)
		% [terrain_map.display_name, 1.0 / editor_preview_scale]
	)
	if auto_frame_editor_preview:
		call_deferred("_frame_editor_preview")


func _ensure_editor_streaming_controller() -> bool:
	if not Engine.is_editor_hint() or not stream_local_map_in_editor or terrain_map == null:
		return false
	var existing := _get_editor_streaming_controller()
	if existing != null:
		if _update_editor_streaming_observer(existing):
			return true
		_destroy_editor_streaming_controller(true)
		return false
	if _editor_extension_api_unavailable:
		return false
	var archive_path := _globalize_path(terrain_map.archive_path)
	if archive_path.is_empty() or not FileAccess.file_exists(archive_path):
		return false
	if not ClassDB.class_exists("TerrainWorld3D"):
		return false
	if auto_focus_local_map and not _editor_local_focus_applied:
		_focus_local_map()
	var controller := ClassDB.instantiate("TerrainWorld3D") as Node3D
	if controller == null:
		push_error("Could not instantiate the 1:1 editor terrain streamer")
		return false
	if not _editor_controller_has_required_api(controller):
		_editor_extension_api_unavailable = true
		_warn_stale_editor_extension()
		controller.free()
		return false
	controller.name = EDITOR_STREAMING_NODE_NAME
	controller.set("editor_streaming_mode", true)
	controller.set("streaming_enabled", false)
	controller.set("map_path", archive_path)
	if not _configure_native_controller(controller, true):
		controller.free()
		return false
	var terrain_tiles := Node3D.new()
	terrain_tiles.name = &"TerrainTiles"
	controller.add_child(terrain_tiles)
	var vegetation := Node3D.new()
	vegetation.name = &"Vegetation"
	controller.add_child(vegetation)
	var ocean_template := get_node_or_null("Ocean") as MeshInstance3D
	if ocean_template != null:
		var ocean := MeshInstance3D.new()
		ocean.name = &"Ocean"
		ocean.transform = ocean_template.transform
		var ocean_template_plane := ocean_template.mesh as PlaneMesh
		if ocean_template_plane != null:
			var editor_ocean_mesh := PlaneMesh.new()
			editor_ocean_mesh.size = ocean_template_plane.size
			editor_ocean_mesh.subdivide_width = EDITOR_OCEAN_SUBDIVISIONS
			editor_ocean_mesh.subdivide_depth = EDITOR_OCEAN_SUBDIVISIONS
			ocean.mesh = editor_ocean_mesh
		else:
			ocean.mesh = ocean_template.mesh
		ocean.cast_shadow = ocean_template.cast_shadow
		var water_material := ocean_template.get_active_material(0)
		if water_material != null:
			ocean.material_override = water_material.duplicate(true)
		controller.add_child(ocean)
	add_child(controller)
	_update_editor_streaming_observer(controller)
	controller.set("streaming_enabled", true)
	print("[TerrainWorldBootstrap] Local 1:1 editor streaming enabled: %s" % archive_path)
	return true


func _update_editor_streaming_observer(controller: Node3D) -> bool:
	if not _editor_controller_has_required_api(controller):
		_editor_extension_api_unavailable = true
		_warn_stale_editor_extension()
		return false
	var editor_viewport := EditorInterface.get_editor_viewport_3d(0)
	if editor_viewport == null:
		return true
	var editor_camera := editor_viewport.get_camera_3d()
	if editor_camera == null:
		return true
	var observer_position := to_local(editor_camera.global_position)
	var observer_direction := (global_basis.inverse() * -editor_camera.global_basis.z).normalized()
	controller.call(
		"set_editor_observer",
		observer_position,
		observer_direction,
		editor_camera.fov,
		editor_viewport.get_visible_rect().size.y
	)
	return true


func _get_editor_streaming_controller() -> Node3D:
	var candidate: Variant = get_node_or_null(NodePath(String(EDITOR_STREAMING_NODE_NAME)))
	if candidate == null or not is_instance_valid(candidate):
		return null
	if not candidate is Node3D:
		return null
	return candidate as Node3D


func _editor_controller_has_required_api(controller: Node3D) -> bool:
	return (
		is_instance_valid(controller)
		and controller.has_method("set_editor_streaming_mode")
		and controller.has_method("set_editor_observer")
	)


func _warn_stale_editor_extension() -> void:
	if _editor_extension_warning_shown:
		return
	_editor_extension_warning_shown = true
	push_warning(
		"The loaded TerrainWorld3D GDExtension predates local editor streaming. "
		+ "Restart the Godot editor to load the rebuilt extension; the 1:200 overview "
		+ "will remain available until then."
	)


func _destroy_editor_streaming_controller(_immediate: bool) -> void:
	var controller := _get_editor_streaming_controller()
	if controller == null:
		return
	remove_child(controller)
	# The controller owns native worker/thread state. Complete its destruction
	# before another deferred editor refresh can resolve the same node name.
	controller.free()


func _reload_local_map() -> void:
	if not Engine.is_editor_hint():
		return
	_destroy_editor_streaming_controller(true)
	_editor_extension_api_unavailable = false
	_editor_extension_warning_shown = false
	_editor_archive_poll_seconds = 1.0
	call_deferred("_refresh_editor_preview")


func _focus_local_map() -> void:
	if not Engine.is_editor_hint() or terrain_map == null:
		return
	var editor_viewport := EditorInterface.get_editor_viewport_3d(0)
	if editor_viewport == null:
		return
	var editor_camera := editor_viewport.get_camera_3d()
	if editor_camera == null:
		return
	var target := Vector3.ZERO
	var heightmap_path := _globalize_path(terrain_map.editor_heightmap_path)
	if not heightmap_path.is_empty() and FileAccess.file_exists(heightmap_path):
		var heightmap := Image.new()
		if heightmap.load(heightmap_path) == OK:
			var center := Vector2(
				float(heightmap.get_width() - 1) * 0.5,
				float(heightmap.get_height() - 1) * 0.5
			)
			var best_distance := INF
			for z in range(heightmap.get_height()):
				for x in range(heightmap.get_width()):
					var elevation := _decode_editor_elevation(heightmap, x, z)
					if elevation <= 10.0:
						continue
					var distance := Vector2(float(x), float(z)).distance_squared_to(center)
					if distance >= best_distance:
						continue
					best_distance = distance
					target = Vector3(
						-terrain_map.width_m * 0.5
							+ terrain_map.width_m * float(x) / float(heightmap.get_width() - 1),
						elevation,
						-terrain_map.height_m * 0.5
							+ terrain_map.height_m * float(z) / float(heightmap.get_height() - 1)
					)
	editor_camera.global_position = to_global(target + Vector3(0.0, 800.0, 1400.0))
	editor_camera.look_at(to_global(target + Vector3(0.0, 80.0, 0.0)), Vector3.UP)
	_editor_local_focus_applied = true


func _build_editor_overview_water() -> void:
	if terrain_map == null:
		return
	_destroy_editor_overview_water(true)
	var water := MeshInstance3D.new()
	water.name = EDITOR_OVERVIEW_WATER_NODE_NAME
	water.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	water.position.y = -0.02
	var plane := PlaneMesh.new()
	plane.size = Vector2(terrain_map.width_m, terrain_map.height_m) * editor_preview_scale * 1.08
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.025, 0.19, 0.31, 1.0)
	material.metallic = 0.08
	material.roughness = 0.26
	plane.material = material
	water.mesh = plane
	add_child(water)


func _destroy_editor_overview_water(immediate: bool) -> void:
	var water := get_node_or_null(NodePath(String(EDITOR_OVERVIEW_WATER_NODE_NAME))) as MeshInstance3D
	if water == null:
		return
	remove_child(water)
	if immediate:
		water.free()
	else:
		water.queue_free()


func _set_scene_ocean_visible(value: bool) -> void:
	var ocean := get_node_or_null("Ocean") as MeshInstance3D
	if ocean != null:
		ocean.visible = value


func _frame_editor_preview() -> void:
	if not Engine.is_editor_hint() or not is_inside_tree():
		return
	var preview := get_node_or_null(NodePath(String(EDITOR_PREVIEW_NODE_NAME))) as MeshInstance3D
	if preview == null or preview.mesh == null:
		_refresh_editor_preview()
		preview = get_node_or_null(NodePath(String(EDITOR_PREVIEW_NODE_NAME))) as MeshInstance3D
		if preview == null or preview.mesh == null:
			return
	var selection := EditorInterface.get_selection()
	selection.clear()
	selection.add_node(preview)
	var editor_viewport := EditorInterface.get_editor_viewport_3d(0)
	if editor_viewport == null:
		return
	var editor_camera := editor_viewport.get_camera_3d()
	if editor_camera == null:
		return
	var bounds := preview.get_aabb()
	var target := preview.to_global(bounds.get_center())
	var half_extent := maxf(bounds.size.x, bounds.size.z) * 0.5
	var half_fov := deg_to_rad(clampf(editor_camera.fov, 20.0, 120.0) * 0.5)
	var distance := half_extent / maxf(tan(half_fov), 0.1) * 1.2
	editor_camera.global_position = target + Vector3(0.0, distance, distance * 0.01)
	editor_camera.look_at(target, Vector3.BACK)


func _build_editor_preview_mesh(heightmap: Image) -> ArrayMesh:
	var width := heightmap.get_width()
	var height := heightmap.get_height()
	var step_x := terrain_map.width_m / float(width - 1)
	var step_z := terrain_map.height_m / float(height - 1)
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	vertices.resize(width * height)
	normals.resize(width * height)
	uvs.resize(width * height)
	for z in range(height):
		for x in range(width):
			var index := z * width + x
			var elevation := _decode_editor_elevation(heightmap, x, z)
			vertices[index] = Vector3(
				(-terrain_map.width_m * 0.5 + float(x) * step_x) * editor_preview_scale,
				elevation * editor_preview_scale,
				(-terrain_map.height_m * 0.5 + float(z) * step_z) * editor_preview_scale
			)
			var left := _decode_editor_elevation(heightmap, maxi(x - 1, 0), z)
			var right := _decode_editor_elevation(heightmap, mini(x + 1, width - 1), z)
			var up := _decode_editor_elevation(heightmap, x, maxi(z - 1, 0))
			var down := _decode_editor_elevation(heightmap, x, mini(z + 1, height - 1))
			var span_x := step_x * float(2 if x > 0 and x < width - 1 else 1)
			var span_z := step_z * float(2 if z > 0 and z < height - 1 else 1)
			normals[index] = Vector3(
				-(right - left) / span_x,
				1.0,
				-(down - up) / span_z
			).normalized()
			uvs[index] = Vector2(float(x) / float(width - 1), float(z) / float(height - 1))

	var indices := PackedInt32Array()
	indices.resize((width - 1) * (height - 1) * 6)
	var write_index := 0
	for z in range(height - 1):
		for x in range(width - 1):
			var top_left := z * width + x
			indices[write_index] = top_left
			indices[write_index + 1] = top_left + width
			indices[write_index + 2] = top_left + 1
			indices[write_index + 3] = top_left + 1
			indices[write_index + 4] = top_left + width
			indices[write_index + 5] = top_left + width + 1
			write_index += 6

	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _decode_editor_elevation(heightmap: Image, x: int, z: int) -> float:
	var encoded := roundf(heightmap.get_pixel(x, z).r * 65535.0)
	return (encoded - terrain_map.elevation_offset) / terrain_map.elevation_units_per_m


func _globalize_path(path: String) -> String:
	if path.begins_with("res://") or path.begins_with("user://"):
		return ProjectSettings.globalize_path(path)
	return path
