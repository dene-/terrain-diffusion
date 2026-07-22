#include "WorldVisualSettings.hpp"

#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cstddef>

namespace terrain::godot_adapter {
namespace {

using godot::PropertyHint;
using godot::PropertyInfo;
using godot::Variant;

template <typename T>
T clamp_size(const std::int64_t value, const std::int64_t minimum, const std::int64_t maximum) {
    return static_cast<T>(std::clamp(value, minimum, maximum));
}

} // namespace

WorldVisualConfig WorldVisualSettings::to_world_config() const noexcept {
    auto result = config_;
    const auto order_unit_range = [](float& start, float& end) {
        if (start > end) std::swap(start, end);
        if (end - start < 0.001F) {
            start = std::max(0.0F, start - 0.001F);
            end = std::min(1.0F, end + 0.001F);
        }
    };
    order_unit_range(result.terrain.rockSlopeStart, result.terrain.rockSlopeFull);
    order_unit_range(result.terrain.screeSlopeStart, result.terrain.screeSlopeFull);
    order_unit_range(result.snow.slopeRetentionStart, result.snow.slopeRetentionEnd);
    if (result.terrain.detailFadeStartMeters >= result.terrain.detailFadeEndMeters) {
        result.terrain.detailFadeEndMeters = result.terrain.detailFadeStartMeters + 1.0F;
    }
    if (result.forest.treeLineStartMeters >= result.forest.treeLineEndMeters) {
        result.forest.treeLineEndMeters = result.forest.treeLineStartMeters + 1.0F;
    }
    if (result.snow.temperatureFullC >= result.snow.temperatureStartC) {
        std::swap(result.snow.temperatureFullC, result.snow.temperatureStartC);
        if (result.snow.temperatureFullC >= result.snow.temperatureStartC) {
            result.snow.temperatureFullC = result.snow.temperatureStartC - 0.25F;
        }
    }
    result.forest.midPixelThreshold = std::min(
        result.forest.midPixelThreshold,
        result.forest.nearPixelThreshold);
    result.forest.geometryCullPixelThreshold = std::min(
        result.forest.geometryCullPixelThreshold,
        result.forest.midPixelThreshold);
    return result;
}

WaterVisualConfig WorldVisualSettings::water() const noexcept {
    auto result = water_;
    if (result.waveFadeStartMeters >= result.waveFadeEndMeters) {
        result.waveFadeEndMeters = result.waveFadeStartMeters + 1.0F;
    }
    return result;
}

void WorldVisualSettings::_bind_methods() {
    using godot::ClassDB;

#define TERRAIN_BIND_SETTING(name) \
    ClassDB::bind_method(godot::D_METHOD("set_" #name, "value"), &WorldVisualSettings::set_##name); \
    ClassDB::bind_method(godot::D_METHOD("get_" #name), &WorldVisualSettings::get_##name)
#define TERRAIN_BIND_FLOAT(name, range) \
    TERRAIN_BIND_SETTING(name); \
    ADD_PROPERTY( \
        PropertyInfo(Variant::FLOAT, #name, PropertyHint::PROPERTY_HINT_RANGE, range), \
        "set_" #name, \
        "get_" #name)
#define TERRAIN_BIND_INT(name, range) \
    TERRAIN_BIND_SETTING(name); \
    ADD_PROPERTY( \
        PropertyInfo(Variant::INT, #name, PropertyHint::PROPERTY_HINT_RANGE, range), \
        "set_" #name, \
        "get_" #name)

    ADD_GROUP("Metrics", "");
    TERRAIN_BIND_FLOAT(meters_per_world_unit, "0.01,100.0,0.01,or_greater,suffix:m/unit");
    TERRAIN_BIND_FLOAT(elevation_meters_per_source_unit, "0.01,100.0,0.01,or_greater,suffix:m/source unit");
    TERRAIN_BIND_FLOAT(vertical_exaggeration, "0.1,4.0,0.01,suffix:x");

    ADD_GROUP("Terrain", "");
    TERRAIN_BIND_FLOAT(terrain_albedo_gain, "0.05,2.0,0.01");
    TERRAIN_BIND_FLOAT(rock_slope_start, "0.0,1.0,0.01");
    TERRAIN_BIND_FLOAT(rock_slope_full, "0.0,1.0,0.01");
    TERRAIN_BIND_FLOAT(scree_slope_start, "0.0,1.0,0.01");
    TERRAIN_BIND_FLOAT(scree_slope_full, "0.0,1.0,0.01");
    TERRAIN_BIND_FLOAT(macro_texture_scale_meters, "1.0,10000.0,1.0,or_greater,suffix:m");
    TERRAIN_BIND_FLOAT(detail_fade_start_meters, "0.0,50000.0,10.0,or_greater,suffix:m");
    TERRAIN_BIND_FLOAT(detail_fade_end_meters, "1.0,200000.0,10.0,or_greater,suffix:m");
    TERRAIN_BIND_FLOAT(max_screen_space_error_pixels, "0.25,16.0,0.05,suffix:px");
    TERRAIN_BIND_FLOAT(lod_hysteresis, "0.0,1.0,0.01");

    ADD_GROUP("Forest", "");
    TERRAIN_BIND_FLOAT(forest_density, "0.0,1.0,0.01");
    TERRAIN_BIND_FLOAT(forest_clearing_scale_meters, "1.0,10000.0,1.0,or_greater,suffix:m");
    TERRAIN_BIND_FLOAT(forest_clearing_strength, "0.0,1.0,0.01");
    TERRAIN_BIND_FLOAT(forest_maximum_slope, "0.0,1.0,0.01");
    TERRAIN_BIND_FLOAT(forest_minimum_temperature, "-60.0,40.0,0.5,suffix:C");
    TERRAIN_BIND_FLOAT(tree_line_start_meters, "0.0,10000.0,10.0,suffix:m");
    TERRAIN_BIND_FLOAT(tree_line_end_meters, "0.0,12000.0,10.0,suffix:m");
    TERRAIN_BIND_FLOAT(tree_billboard_size_multiplier, "0.1,4.0,0.01,suffix:x");
    TERRAIN_BIND_FLOAT(tree_near_pixel_threshold, "1.0,128.0,0.5,suffix:px");
    TERRAIN_BIND_FLOAT(tree_mid_pixel_threshold, "0.25,64.0,0.25,suffix:px");
    TERRAIN_BIND_FLOAT(tree_cull_pixel_threshold, "0.1,16.0,0.05,suffix:px");
    TERRAIN_BIND_FLOAT(tree_shadow_distance_meters, "0.0,10000.0,10.0,suffix:m");

    ADD_GROUP("Snow", "");
    TERRAIN_BIND_FLOAT(snow_temperature_start_c, "-30.0,20.0,0.25,suffix:C");
    TERRAIN_BIND_FLOAT(snow_temperature_full_c, "-50.0,10.0,0.25,suffix:C");
    TERRAIN_BIND_FLOAT(snow_slope_retention_start, "0.0,1.0,0.01");
    TERRAIN_BIND_FLOAT(snow_slope_retention_end, "0.0,1.0,0.01");
    TERRAIN_BIND_FLOAT(snow_aspect_strength, "0.0,1.0,0.01");

    ADD_GROUP("Atmosphere Fallback", "");
    TERRAIN_BIND_FLOAT(fog_distance_density, "0.0,0.01,0.000001");
    TERRAIN_BIND_FLOAT(fog_height_falloff, "0.0,0.01,0.000001");
    TERRAIN_BIND_FLOAT(fog_base_height_meters, "-20000.0,20000.0,1.0,suffix:m");
    TERRAIN_BIND_FLOAT(aerial_perspective_strength, "0.0,4.0,0.01");
    TERRAIN_BIND_FLOAT(fallback_turbidity, "0.0,20.0,0.1");

    ADD_GROUP("Lighting Fallback", "");
    TERRAIN_BIND_FLOAT(fallback_sun_azimuth_degrees, "-360.0,360.0,0.5,suffix:deg");
    TERRAIN_BIND_FLOAT(fallback_sun_elevation_degrees, "-90.0,90.0,0.5,suffix:deg");
    TERRAIN_BIND_FLOAT(fallback_sun_intensity, "0.0,20.0,0.01");
    TERRAIN_BIND_FLOAT(fallback_sky_intensity, "0.0,20.0,0.01");
    TERRAIN_BIND_FLOAT(fallback_exposure, "0.05,8.0,0.01");

    ADD_GROUP("Streaming", "");
    TERRAIN_BIND_INT(max_concurrent_requests, "1,16,1");
    TERRAIN_BIND_INT(max_gpu_uploads_per_frame, "1,32,1");
    TERRAIN_BIND_INT(max_gpu_upload_megabytes_per_frame, "1,1024,1,suffix:MB");
    TERRAIN_BIND_INT(terrain_memory_megabytes, "64,16384,16,suffix:MB");
    TERRAIN_BIND_INT(vegetation_memory_megabytes, "32,8192,16,suffix:MB");
    TERRAIN_BIND_INT(max_resident_tiles, "32,8192,1");

    ADD_GROUP("Ocean", "");
    TERRAIN_BIND_FLOAT(ocean_detail_wavelength_meters, "1.0,100.0,0.5,suffix:m");
    TERRAIN_BIND_FLOAT(ocean_time_scale, "0.0,3.0,0.01");
    TERRAIN_BIND_FLOAT(ocean_detail_normal_strength, "0.0,2.0,0.01");
    TERRAIN_BIND_FLOAT(ocean_macro_wavelength_meters, "25.0,1000.0,1.0,suffix:m");
    TERRAIN_BIND_FLOAT(ocean_macro_height_meters, "0.0,5.0,0.01,suffix:m");
    TERRAIN_BIND_FLOAT(ocean_wave_fade_start_meters, "10.0,5000.0,10.0,suffix:m");
    TERRAIN_BIND_FLOAT(ocean_wave_fade_end_meters, "100.0,20000.0,10.0,suffix:m");

    ADD_GROUP("Debug", "");
    TERRAIN_BIND_SETTING(default_debug_view);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "default_debug_view",
            PropertyHint::PROPERTY_HINT_ENUM,
            "Shaded,Elevation,Normals,Slope,Curvature,Wetness,Forest,Rock,Scree,Snow,Terrain LOD,Fog Amount,PSSM Splits,Vegetation LOD"),
        "set_default_debug_view",
        "get_default_debug_view");

#undef TERRAIN_BIND_SETTING
#undef TERRAIN_BIND_FLOAT
#undef TERRAIN_BIND_INT
}

#define TERRAIN_FLOAT_SETTING(name, member, minimum, maximum) \
    void WorldVisualSettings::set_##name(const float value) { \
        member = std::clamp(value, minimum, maximum); \
        emit_changed(); \
    } \
    float WorldVisualSettings::get_##name() const noexcept { return member; }

TERRAIN_FLOAT_SETTING(meters_per_world_unit, config_.metrics.metersPerWorldUnit, 0.01F, 100.0F)
TERRAIN_FLOAT_SETTING(elevation_meters_per_source_unit, config_.metrics.elevationMetersPerSourceUnit, 0.01F, 100.0F)
TERRAIN_FLOAT_SETTING(vertical_exaggeration, config_.metrics.verticalExaggeration, 0.1F, 4.0F)

TERRAIN_FLOAT_SETTING(terrain_albedo_gain, config_.terrain.albedoGain, 0.05F, 2.0F)
TERRAIN_FLOAT_SETTING(rock_slope_start, config_.terrain.rockSlopeStart, 0.0F, 1.0F)
TERRAIN_FLOAT_SETTING(rock_slope_full, config_.terrain.rockSlopeFull, 0.0F, 1.0F)
TERRAIN_FLOAT_SETTING(scree_slope_start, config_.terrain.screeSlopeStart, 0.0F, 1.0F)
TERRAIN_FLOAT_SETTING(scree_slope_full, config_.terrain.screeSlopeFull, 0.0F, 1.0F)
TERRAIN_FLOAT_SETTING(macro_texture_scale_meters, config_.terrain.macroTextureScaleMeters, 1.0F, 1'000'000.0F)
TERRAIN_FLOAT_SETTING(detail_fade_start_meters, config_.terrain.detailFadeStartMeters, 0.0F, 1'000'000.0F)
TERRAIN_FLOAT_SETTING(detail_fade_end_meters, config_.terrain.detailFadeEndMeters, 1.0F, 1'000'000.0F)
TERRAIN_FLOAT_SETTING(max_screen_space_error_pixels, config_.terrain.maxScreenSpaceErrorPixels, 0.25F, 16.0F)
TERRAIN_FLOAT_SETTING(lod_hysteresis, config_.terrain.lodHysteresis, 0.0F, 1.0F)

TERRAIN_FLOAT_SETTING(forest_density, config_.forest.globalDensity, 0.0F, 1.0F)
TERRAIN_FLOAT_SETTING(forest_clearing_scale_meters, config_.forest.clearingScaleMeters, 1.0F, 1'000'000.0F)
TERRAIN_FLOAT_SETTING(forest_clearing_strength, config_.forest.clearingStrength, 0.0F, 1.0F)
TERRAIN_FLOAT_SETTING(forest_maximum_slope, config_.forest.maximumSlope, 0.0F, 1.0F)
TERRAIN_FLOAT_SETTING(forest_minimum_temperature, config_.forest.minimumTemperature, -100.0F, 100.0F)
TERRAIN_FLOAT_SETTING(tree_line_start_meters, config_.forest.treeLineStartMeters, 0.0F, 20'000.0F)
TERRAIN_FLOAT_SETTING(tree_line_end_meters, config_.forest.treeLineEndMeters, 0.0F, 20'000.0F)
TERRAIN_FLOAT_SETTING(tree_billboard_size_multiplier, config_.forest.billboardSizeMultiplier, 0.1F, 4.0F)
TERRAIN_FLOAT_SETTING(tree_near_pixel_threshold, config_.forest.nearPixelThreshold, 0.1F, 128.0F)
TERRAIN_FLOAT_SETTING(tree_mid_pixel_threshold, config_.forest.midPixelThreshold, 0.1F, 128.0F)
TERRAIN_FLOAT_SETTING(tree_cull_pixel_threshold, config_.forest.geometryCullPixelThreshold, 0.01F, 64.0F)
TERRAIN_FLOAT_SETTING(tree_shadow_distance_meters, config_.forest.nearShadowDistanceMeters, 0.0F, 100'000.0F)

TERRAIN_FLOAT_SETTING(snow_temperature_start_c, config_.snow.temperatureStartC, -100.0F, 100.0F)
TERRAIN_FLOAT_SETTING(snow_temperature_full_c, config_.snow.temperatureFullC, -100.0F, 100.0F)
TERRAIN_FLOAT_SETTING(snow_slope_retention_start, config_.snow.slopeRetentionStart, 0.0F, 1.0F)
TERRAIN_FLOAT_SETTING(snow_slope_retention_end, config_.snow.slopeRetentionEnd, 0.0F, 1.0F)
TERRAIN_FLOAT_SETTING(snow_aspect_strength, config_.snow.aspectStrength, 0.0F, 1.0F)

TERRAIN_FLOAT_SETTING(fog_distance_density, config_.atmosphere.distanceDensity, 0.0F, 0.1F)
TERRAIN_FLOAT_SETTING(fog_height_falloff, config_.atmosphere.heightFalloff, 0.0F, 0.1F)
TERRAIN_FLOAT_SETTING(fog_base_height_meters, config_.atmosphere.baseHeightMeters, -100'000.0F, 100'000.0F)
TERRAIN_FLOAT_SETTING(aerial_perspective_strength, config_.atmosphere.aerialPerspectiveStrength, 0.0F, 8.0F)
TERRAIN_FLOAT_SETTING(fallback_turbidity, config_.atmosphere.turbidity, 0.0F, 20.0F)

TERRAIN_FLOAT_SETTING(fallback_sun_azimuth_degrees, config_.lighting.sunAzimuthDegrees, -360.0F, 360.0F)
TERRAIN_FLOAT_SETTING(fallback_sun_elevation_degrees, config_.lighting.sunElevationDegrees, -90.0F, 90.0F)
TERRAIN_FLOAT_SETTING(fallback_sun_intensity, config_.lighting.sunIntensity, 0.0F, 20.0F)
TERRAIN_FLOAT_SETTING(fallback_sky_intensity, config_.lighting.skyIntensity, 0.0F, 20.0F)
TERRAIN_FLOAT_SETTING(fallback_exposure, config_.lighting.exposure, 0.05F, 8.0F)

#define TERRAIN_INT_SETTING(name, member, minimum, maximum) \
    void WorldVisualSettings::set_##name(const std::int64_t value) { \
        member = clamp_size<decltype(member)>(value, minimum, maximum); \
        emit_changed(); \
    } \
    std::int64_t WorldVisualSettings::get_##name() const noexcept { \
        return static_cast<std::int64_t>(member); \
    }

TERRAIN_INT_SETTING(max_concurrent_requests, config_.streaming.maxConcurrentRequests, 1, 16)
TERRAIN_INT_SETTING(max_gpu_uploads_per_frame, config_.streaming.maxGpuUploadsPerFrame, 1, 32)

void WorldVisualSettings::set_max_gpu_upload_megabytes_per_frame(const std::int64_t value) {
    const auto megabytes = clamp_size<std::size_t>(value, 1, 1'024);
    config_.streaming.maxGpuUploadBytesPerFrame = megabytes * 1'024U * 1'024U;
    emit_changed();
}

std::int64_t WorldVisualSettings::get_max_gpu_upload_megabytes_per_frame() const noexcept {
    return static_cast<std::int64_t>(
        config_.streaming.maxGpuUploadBytesPerFrame / (1'024U * 1'024U));
}

TERRAIN_INT_SETTING(terrain_memory_megabytes, config_.streaming.terrainMemoryMegabytes, 64, 16'384)
TERRAIN_INT_SETTING(vegetation_memory_megabytes, config_.streaming.vegetationMemoryMegabytes, 32, 8'192)
TERRAIN_INT_SETTING(max_resident_tiles, config_.streaming.maxResidentTiles, 32, 8'192)

TERRAIN_FLOAT_SETTING(ocean_detail_wavelength_meters, water_.detailWavelengthMeters, 1.0F, 100.0F)
TERRAIN_FLOAT_SETTING(ocean_time_scale, water_.timeScale, 0.0F, 3.0F)
TERRAIN_FLOAT_SETTING(ocean_detail_normal_strength, water_.detailNormalStrength, 0.0F, 2.0F)
TERRAIN_FLOAT_SETTING(ocean_macro_wavelength_meters, water_.macroWavelengthMeters, 25.0F, 1'000.0F)
TERRAIN_FLOAT_SETTING(ocean_macro_height_meters, water_.macroHeightMeters, 0.0F, 5.0F)
TERRAIN_FLOAT_SETTING(ocean_wave_fade_start_meters, water_.waveFadeStartMeters, 10.0F, 20'000.0F)
TERRAIN_FLOAT_SETTING(ocean_wave_fade_end_meters, water_.waveFadeEndMeters, 100.0F, 100'000.0F)

void WorldVisualSettings::set_default_debug_view(const std::int64_t value) {
    default_debug_view_ = std::clamp<std::int64_t>(value, 0, 13);
    emit_changed();
}

std::int64_t WorldVisualSettings::get_default_debug_view() const noexcept {
    return default_debug_view_;
}

#undef TERRAIN_FLOAT_SETTING
#undef TERRAIN_INT_SETTING

} // namespace terrain::godot_adapter
