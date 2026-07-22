#include <terrain/TerrainMesh.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <iterator>
#include <numbers>
#include <utility>

namespace terrain {
namespace {

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] float random01(std::int64_t x, std::int64_t z, std::uint64_t seed) noexcept {
    auto value = seed;
    value ^= mix64(static_cast<std::uint64_t>(x));
    value ^= mix64(static_cast<std::uint64_t>(z) + 0x9e3779b97f4a7c15ULL);
    return static_cast<float>(mix64(value) >> 40U) / static_cast<float>(1U << 24U);
}

[[nodiscard]] float smoothNoise(double x, double z, std::uint64_t seed) noexcept {
    const auto ix = static_cast<std::int64_t>(std::floor(x));
    const auto iz = static_cast<std::int64_t>(std::floor(z));
    const auto fx = static_cast<float>(x - static_cast<double>(ix));
    const auto fz = static_cast<float>(z - static_cast<double>(iz));
    const auto sx = fx * fx * (3.0F - 2.0F * fx);
    const auto sz = fz * fz * (3.0F - 2.0F * fz);
    const auto a = glm::mix(random01(ix, iz, seed), random01(ix + 1, iz, seed), sx);
    const auto b = glm::mix(random01(ix, iz + 1, seed), random01(ix + 1, iz + 1, seed), sx);
    return glm::mix(a, b, sz);
}

[[nodiscard]] float fractalNoise(double x, double z, std::uint64_t seed) noexcept {
    float value = 0.0F;
    float amplitude = 0.58F;
    double frequency = 1.0;
    for (std::uint32_t octave = 0; octave < 4; ++octave) {
        value += smoothNoise(x * frequency, z * frequency, seed + octave * 0x9e3779b9U) * amplitude;
        frequency *= 2.03;
        amplitude *= 0.48F;
    }
    return value;
}

[[nodiscard]] std::uint32_t packColor(const glm::vec3 color, float alpha = 1.0F) noexcept {
    const auto converted = glm::clamp(glm::round(glm::vec4{color, alpha} * 255.0F), 0.0F, 255.0F);
    return static_cast<std::uint32_t>(converted.r)
        | (static_cast<std::uint32_t>(converted.g) << 8U)
        | (static_cast<std::uint32_t>(converted.b) << 16U)
        | (static_cast<std::uint32_t>(converted.a) << 24U);
}

[[nodiscard]] std::uint32_t packChannels(const glm::vec4 channels) noexcept {
    const auto converted = glm::clamp(glm::round(channels * 255.0F), 0.0F, 255.0F);
    return static_cast<std::uint32_t>(converted.r)
        | (static_cast<std::uint32_t>(converted.g) << 8U)
        | (static_cast<std::uint32_t>(converted.b) << 16U)
        | (static_cast<std::uint32_t>(converted.a) << 24U);
}

[[nodiscard]] std::int32_t visibleWidth(const RawTile& tile) noexcept {
    return tile.width - tile.borderSamples * 2;
}

[[nodiscard]] std::int32_t visibleHeight(const RawTile& tile) noexcept {
    return tile.height - tile.borderSamples * 2;
}

[[nodiscard]] std::size_t rawIndex(const RawTile& tile, std::int32_t x, std::int32_t z) noexcept {
    const auto border = tile.borderSamples;
    x = std::clamp(x, -border, visibleWidth(tile) - 1 + border);
    z = std::clamp(z, -border, visibleHeight(tile) - 1 + border);
    return static_cast<std::size_t>((z + border) * tile.width + x + border);
}

[[nodiscard]] float sourceElevationAt(const RawTile& tile, std::int32_t x, std::int32_t z) noexcept {
    const auto index = rawIndex(tile, x, z);
    return tile.renderElevations.size() == tile.elevations.size()
        ? tile.renderElevations[index]
        : static_cast<float>(tile.elevations[index]);
}

[[nodiscard]] bool smoothFinestElevation(const RawTile& tile) noexcept {
    return tile.spec.source == TileSource::Detailed && tile.spec.stride == 1;
}

[[nodiscard]] float elevationAt(const RawTile& tile, std::int32_t x, std::int32_t z) noexcept {
    if (!smoothFinestElevation(tile)) return sourceElevationAt(tile, x, z);

    // Terrain Diffusion elevations ultimately originate as whole-metre int16
    // samples. Local interpolation preserves sub-metre positions between
    // those samples, but the source contours remain visible on gentle ground.
    // A separable 1-2-1 reconstruction yields smooth render heights while
    // remaining a bounded weighted average of the authoritative field.
    // Global border samples make the reconstruction identical on both sides
    // of every L0 tile edge.
    constexpr std::array<float, 3> weights{1.0F, 2.0F, 1.0F};
    float result{};
    for (std::int32_t dz = -1; dz <= 1; ++dz) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            result += sourceElevationAt(tile, x + dx, z + dz)
                * weights[static_cast<std::size_t>(dx + 1)]
                * weights[static_cast<std::size_t>(dz + 1)];
        }
    }
    return result * (1.0F / 16.0F);
}

[[nodiscard]] float climateAt(
    const RawTile& tile,
    std::int32_t x,
    std::int32_t z,
    std::size_t channel,
    float fallback) noexcept {
    const auto channels = static_cast<std::size_t>(std::max(tile.climateChannels, 0));
    if (channel >= channels) return fallback;
    const auto index = rawIndex(tile, x, z) * channels + channel;
    return index < tile.climate.size() ? tile.climate[index] : fallback;
}

[[nodiscard]] float bilinearElevation(const RawTile& tile, double worldX, double worldZ) noexcept {
    const auto sampleX = (worldX - tile.origin.x) / tile.spec.spacing;
    const auto sampleZ = (worldZ - tile.origin.y) / tile.spec.spacing;
    const auto x0 = std::clamp(static_cast<std::int32_t>(std::floor(sampleX)), 0, visibleWidth(tile) - 2);
    const auto z0 = std::clamp(static_cast<std::int32_t>(std::floor(sampleZ)), 0, visibleHeight(tile) - 2);
    const auto tx = static_cast<float>(std::clamp(sampleX - x0, 0.0, 1.0));
    const auto tz = static_cast<float>(std::clamp(sampleZ - z0, 0.0, 1.0));
    const auto a = glm::mix(elevationAt(tile, x0, z0), elevationAt(tile, x0 + 1, z0), tx);
    const auto b = glm::mix(elevationAt(tile, x0, z0 + 1), elevationAt(tile, x0 + 1, z0 + 1), tx);
    return glm::mix(a, b, tz);
}

[[nodiscard]] glm::vec3 normalAt(
    const RawTile& tile,
    std::int32_t x,
    std::int32_t z,
    const TerrainMetricConfig& metrics) noexcept {
    const auto spacingMetres = static_cast<float>(tile.spec.spacing) * metrics.metersPerWorldUnit;
    const auto elevationScale = metrics.elevationMetersPerSourceUnit * metrics.verticalExaggeration;
    // Average the derivative across the perpendicular axis and use a wide
    // baseline at every detailed LOD. A one-row central difference faithfully
    // exposed the native field's metre quantization as parallel lighting
    // bands even though the reconstructed surface was continuous.
    constexpr std::int32_t radius = 2;
    float dx{};
    float dz{};
    float totalWeight{};
    for (std::int32_t offset = -radius; offset <= radius; ++offset) {
        const auto weight = static_cast<float>(radius + 1 - std::abs(offset));
        dx += (elevationAt(tile, x + radius, z + offset)
            - elevationAt(tile, x - radius, z + offset)) * weight;
        dz += (elevationAt(tile, x + offset, z + radius)
            - elevationAt(tile, x + offset, z - radius)) * weight;
        totalWeight += weight;
    }
    dx = dx / totalWeight * elevationScale;
    dz = dz / totalWeight * elevationScale;
    return glm::normalize(glm::vec3{
        -dx, spacingMetres * static_cast<float>(radius * 2), -dz});
}

struct TerrainFields final {
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    glm::vec3 baseColor{0.35F, 0.42F, 0.24F};
    float slope{};
    float signedCurvature{0.5F};
    float wetness{};
    float macroVariation{0.5F};
    float forest{};
    float rock{};
    float scree{};
    float snow{};
};

void partitionVegetation(
    std::vector<VegetationInstance>& instances,
    std::vector<VegetationRange>& ranges,
    double tileWorldSize,
    float chunkSize,
    float boundsPadding) {
    if (instances.empty()) return;
    const auto chunksPerSide = std::max(1, static_cast<int>(std::ceil(tileWorldSize / chunkSize)));
    std::vector<std::vector<VegetationInstance>> chunks(
        static_cast<std::size_t>(chunksPerSide * chunksPerSide));
    for (auto& instance : instances) {
        const auto chunkX = std::clamp(static_cast<int>(instance.position.x / chunkSize), 0, chunksPerSide - 1);
        const auto chunkZ = std::clamp(static_cast<int>(instance.position.z / chunkSize), 0, chunksPerSide - 1);
        chunks[static_cast<std::size_t>(chunkZ * chunksPerSide + chunkX)].push_back(std::move(instance));
    }
    instances.clear();
    std::size_t totalCount{};
    for (const auto& chunk : chunks) totalCount += chunk.size();
    instances.reserve(totalCount);
    for (int chunkZ = 0; chunkZ < chunksPerSide; ++chunkZ) {
        for (int chunkX = 0; chunkX < chunksPerSide; ++chunkX) {
            auto& chunk = chunks[static_cast<std::size_t>(chunkZ * chunksPerSide + chunkX)];
            if (chunk.empty()) continue;
            const auto offset = static_cast<std::uint32_t>(instances.size());
            instances.insert(instances.end(), std::make_move_iterator(chunk.begin()), std::make_move_iterator(chunk.end()));
            const auto extentX = static_cast<float>(std::min(
                tileWorldSize - static_cast<double>(chunkX) * chunkSize, static_cast<double>(chunkSize)));
            const auto extentZ = static_cast<float>(std::min(
                tileWorldSize - static_cast<double>(chunkZ) * chunkSize, static_cast<double>(chunkSize)));
            ranges.push_back(VegetationRange{
                .offset = offset,
                .count = static_cast<std::uint32_t>(chunk.size()),
                .center = glm::vec2{
                    static_cast<float>(chunkX) * chunkSize + extentX * 0.5F,
                    static_cast<float>(chunkZ) * chunkSize + extentZ * 0.5F,
                },
                .radius = std::hypot(extentX, extentZ) * 0.5F + boundsPadding,
            });
        }
    }
}

[[nodiscard]] TerrainFields deriveTerrainFields(
    const RawTile& tile,
    std::int32_t x,
    std::int32_t z,
    double worldX,
    double worldZ,
    std::uint64_t seed,
    const WorldVisualConfig& visuals) noexcept {
    constexpr glm::vec3 beach{0.58F, 0.53F, 0.40F};
    constexpr glm::vec3 dryGrass{0.44F, 0.46F, 0.25F};
    constexpr glm::vec3 wetGrass{0.20F, 0.37F, 0.16F};
    constexpr glm::vec3 wetSoil{0.20F, 0.19F, 0.14F};
    constexpr glm::vec3 rockColor{0.33F, 0.34F, 0.34F};
    constexpr glm::vec3 screeColor{0.39F, 0.37F, 0.33F};
    constexpr glm::vec3 snow{0.84F, 0.87F, 0.88F};

    TerrainFields result;
    const auto elevation = elevationAt(tile, x, z) * visuals.metrics.elevationMetersPerSourceUnit;
    result.normal = normalAt(tile, x, z, visuals.metrics);
    result.slope = 1.0F - std::clamp(result.normal.y, 0.0F, 1.0F);
    const auto spacing = std::max(1.0F, static_cast<float>(tile.spec.spacing));
    const auto center = elevationAt(tile, x, z);
    const auto localAverage = (elevationAt(tile, x - 1, z) + elevationAt(tile, x + 1, z)
        + elevationAt(tile, x, z - 1) + elevationAt(tile, x, z + 1)) * 0.25F;
    const auto macroAverage = (elevationAt(tile, x - 2, z) + elevationAt(tile, x + 2, z)
        + elevationAt(tile, x, z - 2) + elevationAt(tile, x, z + 2)) * 0.25F;
    // Curvature drives wet soil, vegetation and broad form shading, so it must
    // not amplify the source field's one-metre elevation quantization. Measure
    // it over a physical baseline large enough that isolated integer steps are
    // treated as noise while real gullies and ridges remain visible.
    const auto localCurvature = (localAverage - center) / std::max(spacing * 2.5F, 1.0F);
    const auto macroCurvature = (macroAverage - center) / std::max(spacing * 5.0F, 1.0F);
    result.signedCurvature = 0.5F + 0.5F * std::tanh(localCurvature + macroCurvature * 0.7F);

    const auto temperature = climateAt(tile, x, z, 0, 13.0F);
    const auto precipitation = std::max(0.0F, climateAt(tile, x, z, 2, 850.0F));
    const auto precipitationSeasonality = std::max(0.0F, climateAt(tile, x, z, 3, 35.0F));
    const auto detailedSoilMoisture = climateAt(tile, x, z, 4, -1.0F);
    const auto detailedDrainage = climateAt(tile, x, z, 5, 0.5F);
    const auto detailedExposure = climateAt(tile, x, z, 6, 0.5F);
    const auto detailedRockiness = climateAt(tile, x, z, 7, 0.0F);
    const auto moisture = glm::smoothstep(180.0F, 1'600.0F, precipitation);
    const auto groundWarmth = glm::smoothstep(-8.0F, 22.0F, temperature);
    const auto concavity = glm::smoothstep(0.46F, 0.82F, result.signedCurvature);
    const auto ridgeExposure = 1.0F - glm::smoothstep(0.16F, 0.46F, result.signedCurvature);
    const auto retention = 1.0F - glm::smoothstep(0.10F, 0.58F, result.slope);
    const auto macroWetness = moisture * (0.42F + concavity * 0.48F)
        * (0.48F + retention * 0.52F)
        * (1.0F - 0.34F * std::clamp(precipitationSeasonality / 100.0F, 0.0F, 1.0F));
    result.wetness = detailedSoilMoisture >= 0.0F
        ? std::clamp(detailedSoilMoisture
            * (0.76F + concavity * 0.34F)
            * (1.0F - std::clamp(detailedDrainage, 0.0F, 1.0F) * 0.16F), 0.0F, 1.0F)
        : std::clamp(macroWetness, 0.0F, 1.0F);
    result.macroVariation = std::clamp(fractalNoise(
        worldX / visuals.terrain.macroTextureScaleMeters,
        worldZ / visuals.terrain.macroTextureScaleMeters,
        seed ^ 0xc4ceb9fe1a85ec53ULL), 0.0F, 1.0F);

    result.rock = glm::smoothstep(visuals.terrain.rockSlopeStart, visuals.terrain.rockSlopeFull, result.slope);
    result.rock = std::clamp(result.rock + glm::smoothstep(2'100.0F, 3'650.0F, elevation) * 0.48F
        + ridgeExposure * result.slope * 0.28F - concavity * 0.18F
        + detailedRockiness * 0.42F, 0.0F, 1.0F);
    const auto mediumSlope = glm::smoothstep(visuals.terrain.screeSlopeStart,
        visuals.terrain.screeSlopeFull, result.slope)
        * (1.0F - glm::smoothstep(visuals.terrain.screeSlopeFull, 0.72F, result.slope));
    result.scree = std::clamp(mediumSlope
        * (0.38F + result.rock * 0.45F + concavity * 0.42F
            + detailedRockiness * 0.28F), 0.0F, 1.0F);

    const auto coldness = 1.0F - glm::smoothstep(
        visuals.snow.temperatureFullC, visuals.snow.temperatureStartC, temperature);
    const auto elevationSnow = glm::smoothstep(2'100.0F, 3'700.0F, elevation);
    const auto slopeRetention = 1.0F - glm::smoothstep(
        visuals.snow.slopeRetentionStart, visuals.snow.slopeRetentionEnd, result.slope);
    const auto northAspect = std::clamp(result.normal.z * 0.5F + 0.5F, 0.0F, 1.0F);
    result.snow = std::clamp(std::max(coldness, elevationSnow * 0.72F) * slopeRetention
        * (0.78F + northAspect * visuals.snow.aspectStrength) * (0.78F + concavity * 0.22F)
        * (1.0F - std::clamp(detailedExposure, 0.0F, 1.0F) * 0.14F), 0.0F, 1.0F);

    const auto forestWarmth = glm::smoothstep(visuals.forest.minimumTemperature, 8.0F, temperature)
        * (1.0F - glm::smoothstep(34.0F, 46.0F, temperature));
    const auto fieldScale = tile.spec.source == TileSource::Orbital ? 85'000.0 : 1'650.0;
    const auto stand = fractalNoise(worldX / fieldScale, worldZ / fieldScale,
        seed ^ 0x243f6a8885a308d3ULL);
    const auto climate = forestWarmth * moisture;
    result.forest = glm::smoothstep(0.28F, 0.76F, climate * 0.64F + result.wetness * 0.25F + stand * 0.46F)
        * (1.0F - glm::smoothstep(visuals.forest.maximumSlope * 0.45F, visuals.forest.maximumSlope, result.slope))
        * (1.0F - glm::smoothstep(
            visuals.forest.treeLineStartMeters, visuals.forest.treeLineEndMeters, elevation))
        * (1.0F - result.rock) * (1.0F - result.scree * 0.72F) * (1.0F - result.snow)
        * (1.0F - std::clamp(detailedExposure, 0.0F, 1.0F) * 0.30F)
        * (1.0F - std::clamp(detailedRockiness, 0.0F, 1.0F) * 0.48F)
        * visuals.forest.globalDensity;
    result.forest = std::clamp(result.forest, 0.0F, 1.0F);

    result.baseColor = glm::mix(dryGrass, wetGrass, std::clamp(result.wetness * 0.82F + groundWarmth * 0.10F, 0.0F, 1.0F));
    result.baseColor = glm::mix(result.baseColor, wetSoil, result.wetness * concavity * 0.28F);
    if (elevation < 7.0F) result.baseColor = glm::mix(beach, result.baseColor, glm::smoothstep(-2.0F, 7.0F, elevation));
    result.baseColor = glm::mix(result.baseColor, screeColor, result.scree * (1.0F - result.rock));
    result.baseColor = glm::mix(result.baseColor, rockColor, result.rock);
    result.baseColor = glm::mix(result.baseColor, snow, result.snow);
    result.baseColor *= 0.86F + result.macroVariation * 0.24F;
    return result;
}

void appendSkirts(TileMesh& mesh) {
    const auto width = visibleWidth(mesh.raw);
    const auto height = visibleHeight(mesh.raw);
    const auto skirtDepth = static_cast<float>(std::max(18.0, mesh.raw.spec.spacing * 0.42));
    std::vector<std::uint32_t> perimeter;
    perimeter.reserve(static_cast<std::size_t>((width + height) * 2 - 4));
    for (std::int32_t x = 0; x < width; ++x) perimeter.push_back(static_cast<std::uint32_t>(x));
    for (std::int32_t z = 1; z < height; ++z) perimeter.push_back(static_cast<std::uint32_t>(z * width + width - 1));
    for (std::int32_t x = width - 2; x >= 0; --x) perimeter.push_back(static_cast<std::uint32_t>((height - 1) * width + x));
    for (std::int32_t z = height - 2; z > 0; --z) perimeter.push_back(static_cast<std::uint32_t>(z * width));

    const auto skirtStart = static_cast<std::uint32_t>(mesh.vertices.size());
    for (const auto source : perimeter) {
        auto vertex = mesh.vertices[source];
        vertex.position.y -= skirtDepth;
        mesh.vertices.push_back(vertex);
    }
    for (std::uint32_t index = 0; index < perimeter.size(); ++index) {
        const auto next = (index + 1U) % static_cast<std::uint32_t>(perimeter.size());
        const auto a = perimeter[index];
        const auto b = perimeter[next];
        const auto c = skirtStart + index;
        const auto d = skirtStart + next;
        mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
    }
}

void appendVegetation(TileMesh& mesh, std::uint64_t seed, const WorldVisualConfig& visuals) {
    if (!mesh.raw.spec.vegetation || mesh.raw.climate.empty()) return;

    const auto& tile = mesh.raw;
    // L0 uses individual trees; L1 carries a cheaper but still data-backed
    // forest layer so detailed terrain does not become bare beyond 1.5 km.
    const double treeCell = tile.spec.level == 0 ? 10.5 : 21.0;
    const auto startCellX = static_cast<std::int64_t>(std::floor(tile.origin.x / treeCell));
    const auto startCellZ = static_cast<std::int64_t>(std::floor(tile.origin.y / treeCell));
    const auto endCellX = static_cast<std::int64_t>(std::ceil((tile.origin.x + tile.spec.tileWorldSize) / treeCell));
    const auto endCellZ = static_cast<std::int64_t>(std::ceil((tile.origin.y + tile.spec.tileWorldSize) / treeCell));

    for (auto cellZ = startCellZ; cellZ < endCellZ; ++cellZ) {
        for (auto cellX = startCellX; cellX < endCellX; ++cellX) {
            const auto jitterX = random01(cellX, cellZ, seed ^ 0x8cb92baa3f3d8dd7ULL);
            const auto jitterZ = random01(cellX, cellZ, seed ^ 0x58f38ded2f6b0011ULL);
            const auto worldX = (static_cast<double>(cellX) + 0.12 + jitterX * 0.76) * treeCell;
            const auto worldZ = (static_cast<double>(cellZ) + 0.12 + jitterZ * 0.76) * treeCell;
            if (worldX < tile.origin.x || worldZ < tile.origin.y
                || worldX >= tile.origin.x + tile.spec.tileWorldSize
                || worldZ >= tile.origin.y + tile.spec.tileWorldSize) continue;

            const auto sampleX = std::clamp(static_cast<std::int32_t>((worldX - tile.origin.x) / tile.spec.spacing), 0, visibleWidth(tile) - 1);
            const auto sampleZ = std::clamp(static_cast<std::int32_t>((worldZ - tile.origin.y) / tile.spec.spacing), 0, visibleHeight(tile) - 1);
            const auto elevation = bilinearElevation(tile, worldX, worldZ)
                * visuals.metrics.elevationMetersPerSourceUnit * visuals.metrics.verticalExaggeration;
            // The macro climate describes the broad region, not the local
            // mountain elevation. Never allow a favourable coarse climate or
            // stand-noise sample to bypass the physical treeline.
            if (elevation <= 3.5F || elevation >= visuals.forest.treeLineEndMeters) continue;
            const auto temperature = climateAt(tile, sampleX, sampleZ, 0, 13.0F);
            const auto temperatureStd = climateAt(tile, sampleX, sampleZ, 1, 450.0F) / 100.0F;
            const auto precipitation = std::max(
                0.0F, climateAt(tile, sampleX, sampleZ, 2, 850.0F));
            const auto precipitationCv = std::max(
                0.0F, climateAt(tile, sampleX, sampleZ, 3, 35.0F));
            const auto soilMoisture = climateAt(tile, sampleX, sampleZ, 4, -1.0F);
            const auto exposure = std::clamp(
                climateAt(tile, sampleX, sampleZ, 6, 0.0F), 0.0F, 1.0F);
            const auto substrateRockiness = std::clamp(
                climateAt(tile, sampleX, sampleZ, 7, 0.0F), 0.0F, 1.0F);
            const auto effectiveTemperature = std::max(0.0F, temperature + temperatureStd * 0.5F);
            const auto evapotranspiration = std::max(
                250.0F,
                250.0F + 25.0F * effectiveTemperature + 0.7F * effectiveTemperature * effectiveTemperature);
            auto moisture = (precipitation / evapotranspiration)
                * (1.0F - 0.35F * std::min(1.0F, precipitationCv / 100.0F));
            if (soilMoisture >= 0.0F) {
                moisture = glm::mix(moisture, soilMoisture * 1.35F, 0.68F);
            }
            const auto amplitude = temperatureStd * std::numbers::sqrt2_v<float>;
            float growingDays = 0.0F;
            if (amplitude < 0.1F) {
                growingDays = temperature > 5.0F ? 365.0F : 0.0F;
            } else {
                const auto threshold = (5.0F - temperature) / amplitude;
                growingDays = threshold <= -1.0F ? 365.0F
                    : threshold >= 1.0F ? 0.0F
                    : 365.0F * (0.5F - std::asin(std::clamp(threshold, -1.0F, 1.0F)) / std::numbers::pi_v<float>);
            }
            const auto growingSeason = std::clamp((growingDays - 60.0F) / 90.0F, 0.0F, 1.0F);
            const auto suitability = glm::smoothstep(0.18F, 1.05F,
                moisture * visuals.forest.precipitationResponse * growingSeason);
            const auto fields = deriveTerrainFields(tile, sampleX, sampleZ, worldX, worldZ, seed, visuals);

            // Broad continuous fields create forests with clear cores, feathered
            // edges and natural clearings instead of visible chunk-sized patches.
            const auto forestField = fractalNoise(worldX / (visuals.forest.clearingScaleMeters * 4.0),
                worldZ / (visuals.forest.clearingScaleMeters * 4.0), seed ^ 0x243f6a8885a308d3ULL);
            const auto clearing = fractalNoise(worldX / visuals.forest.clearingScaleMeters,
                worldZ / visuals.forest.clearingScaleMeters, seed ^ 0x13198a2e03707344ULL);
            const auto standStrength = suitability * 0.48F + fields.forest * 0.74F + forestField * 0.28F;
            const auto treeLineSuitability = 1.0F - glm::smoothstep(
                visuals.forest.treeLineStartMeters,
                visuals.forest.treeLineEndMeters,
                elevation);
            const auto terrainSuitability = treeLineSuitability
                * (1.0F - fields.snow)
                * (1.0F - fields.rock)
                * (1.0F - fields.scree * 0.72F)
                * (1.0F - exposure * 0.32F)
                * (1.0F - substrateRockiness * 0.44F)
                * (1.0F - glm::smoothstep(
                    visuals.forest.maximumSlope * 0.45F,
                    visuals.forest.maximumSlope,
                    fields.slope));
            const auto density = glm::smoothstep(0.34F, 0.82F, standStrength)
                * glm::mix(1.0F, glm::smoothstep(0.10F, 0.46F, clearing),
                    visuals.forest.clearingStrength)
                * terrainSuitability
                * visuals.forest.globalDensity;
            if (random01(cellX, cellZ, seed ^ 0xa4093822299f31d0ULL) > density) continue;

            const auto speciesRoll = random01(cellX + 331, cellZ - 257, seed);
            std::uint32_t species = 2U; // oak
            if (temperature < 5.0F) species = 0U; // spruce
            else if (temperature < 12.0F) species = speciesRoll < 0.58F ? 1U : 0U; // birch / spruce
            else if (temperature >= 22.0F && moisture < 0.66F) species = 3U; // acacia
            else if (temperature >= 20.0F && moisture >= 0.9F) species = 4U; // tropical
            const auto variation = random01(cellX, cellZ, seed ^ 0x082efa98ec4e6c89ULL);
            const auto scale = 0.82F + variation * 0.38F;
            constexpr std::array<glm::vec3, 5> dark{{
                {0.66F, 0.74F, 0.62F}, {0.72F, 0.78F, 0.65F}, {0.67F, 0.75F, 0.59F},
                {0.76F, 0.72F, 0.55F}, {0.62F, 0.75F, 0.58F},
            }};
            constexpr std::array<glm::vec3, 5> light{{
                {0.88F, 0.90F, 0.84F}, {0.91F, 0.90F, 0.82F}, {0.87F, 0.86F, 0.72F},
                {0.90F, 0.83F, 0.62F}, {0.80F, 0.87F, 0.72F},
            }};
            const auto color = glm::mix(dark[species], light[species], variation * 0.62F);
            mesh.trees.push_back(VegetationInstance{
                .position = glm::vec3{
                    static_cast<float>(worldX - tile.origin.x),
                    elevation - visuals.forest.billboardSinkMeters,
                    static_cast<float>(worldZ - tile.origin.y),
                },
                .scale = scale,
                .color = packColor(color),
                .parameters = glm::vec2{static_cast<float>(species), random01(cellX, cellZ, seed ^ 0x452821e638d01377ULL) * std::numbers::pi_v<float> * 2.0F},
            });
        }
    }

    if (tile.spec.level != 0) return;

    constexpr double grassCell = 4.0;
    const auto grassStartX = static_cast<std::int64_t>(std::floor(tile.origin.x / grassCell));
    const auto grassStartZ = static_cast<std::int64_t>(std::floor(tile.origin.y / grassCell));
    const auto grassEndX = static_cast<std::int64_t>(std::ceil((tile.origin.x + tile.spec.tileWorldSize) / grassCell));
    const auto grassEndZ = static_cast<std::int64_t>(std::ceil((tile.origin.y + tile.spec.tileWorldSize) / grassCell));
    mesh.grass.reserve(static_cast<std::size_t>((grassEndX - grassStartX) * (grassEndZ - grassStartZ) / 2));
    for (auto cellZ = grassStartZ; cellZ < grassEndZ; ++cellZ) {
        for (auto cellX = grassStartX; cellX < grassEndX; ++cellX) {
            const auto worldX = (static_cast<double>(cellX) + random01(cellX, cellZ, seed ^ 0xbe5466cf34e90c6cULL)) * grassCell;
            const auto worldZ = (static_cast<double>(cellZ) + random01(cellX, cellZ, seed ^ 0xc0ac29b7c97c50ddULL)) * grassCell;
            if (worldX < tile.origin.x || worldZ < tile.origin.y
                || worldX >= tile.origin.x + tile.spec.tileWorldSize
                || worldZ >= tile.origin.y + tile.spec.tileWorldSize) continue;
            const auto sampleX = std::clamp(static_cast<std::int32_t>((worldX - tile.origin.x) / tile.spec.spacing), 0, visibleWidth(tile) - 1);
            const auto sampleZ = std::clamp(static_cast<std::int32_t>((worldZ - tile.origin.y) / tile.spec.spacing), 0, visibleHeight(tile) - 1);
            const auto elevation = bilinearElevation(tile, worldX, worldZ)
                * visuals.metrics.elevationMetersPerSourceUnit * visuals.metrics.verticalExaggeration;
            const auto normal = normalAt(tile, sampleX, sampleZ, visuals.metrics);
            const auto temperature = climateAt(tile, sampleX, sampleZ, 0, 13.0F);
            const auto precipitation = climateAt(tile, sampleX, sampleZ, 2, 850.0F);
            const auto precipitationCv = climateAt(tile, sampleX, sampleZ, 3, 35.0F);
            const auto soilMoisture = climateAt(tile, sampleX, sampleZ, 4, -1.0F);
            const auto exposure = std::clamp(
                climateAt(tile, sampleX, sampleZ, 6, 0.0F), 0.0F, 1.0F);
            const auto substrateRockiness = std::clamp(
                climateAt(tile, sampleX, sampleZ, 7, 0.0F), 0.0F, 1.0F);
            const auto patch = fractalNoise(worldX / 52.0, worldZ / 52.0, seed ^ 0x3f84d5b5b5470917ULL);
            const auto warmEnough = glm::smoothstep(-10.0F, 4.0F, temperature);
            const auto notScorched = 1.0F - glm::smoothstep(34.0F, 47.0F, temperature);
            auto moisture = glm::smoothstep(70.0F, 900.0F, precipitation)
                * (1.0F - 0.28F * std::clamp(precipitationCv / 100.0F, 0.0F, 1.0F));
            if (soilMoisture >= 0.0F) moisture = glm::mix(moisture, soilMoisture, 0.72F);
            const auto growingClimate = warmEnough * notScorched;
            const auto desertFade = glm::smoothstep(35.0F, 150.0F, precipitation);
            const auto density = std::clamp(
                growingClimate * desertFade * glm::mix(0.78F, 0.995F, moisture)
                    + (patch - 0.5F) * 0.16F,
                0.0F, 0.995F) * glm::smoothstep(0.72F, 0.98F, normal.y)
                * (1.0F - exposure * 0.28F) * (1.0F - substrateRockiness * 0.58F);
            if (elevation <= 2.8F || elevation > 2'900.0F
                || random01(cellX, cellZ, seed ^ 0x9216d5d98979fb1bULL) > density) continue;
            const auto heightPatch = fractalNoise(worldX / 38.0, worldZ / 38.0, seed ^ 0xd1310ba698dfb5acULL);
            const auto height = glm::mix(0.34F, 0.78F, heightPatch) * glm::mix(0.82F, 1.06F, moisture);
            auto color = glm::mix(glm::vec3{0.66F, 0.60F, 0.31F}, glm::vec3{0.30F, 0.50F, 0.22F}, moisture);
            if (temperature < 5.0F) color = glm::mix(color, glm::vec3{0.42F, 0.50F, 0.31F}, 0.58F);
            mesh.grass.push_back(VegetationInstance{
                .position = glm::vec3{
                    static_cast<float>(worldX - tile.origin.x), elevation - 0.025F,
                    static_cast<float>(worldZ - tile.origin.y),
                },
                .scale = height,
                .color = packColor(color),
                .parameters = glm::vec2{random01(cellX, cellZ, seed) * std::numbers::pi_v<float>, patch},
            });
        }
    }
}

} // namespace

TileMeshPtr buildTerrainMesh(
    RawTile raw,
    std::uint64_t worldSeed,
    const WorldVisualConfig& visuals) {
    auto mesh = std::make_shared<TileMesh>();
    const auto derivedStart = std::chrono::steady_clock::now();
    mesh->raw = std::move(raw);
    const auto& tile = mesh->raw;
    const auto width = visibleWidth(tile);
    const auto height = visibleHeight(tile);
    const auto vertexCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    mesh->vertices.resize(vertexCount);
    mesh->indices.reserve(static_cast<std::size_t>((width - 1) * (height - 1) * 6));

    for (std::int32_t z = 0; z < height; ++z) {
        for (std::int32_t x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(z * width + x);
            const auto elevation = elevationAt(tile, x, z)
                * visuals.metrics.elevationMetersPerSourceUnit * visuals.metrics.verticalExaggeration;
            const auto worldX = tile.origin.x + static_cast<double>(x) * tile.spec.spacing;
            const auto worldZ = tile.origin.y + static_cast<double>(z) * tile.spec.spacing;
            const auto fields = deriveTerrainFields(tile, x, z, worldX, worldZ, worldSeed, visuals);
            mesh->vertices[index] = TerrainVertex{
                .position = glm::vec3{
                    static_cast<float>(static_cast<double>(x) * tile.spec.spacing),
                    elevation,
                    static_cast<float>(static_cast<double>(z) * tile.spec.spacing),
                },
                .normal = fields.normal,
                .color = packColor(fields.baseColor),
                .derived = packChannels(glm::vec4{
                    fields.slope, fields.signedCurvature, fields.wetness, fields.macroVariation}),
                .biome = packChannels(glm::vec4{
                    fields.forest, fields.rock, fields.scree, fields.snow}),
            };
        }
    }
    for (std::int32_t z = 0; z < height - 1; ++z) {
        for (std::int32_t x = 0; x < width - 1; ++x) {
            const auto a = static_cast<std::uint32_t>(z * width + x);
            const auto b = a + 1U;
            const auto c = a + static_cast<std::uint32_t>(width);
            const auto d = c + 1U;
            mesh->indices.insert(mesh->indices.end(), {a, c, b, b, c, d});
        }
    }
    appendSkirts(*mesh);
    const auto vegetationStart = std::chrono::steady_clock::now();
    mesh->derivedMilliseconds = std::chrono::duration<double, std::milli>(
        vegetationStart - derivedStart).count();
    appendVegetation(*mesh, worldSeed, visuals);
    partitionVegetation(mesh->trees, mesh->treeRanges, tile.spec.tileWorldSize,
        tile.spec.level == 0 ? 240.0F : 640.0F, 28.0F);
    partitionVegetation(mesh->grass, mesh->grassRanges, tile.spec.tileWorldSize, 64.0F, 2.0F);
    mesh->vegetationMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - vegetationStart).count();
    return mesh;
}

} // namespace terrain
