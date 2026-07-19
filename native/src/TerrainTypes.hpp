#pragma once

#include <glm/glm.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace terrain {

constexpr double EarthRadiusMetres = 6'371'000.0;
constexpr double EarthDiameterMetres = EarthRadiusMetres * 2.0;

struct WorldInfo final {
    std::string seed;
    std::int32_t nativeResolution{30};
    std::int32_t latentResolution{240};
    std::int32_t coarseResolution{11'520};
};

enum class TileSource : std::uint8_t { Detailed, Latent, Orbital };

struct TileKey final {
    std::int32_t level{};
    std::int64_t x{};
    std::int64_t z{};
    auto operator<=>(const TileKey&) const = default;
};

struct TileKeyHash final {
    [[nodiscard]] std::size_t operator()(const TileKey& key) const noexcept {
        auto hash = static_cast<std::uint64_t>(key.level) * 0x9e3779b97f4a7c15ULL;
        hash ^= static_cast<std::uint64_t>(key.x) + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<std::uint64_t>(key.z) + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
        return static_cast<std::size_t>(hash);
    }
};

struct LodSpec final {
    std::int32_t level{};
    TileSource source{TileSource::Detailed};
    std::int32_t segments{};
    std::int32_t scale{1};
    std::int32_t stride{1};
    double spacing{};
    double tileWorldSize{};
    double innerRadius{};
    double outerRadius{};
    std::int32_t tileRadius{};
    bool castsShadow{};
    bool vegetation{};
};

struct RawTile final {
    TileKey key;
    LodSpec spec;
    std::int32_t width{};
    std::int32_t height{};
    // Samples outside the visible tile extent. They are retained on the CPU
    // for central derivatives but never become render vertices.
    std::int32_t borderSamples{};
    glm::dvec2 origin{};
    std::vector<std::int16_t> elevations;
    std::vector<float> climate;
};

struct TerrainVertex final {
    glm::vec3 position{};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    std::uint32_t color{0xffffffffU};
    // UBYTE4_NORM: slope, signed curvature, wetness, macro variation.
    std::uint32_t derived{0x80800000U};
    // UBYTE4_NORM: forest, rock, scree, snow suitability.
    std::uint32_t biome{};
};

struct VegetationInstance final {
    glm::vec3 position{};
    float scale{1.0F};
    std::uint32_t color{0xffffffffU};
    glm::vec2 parameters{};
    float padding{};
};

struct VegetationRange final {
    std::uint32_t offset{};
    std::uint32_t count{};
    glm::vec2 center{};
    float radius{};
};

struct TileMesh final {
    RawTile raw;
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<VegetationInstance> trees;
    std::vector<VegetationInstance> grass;
    std::vector<VegetationRange> treeRanges;
    std::vector<VegetationRange> grassRanges;
    double derivedMilliseconds{};
    double vegetationMilliseconds{};
};

using TileMeshPtr = std::shared_ptr<TileMesh>;

struct TerrainEvent final {
    enum class Type : std::uint8_t { Added, Removed, Failed };
    Type type{Type::Added};
    TileKey key{};
    TileMeshPtr tile;
    std::string message;
};

struct CameraState final {
    glm::dvec3 absolutePosition{};
    glm::vec3 localPosition{};
    glm::vec3 forward{0.0F, 0.0F, 1.0F};
    glm::vec3 right{1.0F, 0.0F, 0.0F};
    glm::mat4 view{};
    glm::mat4 projection{};
    glm::mat4 viewProjection{};
    glm::dvec2 floatingOrigin{};
    float altitudeAboveGround{};
    float verticalFovRadians{};
    float aspect{};
    float nearPlane{0.1F};
    float farPlane{6'500'000.0F};
};

} // namespace terrain
