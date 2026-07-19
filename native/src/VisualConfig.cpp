#include "VisualConfig.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <cmath>
#include <stdexcept>
#include <string>

namespace terrain {
namespace {

template <typename T>
void read(const nlohmann::json& object, const char* key, T& value) {
    if (object.contains(key)) value = object.at(key).get<T>();
}

[[nodiscard]] nlohmann::json parseFile(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) throw std::runtime_error("Could not open visual configuration: " + path.string());
    try {
        return nlohmann::json::parse(stream);
    } catch (const std::exception& error) {
        throw std::runtime_error("Invalid visual configuration " + path.string() + ": " + error.what());
    }
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string{"Invalid visual configuration: "} + message);
}

void requireUnit(float value, const char* name) {
    require(std::isfinite(value) && value >= 0.0F && value <= 1.0F, name);
}

} // namespace

WorldVisualConfig WorldVisualConfig::load(const std::filesystem::path& path) {
    auto result = WorldVisualConfig{};
    const auto root = parseFile(path);
    if (const auto iterator = root.find("metrics"); iterator != root.end()) {
        read(*iterator, "metersPerWorldUnit", result.metrics.metersPerWorldUnit);
        read(*iterator, "elevationMetersPerSourceUnit", result.metrics.elevationMetersPerSourceUnit);
        read(*iterator, "verticalExaggeration", result.metrics.verticalExaggeration);
    }
    if (const auto iterator = root.find("terrain"); iterator != root.end()) {
        read(*iterator, "rockSlopeStart", result.terrain.rockSlopeStart);
        read(*iterator, "rockSlopeFull", result.terrain.rockSlopeFull);
        read(*iterator, "screeSlopeStart", result.terrain.screeSlopeStart);
        read(*iterator, "screeSlopeFull", result.terrain.screeSlopeFull);
        read(*iterator, "macroTextureScaleMeters", result.terrain.macroTextureScaleMeters);
        read(*iterator, "detailFadeStartMeters", result.terrain.detailFadeStartMeters);
        read(*iterator, "detailFadeEndMeters", result.terrain.detailFadeEndMeters);
        read(*iterator, "maxScreenSpaceErrorPixels", result.terrain.maxScreenSpaceErrorPixels);
        read(*iterator, "lodHysteresis", result.terrain.lodHysteresis);
    }
    if (const auto iterator = root.find("forest"); iterator != root.end()) {
        read(*iterator, "globalDensity", result.forest.globalDensity);
        read(*iterator, "clearingScaleMeters", result.forest.clearingScaleMeters);
        read(*iterator, "clearingStrength", result.forest.clearingStrength);
        read(*iterator, "maximumSlope", result.forest.maximumSlope);
        read(*iterator, "minimumTemperature", result.forest.minimumTemperature);
        read(*iterator, "treeLineStartMeters", result.forest.treeLineStartMeters);
        read(*iterator, "treeLineEndMeters", result.forest.treeLineEndMeters);
        read(*iterator, "precipitationResponse", result.forest.precipitationResponse);
        read(*iterator, "billboardSinkMeters", result.forest.billboardSinkMeters);
        read(*iterator, "billboardSizeMultiplier", result.forest.billboardSizeMultiplier);
        read(*iterator, "nearPixelThreshold", result.forest.nearPixelThreshold);
        read(*iterator, "midPixelThreshold", result.forest.midPixelThreshold);
        read(*iterator, "geometryCullPixelThreshold", result.forest.geometryCullPixelThreshold);
        read(*iterator, "nearShadowDistanceMeters", result.forest.nearShadowDistanceMeters);
    }
    if (const auto iterator = root.find("snow"); iterator != root.end()) {
        read(*iterator, "temperatureStartC", result.snow.temperatureStartC);
        read(*iterator, "temperatureFullC", result.snow.temperatureFullC);
        read(*iterator, "slopeRetentionStart", result.snow.slopeRetentionStart);
        read(*iterator, "slopeRetentionEnd", result.snow.slopeRetentionEnd);
        read(*iterator, "aspectStrength", result.snow.aspectStrength);
    }
    if (const auto iterator = root.find("atmosphere"); iterator != root.end()) {
        read(*iterator, "distanceDensity", result.atmosphere.distanceDensity);
        read(*iterator, "heightFalloff", result.atmosphere.heightFalloff);
        read(*iterator, "baseHeightMeters", result.atmosphere.baseHeightMeters);
        read(*iterator, "aerialPerspectiveStrength", result.atmosphere.aerialPerspectiveStrength);
        read(*iterator, "turbidity", result.atmosphere.turbidity);
    }
    if (const auto iterator = root.find("lighting"); iterator != root.end()) {
        read(*iterator, "sunAzimuthDegrees", result.lighting.sunAzimuthDegrees);
        read(*iterator, "sunElevationDegrees", result.lighting.sunElevationDegrees);
        read(*iterator, "sunIntensity", result.lighting.sunIntensity);
        read(*iterator, "skyIntensity", result.lighting.skyIntensity);
        read(*iterator, "exposure", result.lighting.exposure);
    }
    if (const auto iterator = root.find("clouds"); iterator != root.end()) {
        read(*iterator, "coverage", result.clouds.coverage);
        read(*iterator, "density", result.clouds.density);
        read(*iterator, "baseHeightMeters", result.clouds.baseHeightMeters);
        read(*iterator, "topHeightMeters", result.clouds.topHeightMeters);
        read(*iterator, "weatherScaleMeters", result.clouds.weatherScaleMeters);
        read(*iterator, "detailScaleMeters", result.clouds.detailScaleMeters);
        read(*iterator, "windMetresPerSecond", result.clouds.windMetresPerSecond);
        read(*iterator, "primarySteps", result.clouds.primarySteps);
        read(*iterator, "lightSteps", result.clouds.lightSteps);
    }
    if (const auto iterator = root.find("streaming"); iterator != root.end()) {
        read(*iterator, "maxConcurrentRequests", result.streaming.maxConcurrentRequests);
        read(*iterator, "maxDecodeJobs", result.streaming.maxDecodeJobs);
        read(*iterator, "maxDerivedMapJobs", result.streaming.maxDerivedMapJobs);
        read(*iterator, "maxVegetationJobs", result.streaming.maxVegetationJobs);
        read(*iterator, "maxGpuUploadsPerFrame", result.streaming.maxGpuUploadsPerFrame);
        read(*iterator, "maxGpuUploadBytesPerFrame", result.streaming.maxGpuUploadBytesPerFrame);
        read(*iterator, "terrainMemoryMB", result.streaming.terrainMemoryMegabytes);
        read(*iterator, "vegetationMemoryMB", result.streaming.vegetationMemoryMegabytes);
        read(*iterator, "maxResidentTiles", result.streaming.maxResidentTiles);
    }
    if (result.metrics.metersPerWorldUnit <= 0.0F || result.metrics.elevationMetersPerSourceUnit <= 0.0F
        || result.metrics.verticalExaggeration <= 0.0F) {
        throw std::runtime_error("Visual metric scales must be positive");
    }
    requireUnit(result.terrain.rockSlopeStart, "rockSlopeStart must be in 0..1");
    requireUnit(result.terrain.rockSlopeFull, "rockSlopeFull must be in 0..1");
    requireUnit(result.terrain.screeSlopeStart, "screeSlopeStart must be in 0..1");
    requireUnit(result.terrain.screeSlopeFull, "screeSlopeFull must be in 0..1");
    require(result.terrain.rockSlopeStart < result.terrain.rockSlopeFull,
        "rockSlopeStart must be less than rockSlopeFull");
    require(result.terrain.screeSlopeStart < result.terrain.screeSlopeFull,
        "screeSlopeStart must be less than screeSlopeFull");
    require(result.terrain.macroTextureScaleMeters > 0.0F
        && result.terrain.detailFadeStartMeters >= 0.0F
        && result.terrain.detailFadeStartMeters < result.terrain.detailFadeEndMeters,
        "terrain texture scales and fade range must be positive and ordered");
    requireUnit(result.terrain.lodHysteresis, "lodHysteresis must be in 0..1");
    requireUnit(result.forest.globalDensity, "forest globalDensity must be in 0..1");
    requireUnit(result.forest.clearingStrength, "forest clearingStrength must be in 0..1");
    requireUnit(result.forest.maximumSlope, "forest maximumSlope must be in 0..1");
    require(result.forest.treeLineStartMeters >= 0.0F
        && result.forest.treeLineStartMeters < result.forest.treeLineEndMeters,
        "forest tree-line elevations must be non-negative and ordered");
    require(result.forest.clearingScaleMeters > 0.0F && result.forest.billboardSizeMultiplier > 0.0F
        && result.forest.geometryCullPixelThreshold > 0.0F
        && result.forest.nearShadowDistanceMeters >= 0.0F,
        "forest physical scales and pixel threshold must be positive");
    require(result.snow.temperatureFullC < result.snow.temperatureStartC,
        "snow temperatureFullC must be colder than temperatureStartC");
    requireUnit(result.snow.slopeRetentionStart, "snow slopeRetentionStart must be in 0..1");
    requireUnit(result.snow.slopeRetentionEnd, "snow slopeRetentionEnd must be in 0..1");
    require(result.snow.slopeRetentionStart < result.snow.slopeRetentionEnd,
        "snow slope retention range must be ordered");
    require(result.atmosphere.distanceDensity >= 0.0F && result.atmosphere.heightFalloff >= 0.0F
        && result.atmosphere.aerialPerspectiveStrength >= 0.0F,
        "atmosphere densities must not be negative");
    require(result.lighting.sunElevationDegrees >= -90.0F && result.lighting.sunElevationDegrees <= 90.0F
        && result.lighting.sunIntensity >= 0.0F && result.lighting.skyIntensity >= 0.0F
        && result.lighting.exposure > 0.0F,
        "lighting values are outside their physical range");
    requireUnit(result.clouds.coverage, "cloud coverage must be in 0..1");
    requireUnit(result.clouds.density, "cloud density must be in 0..1");
    require(result.clouds.baseHeightMeters < result.clouds.topHeightMeters
        && result.clouds.weatherScaleMeters > 0.0F && result.clouds.detailScaleMeters > 0.0F,
        "cloud layer and noise scales must be positive and ordered");
    require(result.clouds.primarySteps >= 6U && result.clouds.primarySteps <= 24U
        && result.clouds.lightSteps <= 8U,
        "cloud primarySteps must be 6..24 and lightSteps at most 8");
    require(result.streaming.maxConcurrentRequests > 0U && result.streaming.maxDecodeJobs > 0U
        && result.streaming.maxDerivedMapJobs > 0U && result.streaming.maxVegetationJobs > 0U
        && result.streaming.maxGpuUploadsPerFrame > 0U && result.streaming.maxGpuUploadBytesPerFrame > 0U
        && result.streaming.maxResidentTiles > 0U,
        "streaming budgets must be non-zero");
    return result;
}

void WorldVisualConfig::save(const std::filesystem::path& path) const {
    const nlohmann::json root{
        {"metrics", {{"metersPerWorldUnit", metrics.metersPerWorldUnit}, {"elevationMetersPerSourceUnit", metrics.elevationMetersPerSourceUnit}, {"verticalExaggeration", metrics.verticalExaggeration}}},
        {"terrain", {{"rockSlopeStart", terrain.rockSlopeStart}, {"rockSlopeFull", terrain.rockSlopeFull}, {"screeSlopeStart", terrain.screeSlopeStart}, {"screeSlopeFull", terrain.screeSlopeFull}, {"macroTextureScaleMeters", terrain.macroTextureScaleMeters}, {"detailFadeStartMeters", terrain.detailFadeStartMeters}, {"detailFadeEndMeters", terrain.detailFadeEndMeters}, {"maxScreenSpaceErrorPixels", terrain.maxScreenSpaceErrorPixels}, {"lodHysteresis", terrain.lodHysteresis}}},
        {"forest", {{"globalDensity", forest.globalDensity}, {"clearingScaleMeters", forest.clearingScaleMeters}, {"clearingStrength", forest.clearingStrength}, {"maximumSlope", forest.maximumSlope}, {"minimumTemperature", forest.minimumTemperature}, {"treeLineStartMeters", forest.treeLineStartMeters}, {"treeLineEndMeters", forest.treeLineEndMeters}, {"precipitationResponse", forest.precipitationResponse}, {"billboardSinkMeters", forest.billboardSinkMeters}, {"billboardSizeMultiplier", forest.billboardSizeMultiplier}, {"nearPixelThreshold", forest.nearPixelThreshold}, {"midPixelThreshold", forest.midPixelThreshold}, {"geometryCullPixelThreshold", forest.geometryCullPixelThreshold}, {"nearShadowDistanceMeters", forest.nearShadowDistanceMeters}}},
        {"snow", {{"temperatureStartC", snow.temperatureStartC}, {"temperatureFullC", snow.temperatureFullC}, {"slopeRetentionStart", snow.slopeRetentionStart}, {"slopeRetentionEnd", snow.slopeRetentionEnd}, {"aspectStrength", snow.aspectStrength}}},
        {"atmosphere", {{"distanceDensity", atmosphere.distanceDensity}, {"heightFalloff", atmosphere.heightFalloff}, {"baseHeightMeters", atmosphere.baseHeightMeters}, {"aerialPerspectiveStrength", atmosphere.aerialPerspectiveStrength}, {"turbidity", atmosphere.turbidity}}},
        {"lighting", {{"sunAzimuthDegrees", lighting.sunAzimuthDegrees}, {"sunElevationDegrees", lighting.sunElevationDegrees}, {"sunIntensity", lighting.sunIntensity}, {"skyIntensity", lighting.skyIntensity}, {"exposure", lighting.exposure}}},
        {"clouds", {{"coverage", clouds.coverage}, {"density", clouds.density}, {"baseHeightMeters", clouds.baseHeightMeters}, {"topHeightMeters", clouds.topHeightMeters}, {"weatherScaleMeters", clouds.weatherScaleMeters}, {"detailScaleMeters", clouds.detailScaleMeters}, {"windMetresPerSecond", clouds.windMetresPerSecond}, {"primarySteps", clouds.primarySteps}, {"lightSteps", clouds.lightSteps}}},
        {"streaming", {{"maxConcurrentRequests", streaming.maxConcurrentRequests}, {"maxDecodeJobs", streaming.maxDecodeJobs}, {"maxDerivedMapJobs", streaming.maxDerivedMapJobs}, {"maxVegetationJobs", streaming.maxVegetationJobs}, {"maxGpuUploadsPerFrame", streaming.maxGpuUploadsPerFrame}, {"maxGpuUploadBytesPerFrame", streaming.maxGpuUploadBytesPerFrame}, {"terrainMemoryMB", streaming.terrainMemoryMegabytes}, {"vegetationMemoryMB", streaming.vegetationMemoryMegabytes}, {"maxResidentTiles", streaming.maxResidentTiles}}},
    };
    std::ofstream stream{path};
    if (!stream) throw std::runtime_error("Could not write visual configuration: " + path.string());
    stream << root.dump(2) << '\n';
}

} // namespace terrain
