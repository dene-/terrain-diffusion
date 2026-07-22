@tool
class_name PhysicalAtmosphere
extends WorldEnvironment

const BETA_RAYLEIGH := Vector3(5.804544e-6, 1.3562913e-5, 3.311258e-5)
const BETA_MIE := 2.1e-5
const AXIAL_TILT_RADIANS := deg_to_rad(23.44)

@export_category("Solar Position")
@export_range(0.0, 23.999, 0.001) var solar_time_hours := 11.05:
	set(value):
		solar_time_hours = fposmod(value, 24.0)
		_apply_solar_state()
@export_range(-90.0, 90.0, 0.1) var latitude_degrees := 35.0:
	set(value):
		latitude_degrees = clampf(value, -90.0, 90.0)
		_apply_solar_state()
@export_range(1, 365, 1) var day_of_year := 172:
	set(value):
		day_of_year = clampi(value, 1, 365)
		_apply_solar_state()

@export_category("Day / Night Cycle")
@export var game_time_enabled := true:
	set(value):
		game_time_enabled = value
		_update_processing()
@export var editor_time_enabled := false:
	set(value):
		editor_time_enabled = value
		_update_processing()
@export_range(1.0, 1440.0, 1.0) var minutes_per_day := 96.0
@export_range(0.02, 1.0, 0.01) var update_interval_seconds := 0.10

@export_category("Physical Atmosphere")
@export_range(10000.0, 200000.0, 1000.0) var atmosphere_height_meters := 100000.0:
	set(value):
		atmosphere_height_meters = value
		_apply_physical_parameters()
@export_range(1000.0, 20000.0, 100.0) var rayleigh_scale_height_meters := 8400.0:
	set(value):
		rayleigh_scale_height_meters = maxf(value, 1.0)
		_apply_physical_parameters()
@export_range(100.0, 10000.0, 50.0) var mie_scale_height_meters := 1250.0:
	set(value):
		mie_scale_height_meters = maxf(value, 1.0)
		_apply_physical_parameters()
@export_range(0.0, 4.0, 0.01) var rayleigh_scattering_multiplier := 1.0:
	set(value):
		rayleigh_scattering_multiplier = maxf(value, 0.0)
		_apply_physical_parameters()
@export_range(0.0, 8.0, 0.05) var maximum_sun_energy := 1.15:
	set(value):
		maximum_sun_energy = value
		_apply_solar_state()
@export_range(1.0, 40.0, 0.1) var solar_radiance := 16.0:
	set(value):
		solar_radiance = value
		_apply_physical_parameters()
@export_range(0.0, 0.0001, 0.000001) var aerosol_extinction_per_meter := BETA_MIE:
	set(value):
		aerosol_extinction_per_meter = value
		_apply_physical_parameters()
		_apply_solar_state()

@export_category("Aerial Perspective")
@export_range(0.0, 100000.0, 100.0) var aerial_perspective_start_meters := 0.0:
	set(value):
		aerial_perspective_start_meters = maxf(value, 0.0)
		_apply_physical_parameters()
@export_range(0.0, 4.0, 0.01) var aerial_perspective_strength := 1.0:
	set(value):
		aerial_perspective_strength = maxf(value, 0.0)
		_apply_physical_parameters()
@export_range(1000.0, 240000.0, 1000.0) var maximum_visibility_distance_meters := 240000.0:
	set(value):
		maximum_visibility_distance_meters = clampf(value, 1000.0, 240000.0)
		if is_inside_tree():
			set_viewer_state(
				_viewer_altitude_meters,
				_requested_maximum_distance_meters,
				_viewer_height_agl_meters,
				_viewer_height_signed_meters)

var _sun: DirectionalLight3D
var _aerial_material: ShaderMaterial
var _sky_material: ShaderMaterial
var _ocean_material: ShaderMaterial
var _viewer_altitude_meters := 0.0
var _viewer_height_agl_meters := 0.0
var _viewer_height_signed_meters := 0.0
var _maximum_distance_meters := 200000.0
var _requested_maximum_distance_meters := 200000.0
var _last_sky_altitude_meters := -1e20
var _cycle_accumulator_seconds := 0.0


func _ready() -> void:
	_resolve_scene_resources()
	_configure_environment()
	_apply_physical_parameters()
	_apply_solar_state()
	set_viewer_state(
		_viewer_altitude_meters,
		_maximum_distance_meters,
		_viewer_height_agl_meters,
		_viewer_height_signed_meters)
	_update_processing()


func _process(delta: float) -> void:
	_cycle_accumulator_seconds += delta
	if _cycle_accumulator_seconds < update_interval_seconds:
		return
	var elapsed := _cycle_accumulator_seconds
	_cycle_accumulator_seconds = 0.0
	var real_seconds_per_day := maxf(minutes_per_day * 60.0, 1.0)
	solar_time_hours = fposmod(solar_time_hours + elapsed * 24.0 / real_seconds_per_day, 24.0)


func set_viewer_state(
	viewer_altitude_meters: float,
	maximum_distance_meters: float,
	viewer_height_agl_meters: float,
	viewer_height_signed_meters: float,
) -> void:
	_viewer_altitude_meters = maxf(viewer_altitude_meters, 0.0)
	_viewer_height_agl_meters = maxf(viewer_height_agl_meters, 0.0)
	_viewer_height_signed_meters = viewer_height_signed_meters
	_requested_maximum_distance_meters = maxf(maximum_distance_meters, 1000.0)
	_maximum_distance_meters = clampf(
		_requested_maximum_distance_meters,
		1000.0,
		maximum_visibility_distance_meters)
	if _aerial_material != null:
		_aerial_material.set_shader_parameter("viewer_altitude_meters", _viewer_altitude_meters)
		_aerial_material.set_shader_parameter(
			"viewer_height_signed_meters", _viewer_height_signed_meters)
		_aerial_material.set_shader_parameter("maximum_distance_meters", _maximum_distance_meters)
	if _sky_material != null:
		_sky_material.set_shader_parameter("maximum_distance_meters", _maximum_distance_meters)
	_update_sun_shadow_state()
	if _sky_material != null and absf(_viewer_altitude_meters - _last_sky_altitude_meters) >= 25.0:
		_sky_material.set_shader_parameter("viewer_altitude_meters", _viewer_altitude_meters)
		_last_sky_altitude_meters = _viewer_altitude_meters


func _resolve_scene_resources() -> void:
	_sun = get_node_or_null("Sun") as DirectionalLight3D
	var aerial_mesh := get_node_or_null("AerialPerspective") as MeshInstance3D
	if aerial_mesh != null:
		_aerial_material = aerial_mesh.get_active_material(0) as ShaderMaterial
	if environment != null and environment.sky != null:
		_sky_material = environment.sky.sky_material as ShaderMaterial
	var ocean := get_node_or_null("../WorldRoot/TerrainWorld3D/Ocean") as MeshInstance3D
	if ocean != null:
		_ocean_material = ocean.get_active_material(0) as ShaderMaterial


func _configure_environment() -> void:
	if environment == null:
		return
	environment.background_mode = Environment.BG_SKY
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	environment.tonemap_mode = Environment.TONE_MAPPER_ACES
	environment.tonemap_white = 6.0
	environment.fog_enabled = false
	environment.volumetric_fog_enabled = false
	environment.ssao_enabled = false
	environment.ssao_radius = 2.2
	environment.ssao_intensity = 1.25
	environment.ssao_power = 1.35
	environment.ssao_detail = 0.55
	environment.ssao_horizon = 0.08
	environment.ssao_sharpness = 0.90
	environment.ssao_ao_channel_affect = 0.85


func _apply_physical_parameters() -> void:
	if not is_inside_tree():
		return
	if _sky_material == null or _aerial_material == null:
		_resolve_scene_resources()
	for material: ShaderMaterial in [_sky_material, _aerial_material]:
		if material == null:
			continue
		material.set_shader_parameter("atmosphere_height_meters", atmosphere_height_meters)
		material.set_shader_parameter("rayleigh_scale_height_meters", rayleigh_scale_height_meters)
		material.set_shader_parameter("mie_scale_height_meters", mie_scale_height_meters)
		material.set_shader_parameter(
			"beta_rayleigh_per_meter",
			BETA_RAYLEIGH * rayleigh_scattering_multiplier)
		material.set_shader_parameter("beta_mie_per_meter", aerosol_extinction_per_meter)
		material.set_shader_parameter("solar_radiance", solar_radiance)
	if _aerial_material != null:
		_aerial_material.set_shader_parameter(
			"aerial_perspective_start_meters", aerial_perspective_start_meters)
		_aerial_material.set_shader_parameter(
			"aerial_perspective_strength", aerial_perspective_strength)


func _apply_solar_state() -> void:
	if not is_inside_tree():
		return
	if _sun == null or _sky_material == null or _aerial_material == null:
		_resolve_scene_resources()
	var sun_direction := _calculate_sun_direction()
	if _sky_material != null:
		_sky_material.set_shader_parameter("sun_direction", sun_direction)
	if _aerial_material != null:
		_aerial_material.set_shader_parameter("sun_direction", sun_direction)
	if _ocean_material != null:
		_ocean_material.set_shader_parameter("sun_direction_world", sun_direction)
	if _sun == null:
		return

	var basis_up := Vector3.FORWARD if absf(sun_direction.y) > 0.995 else Vector3.UP
	_sun.transform = Transform3D(Basis.looking_at(-sun_direction, basis_up), Vector3.ZERO)
	var elevation_radians := asin(clampf(sun_direction.y, -1.0, 1.0))
	var elevation_degrees := rad_to_deg(elevation_radians)
	var daylight := smoothstep(-0.833, 2.0, elevation_degrees)
	var direct_transmittance := _direct_sun_transmittance(elevation_degrees)
	var normalization := maxf(
		maxf(direct_transmittance.x, direct_transmittance.y),
		direct_transmittance.z)
	var normalized_color := direct_transmittance / maxf(normalization, 1e-5)
	_sun.light_color = Color(normalized_color.x, normalized_color.y, normalized_color.z)
	_sun.light_energy = maximum_sun_energy * daylight * normalization
	if _ocean_material != null:
		_ocean_material.set_shader_parameter("sun_light_color", normalized_color)
		_ocean_material.set_shader_parameter("daylight", daylight)
	_update_sun_shadow_state(daylight)
	_sun.set_param(Light3D.PARAM_SIZE, 0.53)

	if environment != null:
		var twilight := smoothstep(-8.0, 8.0, elevation_degrees)
		var ambient_color := Color(0.22, 0.34, 0.58).lerp(Color(0.54, 0.66, 0.88), twilight)
		environment.ambient_light_color = ambient_color
		environment.ambient_light_energy = lerpf(0.012, 0.42, twilight)


func _update_sun_shadow_state(daylight := -1.0) -> void:
	if _sun == null:
		return
	var effective_daylight := daylight
	if effective_daylight < 0.0:
		effective_daylight = 1.0 if _sun.light_energy > 0.001 else 0.0
	# A directional shadow texture is a finite camera-centred projection. Its
	# boundary is useful near the ground but becomes a giant moving rectangle
	# from aircraft views. Fade it by physical height above terrain, while the
	# sun and macro terrain lighting remain active at every altitude.
	var altitude_visibility := 1.0 - smoothstep(
		80.0, 350.0, _viewer_height_agl_meters)
	_sun.shadow_opacity = 0.72 * altitude_visibility
	_sun.shadow_enabled = effective_daylight > 0.01 and altitude_visibility > 0.01
	_sun.directional_shadow_fade_start = 0.45


func _calculate_sun_direction() -> Vector3:
	var latitude := deg_to_rad(latitude_degrees)
	var declination := AXIAL_TILT_RADIANS * sin(
		TAU * (284.0 + float(day_of_year)) / 365.0)
	var hour_angle := deg_to_rad((solar_time_hours - 12.0) * 15.0)
	var east := -cos(declination) * sin(hour_angle)
	var north := (
		cos(latitude) * sin(declination)
		- sin(latitude) * cos(declination) * cos(hour_angle))
	var up := (
		sin(latitude) * sin(declination)
		+ cos(latitude) * cos(declination) * cos(hour_angle))
	return Vector3(east, up, -north).normalized()


func _direct_sun_transmittance(elevation_degrees: float) -> Vector3:
	if elevation_degrees <= -0.833:
		return Vector3.ZERO
	var safe_elevation := maxf(elevation_degrees, -5.9)
	var air_mass := 1.0 / (
		sin(deg_to_rad(safe_elevation))
		+ 0.50572 * pow(safe_elevation + 6.07995, -1.6364))
	air_mass = clampf(air_mass, 1.0, 40.0)
	var optical_depth := (
		BETA_RAYLEIGH * rayleigh_scattering_multiplier
			* (rayleigh_scale_height_meters * air_mass)
		+ Vector3.ONE * (aerosol_extinction_per_meter * mie_scale_height_meters * air_mass))
	return Vector3(
		exp(-optical_depth.x),
		exp(-optical_depth.y),
		exp(-optical_depth.z))


func _update_processing() -> void:
	if not is_inside_tree():
		return
	set_process(editor_time_enabled if Engine.is_editor_hint() else game_time_enabled)
