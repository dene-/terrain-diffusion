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
    std::size_t residentRenderBytes{};
};

class TerrainStreamer final {
public:
    TerrainStreamer(TerrainClient client, WorldInfo world, WorldVisualConfig visuals);
    ~TerrainStreamer();

    TerrainStreamer(const TerrainStreamer&) = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;

    void update(
        const glm::dvec3& cameraPosition,
        const glm::dvec3& viewDirection,
        double verticalFovRadians,
        double viewportHeightPixels);
    [[nodiscard]] std::vector<TerrainEvent> drainEvents(
        std::size_t maxAddedEvents,
        std::size_t maxUploadBytes);
    void releaseUploadedPayload(const TileKey& key);
    void recordResidentRenderBytes(const TileKey& key, std::size_t bytes);
    void releaseResidentRenderBytes(const TileKey& key);
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
    [[nodiscard]] std::int32_t activeMinimumLevel() const noexcept { return activeMinimumLevel_; }
    [[nodiscard]] LodPresentationRange presentationRange(std::int32_t level) const noexcept;
    [[nodiscard]] std::uint64_t presentationRevision() const noexcept {
        return presentationRevision_;
    }
    [[nodiscard]] std::size_t loadedTileCount() const noexcept { return loaded_.size(); }
    [[nodiscard]] bool isWanted(const TileKey& key) const noexcept {
        return wanted_.contains(key);
    }
    [[nodiscard]] bool hasWantedAncestor(TileKey key) const noexcept;
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
    [[nodiscard]] double requestPriority(
        const TileKey& key,
        const LodSpec& spec,
        const glm::dvec3& cameraPosition) const;
    [[nodiscard]] static std::vector<LodSpec> makeSpecs(const WorldInfo& world);
    [[nodiscard]] const LodSpec& specFor(std::int32_t level) const;
    [[nodiscard]] static std::uint64_t parseSeed(const std::string& value);
    [[nodiscard]] static bool contains(const RawTile& tile, double worldX, double worldZ) noexcept;
    [[nodiscard]] float sampleRawHeight(const RawTile& tile, double worldX, double worldZ) const noexcept;
    [[nodiscard]] std::optional<float> sampleRenderedHeight(double worldX, double worldZ) const;
    [[nodiscard]] double geometricErrorMetres(const LodSpec& spec) const noexcept;

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
    std::unordered_map<TileKey, std::size_t, TileKeyHash> residentRenderBytes_;
    std::unordered_map<TileKey, std::chrono::steady_clock::time_point, TileKeyHash> retryAfter_;
    std::vector<TerrainEvent> deferredEvents_;
    glm::dvec3 lastRebuildPosition_{std::numeric_limits<double>::infinity()};
    double lastRebuildAltitude_{-1.0};
    std::chrono::steady_clock::time_point lastMissingSchedule_{};
    double lastViewerHeight_{1.68};
    double verticalFovRadians_{1.0471975511965976};
    double viewportHeightPixels_{900.0};
    double requestedRadius_{6'000.0};
    std::int32_t activeMinimumLevel_{};
    std::vector<double> measuredGeometricErrorSourceUnits_;
    std::vector<LodPresentationRange> presentationRanges_;
    std::uint64_t presentationRevision_{};
    StreamingPerformance performance_;
};

} // namespace terrain
