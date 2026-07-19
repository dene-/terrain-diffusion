#pragma once

#include <terrain/TerrainClient.hpp>
#include <terrain/TerrainTypes.hpp>
#include <terrain/VisualConfig.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace terrain {

struct StreamingPerformance final {
    double latestFetchMilliseconds{};
    double latestBuildMilliseconds{};
    double averageFetchMilliseconds{};
    double averageDecodeMilliseconds{};
    double averageDerivedMilliseconds{};
    double averageVegetationMilliseconds{};
    double averageBuildMilliseconds{};
    std::uint64_t completedTiles{};
    std::size_t terrainCpuBytes{};
    std::size_t vegetationCpuBytes{};
};

class TerrainStreamer final {
public:
    TerrainStreamer(TerrainClient client, WorldInfo world, WorldVisualConfig visuals);
    ~TerrainStreamer();

    TerrainStreamer(const TerrainStreamer&) = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;

    void update(const glm::dvec3& cameraPosition, const glm::dvec3& viewDirection);
    [[nodiscard]] std::vector<TerrainEvent> drainEvents(
        std::size_t maxAddedEvents,
        std::size_t maxUploadBytes);
    void releaseUploadedPayload(const TileKey& key);
    void regenerate(std::uint64_t seed);
    void setVisualConfig(const WorldVisualConfig& visuals, bool rebuildDerived);

    [[nodiscard]] std::optional<float> sampleHeight(double worldX, double worldZ) const;
    [[nodiscard]] std::optional<glm::dvec3> findWalkableLand(
        const glm::dvec3& center,
        double maximumDistance,
        bool finestOnly) const;
    [[nodiscard]] std::optional<double> measureRay(
        const glm::dvec3& origin,
        const glm::dvec3& direction,
        double maximumDistance) const;
    [[nodiscard]] bool readyNearPlayer(const glm::dvec3& position) const;
    [[nodiscard]] double installedRadius() const;
    [[nodiscard]] double requestedRadius() const noexcept { return requestedRadius_; }
    [[nodiscard]] std::size_t loadedTileCount() const noexcept { return loaded_.size(); }
    [[nodiscard]] std::size_t pendingTileCount() const;
    [[nodiscard]] std::string debugLodConfiguration() const;
    [[nodiscard]] std::string debugHeightStack(double worldX, double worldZ) const;
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
    [[nodiscard]] const WorldInfo& world() const noexcept { return world_; }
    [[nodiscard]] const StreamingPerformance& performance() const noexcept { return performance_; }

private:
    struct Request final {
        TileKey key;
        LodSpec spec;
        std::uint64_t generation{};
        std::uint64_t seed{};
        double priority{};
    };

    struct RequestLater final {
        bool operator()(const Request& left, const Request& right) const noexcept {
            return left.priority > right.priority;
        }
    };

    struct Completed final {
        Request request;
        TileMeshPtr tile;
        std::string error;
        double fetchMilliseconds{};
        double decodeMilliseconds{};
        double buildMilliseconds{};
    };

    void workerLoop(std::stop_token stopToken);
    void rebuildWanted(const glm::dvec3& cameraPosition);
    void scheduleMissing(const glm::dvec3& cameraPosition);
    [[nodiscard]] static std::vector<LodSpec> makeSpecs(const WorldInfo& world);
    [[nodiscard]] const LodSpec& specFor(std::int32_t level) const;
    [[nodiscard]] static std::uint64_t parseSeed(const std::string& value);
    [[nodiscard]] static bool contains(const RawTile& tile, double worldX, double worldZ) noexcept;
    [[nodiscard]] float sampleRawHeight(const RawTile& tile, double worldX, double worldZ) const noexcept;
    [[nodiscard]] std::optional<float> sampleRenderedHeight(double worldX, double worldZ) const;

    TerrainClient client_;
    WorldInfo world_;
    WorldVisualConfig visuals_;
    std::vector<LodSpec> specs_;
    std::uint64_t seed_{};
    std::uint64_t generation_{1};

    mutable std::mutex workMutex_;
    std::condition_variable_any workReady_;
    std::priority_queue<Request, std::vector<Request>, RequestLater> requests_;
    std::vector<Completed> completed_;
    std::unordered_set<TileKey, TileKeyHash> queued_;
    std::vector<std::jthread> workers_;

    std::unordered_set<TileKey, TileKeyHash> wanted_;
    std::unordered_map<TileKey, TileMeshPtr, TileKeyHash> loaded_;
    std::unordered_map<TileKey, std::chrono::steady_clock::time_point, TileKeyHash> retryAfter_;
    std::vector<TerrainEvent> deferredEvents_;
    glm::dvec3 lastRebuildPosition_{std::numeric_limits<double>::infinity()};
    glm::dvec2 preferredTravelDirection_{};
    glm::dvec2 preferredViewDirection_{0.0, 1.0};
    double lastRebuildAltitude_{-1.0};
    double requestedRadius_{6'000.0};
    std::int32_t activeMinimumLevel_{};
    bool prioritizeCoarseFirst_{};
    StreamingPerformance performance_;
};

} // namespace terrain
