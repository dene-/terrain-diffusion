#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace terrain {

struct TerrainMetricConfig final {
    float metersPerWorldUnit{1.0F};
    float elevationMetersPerSourceUnit{1.0F};
    float verticalExaggeration{1.0F};
};

struct TerrainVisualConfig final {
    float rockSlopeStart{0.20F};
    float rockSlopeFull{0.58F};
    float screeSlopeStart{0.12F};
    float screeSlopeFull{0.42F};
    float macroTextureScaleMeters{1'200.0F};
    float detailFadeStartMeters{1'200.0F};
    float detailFadeEndMeters{18'000.0F};
    float maxScreenSpaceErrorPixels{2.4F};
    float lodHysteresis{0.18F};
};

struct ForestVisualConfig final {
    float globalDensity{0.88F};
    float clearingScaleMeters{420.0F};
    float clearingStrength{0.72F};
    float maximumSlope{0.48F};
    float minimumTemperature{-8.0F};
    float treeLineStartMeters{2'150.0F};
    float treeLineEndMeters{3'450.0F};
    float precipitationResponse{1.0F};
    float billboardSinkMeters{0.12F};
    float billboardSizeMultiplier{1.0F};
    float nearPixelThreshold{18.0F};
    float midPixelThreshold{4.0F};
    float geometryCullPixelThreshold{1.35F};
    float nearShadowDistanceMeters{1'400.0F};
};

struct SnowVisualConfig final {
    float temperatureStartC{1.5F};
    float temperatureFullC{-7.0F};
    float slopeRetentionStart{0.10F};
    float slopeRetentionEnd{0.62F};
    float aspectStrength{0.22F};
};

struct AtmosphereVisualConfig final {
    float distanceDensity{0.000085F};
    float heightFalloff{0.000118F};
    float baseHeightMeters{0.0F};
    float aerialPerspectiveStrength{1.0F};
    float turbidity{2.4F};
};

struct LightingVisualConfig final {
    float sunAzimuthDegrees{132.0F};
    float sunElevationDegrees{46.0F};
    float sunIntensity{1.08F};
    float skyIntensity{0.68F};
    float exposure{0.90F};
};

struct CloudVisualConfig final {
    float coverage{0.48F};
    float density{0.46F};
    float baseHeightMeters{1'850.0F};
    float topHeightMeters{5'400.0F};
    float weatherScaleMeters{24'000.0F};
    float detailScaleMeters{3'600.0F};
    float windMetresPerSecond{11.0F};
    std::uint32_t primarySteps{10};
    std::uint32_t lightSteps{2};
};

struct StreamingBudgetConfig final {
    std::size_t maxConcurrentRequests{1};
    std::size_t maxDecodeJobs{1};
    std::size_t maxDerivedMapJobs{1};
    std::size_t maxVegetationJobs{1};
    std::size_t maxGpuUploadsPerFrame{2};
    std::size_t maxGpuUploadBytesPerFrame{32U * 1024U * 1024U};
    std::size_t terrainMemoryMegabytes{768};
    std::size_t vegetationMemoryMegabytes{384};
    // Bounds GPU-resident terrain tiles retained for instant revisits. CPU
    // render payloads are released after upload; this limits the GPU cache.
    std::size_t maxResidentTiles{512};
};

struct WorldVisualConfig final {
    TerrainMetricConfig metrics;
    TerrainVisualConfig terrain;
    ForestVisualConfig forest;
    SnowVisualConfig snow;
    AtmosphereVisualConfig atmosphere;
    LightingVisualConfig lighting;
    CloudVisualConfig clouds;
    StreamingBudgetConfig streaming;

    [[nodiscard]] static WorldVisualConfig load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
};

} // namespace terrain
