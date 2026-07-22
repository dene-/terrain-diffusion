@tool
class_name WorldClouds
extends Node

## Scene-facing owner for cloud weather and world anchoring. GPU resource
## lifetime remains isolated inside WorldCloudEffect.

signal runtime_enabled_changed(enabled: bool)

@export var profile: WorldCloudProfile
@export_category("Runtime")
@export var enabled_on_start := true
@export_category("Editor")
@export var editor_preview_enabled := false
@export_node_path("Node3D") var authored_world_path := NodePath("../../WorldRoot/AuthoredWorld")
@export_node_path("Node3D") var ocean_path := NodePath("../../WorldRoot/TerrainWorld3D/Ocean")

var _effect: WorldCloudEffect
var _authored_world: Node3D
var _ocean: Node3D
var _sun: DirectionalLight3D
var _weather_offset_meters := Vector2.ZERO
var _runtime_coverage := 0.56
var _runtime_enabled := true


func _ready() -> void:
	_runtime_enabled = enabled_on_start
	_resolve_scene_dependencies()
	_initialize_weather()
	_apply_profile()
	_apply_effect_enabled()
	if profile != null and not profile.changed.is_connected(_apply_profile):
		profile.changed.connect(_apply_profile)
	set_process(true)
	set_process_unhandled_input(not Engine.is_editor_hint())


func _exit_tree() -> void:
	if profile != null and profile.changed.is_connected(_apply_profile):
		profile.changed.disconnect(_apply_profile)
	if _effect != null:
		_effect.enabled = false


func _process(delta: float) -> void:
	if _effect == null:
		_resolve_scene_dependencies()
	if _effect == null or profile == null:
		return

	_apply_effect_enabled()
	if Engine.is_editor_hint() and not editor_preview_enabled:
		return

	if not Engine.is_editor_hint():
		var wind := profile.wind_direction.normalized()
		_weather_offset_meters += wind * profile.wind_speed_meters_per_second * delta
		# Keeping this bounded preserves float precision while the noise itself tiles.
		var weather_period := maxf(profile.weather_scale_meters, 1000.0)
		_weather_offset_meters.x = fposmod(_weather_offset_meters.x, weather_period)
		_weather_offset_meters.y = fposmod(_weather_offset_meters.y, weather_period)

	var floating_origin := Vector2.ZERO
	if is_instance_valid(_authored_world):
		floating_origin = Vector2(
			-_authored_world.global_position.x,
			-_authored_world.global_position.z)
	var sea_level_meters := -0.08
	if is_instance_valid(_ocean):
		sea_level_meters = _ocean.global_position.y

	var sun_direction := Vector3(0.25, 0.93, 0.25).normalized()
	var sun_color := Vector3.ONE
	var sun_energy := 1.0
	if is_instance_valid(_sun):
		sun_direction = _sun.global_basis.z.normalized()
		sun_color = Vector3(_sun.light_color.r, _sun.light_color.g, _sun.light_color.b)
		sun_energy = _sun.light_energy

	_effect.set_runtime_state(
		floating_origin,
		_weather_offset_meters,
		_runtime_coverage,
		sea_level_meters,
		sun_direction,
		sun_color,
		sun_energy)


func _unhandled_input(event: InputEvent) -> void:
	if Engine.is_editor_hint() or not event.is_action_pressed(&"toggle_clouds"):
		return
	toggle_runtime_enabled()
	get_viewport().set_input_as_handled()


func toggle_runtime_enabled() -> void:
	set_runtime_enabled(not _runtime_enabled)


func set_runtime_enabled(value: bool) -> void:
	if _runtime_enabled == value:
		return
	_runtime_enabled = value
	_apply_effect_enabled()
	runtime_enabled_changed.emit(_runtime_enabled)
	print("[WorldClouds] Clouds %s" % ("enabled" if _runtime_enabled else "disabled"))


func is_runtime_enabled() -> bool:
	return _runtime_enabled


func _apply_effect_enabled() -> void:
	if _effect == null:
		return
	_effect.enabled = editor_preview_enabled if Engine.is_editor_hint() else _runtime_enabled


func _resolve_scene_dependencies() -> void:
	_authored_world = get_node_or_null(authored_world_path) as Node3D
	_ocean = get_node_or_null(ocean_path) as Node3D
	_sun = get_node_or_null(NodePath("../Sun")) as DirectionalLight3D
	_effect = null
	var world_environment := get_parent() as WorldEnvironment
	if world_environment == null or world_environment.compositor == null:
		return
	for compositor_effect in world_environment.compositor.compositor_effects:
		if compositor_effect is WorldCloudEffect:
			_effect = compositor_effect as WorldCloudEffect
			break


func _initialize_weather() -> void:
	if profile == null:
		return
	var random := RandomNumberGenerator.new()
	if Engine.is_editor_hint():
		random.seed = 1
	elif profile.randomize_on_start and profile.startup_seed == 0:
		random.randomize()
	else:
		random.seed = profile.startup_seed
	_weather_offset_meters = Vector2(
		random.randf_range(0.0, profile.weather_scale_meters),
		random.randf_range(0.0, profile.weather_scale_meters))
	_runtime_coverage = clampf(
		profile.coverage + random.randf_range(
			-profile.startup_coverage_variation,
			profile.startup_coverage_variation),
		0.0,
		1.0)


func _apply_profile() -> void:
	if _effect == null or profile == null:
		return
	if Engine.is_editor_hint():
		_runtime_coverage = profile.coverage
	_effect.set_profile_settings(profile.make_render_settings())
