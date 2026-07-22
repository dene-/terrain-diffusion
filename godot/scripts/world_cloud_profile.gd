@tool
class_name WorldCloudProfile
extends Resource

## Authoring data for the world-space cloud renderer. Distances and speeds use
## the same real-world units as the terrain: metres and metres per second.

@export_category("Layer")
@export_range(0.0, 15000.0, 10.0, "or_greater") var base_altitude_meters := 1800.0
@export_range(100.0, 20000.0, 10.0, "or_greater") var top_altitude_meters := 6200.0
@export_range(1000.0, 240000.0, 1000.0) var maximum_distance_meters := 180000.0
@export_range(1000.0, 240000.0, 1000.0) var distance_fade_start_meters := 120000.0

@export_category("Weather")
@export_range(0.0, 1.0, 0.01) var coverage := 0.56
@export_range(0.0, 0.25, 0.01) var startup_coverage_variation := 0.08
@export_range(0.1, 3.0, 0.01) var density := 1.05
@export_range(10000.0, 1000000.0, 1000.0) var weather_scale_meters := 320000.0
@export var randomize_on_start := true
@export var startup_seed := 0
@export var wind_direction := Vector2(0.94, 0.34)
@export_range(0.0, 100.0, 0.5) var wind_speed_meters_per_second := 12.0

@export_category("Shape")
@export_range(500.0, 30000.0, 100.0) var shape_scale_meters := 9000.0
@export_range(100.0, 5000.0, 50.0) var detail_scale_meters := 1050.0
@export_range(0.0, 1.0, 0.01) var detail_erosion := 0.38
@export_range(0.00005, 0.005, 0.00005) var extinction_per_meter := 0.00115

@export_category("Lighting")
@export_range(0.0, 2.0, 0.01) var ambient_strength := 0.34
@export_range(0.0, 0.95, 0.01) var forward_scattering := 0.72
@export_range(-0.95, 0.0, 0.01) var backward_scattering := -0.28
@export_range(0.0, 1.0, 0.01) var backward_scattering_weight := 0.18

@export_category("Quality")
@export_enum("Half:2", "Quarter:4") var resolution_divisor := 4
@export_range(16, 96, 1) var maximum_ray_steps := 64
@export_range(1, 6, 1) var light_steps := 3
# Stable per-pixel stratification breaks up visible integration slices. The
# cloud compositor spatially resolves this pattern without temporal crawling.
@export_range(0.0, 1.0, 0.01) var ray_jitter := 1.0


func make_render_settings() -> Dictionary:
	var safe_base := maxf(base_altitude_meters, 0.0)
	var safe_top := maxf(top_altitude_meters, safe_base + 100.0)
	var safe_maximum_distance := maxf(maximum_distance_meters, 1000.0)
	return {
		"base_altitude_meters": safe_base,
		"top_altitude_meters": safe_top,
		"maximum_distance_meters": safe_maximum_distance,
		"distance_fade_start_meters": clampf(
			distance_fade_start_meters, 0.0, safe_maximum_distance),
		"coverage": clampf(coverage, 0.0, 1.0),
		"density": maxf(density, 0.0),
		"weather_scale_meters": maxf(weather_scale_meters, 1000.0),
		"shape_scale_meters": maxf(shape_scale_meters, 100.0),
		"detail_scale_meters": maxf(detail_scale_meters, 25.0),
		"detail_erosion": clampf(detail_erosion, 0.0, 1.0),
		"extinction_per_meter": maxf(extinction_per_meter, 0.000001),
		"ambient_strength": maxf(ambient_strength, 0.0),
		"forward_scattering": clampf(forward_scattering, 0.0, 0.95),
		"backward_scattering": clampf(backward_scattering, -0.95, 0.0),
		"backward_scattering_weight": clampf(backward_scattering_weight, 0.0, 1.0),
		"resolution_divisor": 2 if resolution_divisor <= 2 else 4,
		"maximum_ray_steps": clampi(maximum_ray_steps, 16, 96),
		"light_steps": clampi(light_steps, 1, 6),
		"ray_jitter": clampf(ray_jitter, 0.0, 1.0),
	}
