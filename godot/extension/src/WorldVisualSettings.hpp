#pragma once

#include <terrain/VisualConfig.hpp>

#include <godot_cpp/classes/resource.hpp>

#include <cstdint>

namespace terrain::godot_adapter {

struct WaterVisualConfig final {
    float detailWavelengthMeters{13.0F};
    float timeScale{0.42F};
    float detailNormalStrength{0.72F};
    float macroWavelengthMeters{260.0F};
    float macroHeightMeters{1.15F};
    float waveFadeStartMeters{450.0F};
    float waveFadeEndMeters{3'200.0F};
};

// Serializable Godot-facing view of the renderer-independent visual config.
// The scene-owned physical atmosphere remains authoritative; the lighting and
// atmosphere values below configure the built-in fallback and custom terrain materials.
class WorldVisualSettings final : public godot::Resource {
    GDCLASS(WorldVisualSettings, godot::Resource)

public:
    [[nodiscard]] WorldVisualConfig to_world_config() const noexcept;
    [[nodiscard]] WaterVisualConfig water() const noexcept;

#define TERRAIN_DECLARE_FLOAT_SETTING(name) \
    void set_##name(float value); \
    [[nodiscard]] float get_##name() const noexcept;
#define TERRAIN_DECLARE_INT_SETTING(name) \
    void set_##name(std::int64_t value); \
    [[nodiscard]] std::int64_t get_##name() const noexcept;

    TERRAIN_DECLARE_FLOAT_SETTING(meters_per_world_unit)
    TERRAIN_DECLARE_FLOAT_SETTING(elevation_meters_per_source_unit)
    TERRAIN_DECLARE_FLOAT_SETTING(vertical_exaggeration)

    TERRAIN_DECLARE_FLOAT_SETTING(terrain_albedo_gain)
    TERRAIN_DECLARE_FLOAT_SETTING(rock_slope_start)
    TERRAIN_DECLARE_FLOAT_SETTING(rock_slope_full)
    TERRAIN_DECLARE_FLOAT_SETTING(scree_slope_start)
    TERRAIN_DECLARE_FLOAT_SETTING(scree_slope_full)
    TERRAIN_DECLARE_FLOAT_SETTING(macro_texture_scale_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(detail_fade_start_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(detail_fade_end_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(max_screen_space_error_pixels)
    TERRAIN_DECLARE_FLOAT_SETTING(lod_hysteresis)

    TERRAIN_DECLARE_FLOAT_SETTING(forest_density)
    TERRAIN_DECLARE_FLOAT_SETTING(forest_clearing_scale_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(forest_clearing_strength)
    TERRAIN_DECLARE_FLOAT_SETTING(forest_maximum_slope)
    TERRAIN_DECLARE_FLOAT_SETTING(forest_minimum_temperature)
    TERRAIN_DECLARE_FLOAT_SETTING(tree_line_start_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(tree_line_end_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(tree_billboard_size_multiplier)
    TERRAIN_DECLARE_FLOAT_SETTING(tree_near_pixel_threshold)
    TERRAIN_DECLARE_FLOAT_SETTING(tree_mid_pixel_threshold)
    TERRAIN_DECLARE_FLOAT_SETTING(tree_cull_pixel_threshold)
    TERRAIN_DECLARE_FLOAT_SETTING(tree_shadow_distance_meters)

    TERRAIN_DECLARE_FLOAT_SETTING(snow_temperature_start_c)
    TERRAIN_DECLARE_FLOAT_SETTING(snow_temperature_full_c)
    TERRAIN_DECLARE_FLOAT_SETTING(snow_slope_retention_start)
    TERRAIN_DECLARE_FLOAT_SETTING(snow_slope_retention_end)
    TERRAIN_DECLARE_FLOAT_SETTING(snow_aspect_strength)

    TERRAIN_DECLARE_FLOAT_SETTING(fog_distance_density)
    TERRAIN_DECLARE_FLOAT_SETTING(fog_height_falloff)
    TERRAIN_DECLARE_FLOAT_SETTING(fog_base_height_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(aerial_perspective_strength)
    TERRAIN_DECLARE_FLOAT_SETTING(fallback_turbidity)

    TERRAIN_DECLARE_FLOAT_SETTING(fallback_sun_azimuth_degrees)
    TERRAIN_DECLARE_FLOAT_SETTING(fallback_sun_elevation_degrees)
    TERRAIN_DECLARE_FLOAT_SETTING(fallback_sun_intensity)
    TERRAIN_DECLARE_FLOAT_SETTING(fallback_sky_intensity)
    TERRAIN_DECLARE_FLOAT_SETTING(fallback_exposure)

    TERRAIN_DECLARE_INT_SETTING(max_concurrent_requests)
    TERRAIN_DECLARE_INT_SETTING(max_gpu_uploads_per_frame)
    TERRAIN_DECLARE_INT_SETTING(max_gpu_upload_megabytes_per_frame)
    TERRAIN_DECLARE_INT_SETTING(terrain_memory_megabytes)
    TERRAIN_DECLARE_INT_SETTING(vegetation_memory_megabytes)
    TERRAIN_DECLARE_INT_SETTING(max_resident_tiles)

    TERRAIN_DECLARE_FLOAT_SETTING(ocean_detail_wavelength_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(ocean_time_scale)
    TERRAIN_DECLARE_FLOAT_SETTING(ocean_detail_normal_strength)
    TERRAIN_DECLARE_FLOAT_SETTING(ocean_macro_wavelength_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(ocean_macro_height_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(ocean_wave_fade_start_meters)
    TERRAIN_DECLARE_FLOAT_SETTING(ocean_wave_fade_end_meters)

    TERRAIN_DECLARE_INT_SETTING(default_debug_view)

#undef TERRAIN_DECLARE_FLOAT_SETTING
#undef TERRAIN_DECLARE_INT_SETTING

protected:
    static void _bind_methods();

private:
    WorldVisualConfig config_{};
    WaterVisualConfig water_{};
    std::int64_t default_debug_view_{};
};

} // namespace terrain::godot_adapter
