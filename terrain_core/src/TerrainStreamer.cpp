#include <terrain/TerrainStreamer.hpp>

#include <terrain/TerrainMesh.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace terrain {
namespace {

[[nodiscard]] std::size_t terrainBytes(const TileMesh& tile) noexcept {
    return tile.raw.elevations.size() * sizeof(std::int16_t)
        + tile.raw.climate.size() * sizeof(float)
        + tile.vertices.size() * sizeof(TerrainVertex)
        + tile.indices.size() * sizeof(std::uint32_t);
}

[[nodiscard]] std::size_t vegetationBytes(const TileMesh& tile) noexcept {
    return tile.trees.size() * sizeof(VegetationInstance)
        + tile.grass.size() * sizeof(VegetationInstance);
}

} // namespace

TerrainStreamer::TerrainStreamer(TerrainClient client, WorldInfo world, WorldVisualConfig visuals)
    : client_(std::move(client)), world_(std::move(world)), visuals_(std::move(visuals)),
      specs_(makeSpecs(world_)), seed_(parseSeed(world_.seed)) {
    workers_.reserve(visuals_.streaming.maxConcurrentRequests);
    for (std::size_t index = 0; index < visuals_.streaming.maxConcurrentRequests; ++index) {
        workers_.emplace_back([this](const std::stop_token token) { workerLoop(token); });
    }
}

TerrainStreamer::~TerrainStreamer() {
    for (auto& worker : workers_) worker.request_stop();
    workReady_.notify_all();
}

void TerrainStreamer::update(const glm::dvec3& cameraPosition, const glm::dvec3& viewDirection) {
    const glm::dvec2 horizontalView{viewDirection.x, viewDirection.z};
    const auto horizontalViewLength = glm::length(horizontalView);
    if (std::isfinite(horizontalViewLength) && horizontalViewLength > 0.05) {
        preferredViewDirection_ = horizontalView / horizontalViewLength;
    }
    const glm::dvec2 horizontalDelta{
        cameraPosition.x - lastRebuildPosition_.x,
        cameraPosition.z - lastRebuildPosition_.z,
    };
    const auto horizontalMoved = glm::length(horizontalDelta);
    if (std::isfinite(horizontalMoved) && horizontalMoved > 5.0) {
        preferredTravelDirection_ = horizontalDelta / horizontalMoved;
    }
    if (!std::isfinite(lastRebuildPosition_.x)
        || horizontalMoved > 120.0
        || std::abs(cameraPosition.y - lastRebuildAltitude_) > 500.0) {
        rebuildWanted(cameraPosition);
        lastRebuildPosition_ = cameraPosition;
        lastRebuildAltitude_ = cameraPosition.y;
    }
    scheduleMissing(cameraPosition);
}

std::vector<TerrainEvent> TerrainStreamer::drainEvents(
    std::size_t maxAddedEvents,
    std::size_t maxUploadBytes) {
    std::vector<Completed> completed;
    {
        std::scoped_lock lock{workMutex_};
        completed.swap(completed_);
    }

    std::size_t additions{};
    std::size_t uploadBytes{};
    for (auto& item : completed) {
        if (item.request.generation != generation_ || !wanted_.contains(item.request.key)) {
            queued_.erase(item.request.key);
            continue;
        }
        if (!item.error.empty()) {
            queued_.erase(item.request.key);
            retryAfter_[item.request.key] = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            deferredEvents_.push_back(TerrainEvent{
                .type = TerrainEvent::Type::Failed,
                .key = item.request.key,
                .message = std::move(item.error),
            });
            continue;
        }
        const auto itemBytes = item.tile
            ? item.tile->vertices.size() * sizeof(TerrainVertex)
                + item.tile->indices.size() * sizeof(std::uint32_t)
                + item.tile->trees.size() * sizeof(VegetationInstance)
                + item.tile->grass.size() * sizeof(VegetationInstance)
            : 0U;
        if (additions >= maxAddedEvents
            || (additions > 0U && uploadBytes + itemBytes > maxUploadBytes)) {
            std::scoped_lock lock{workMutex_};
            completed_.push_back(std::move(item));
            continue;
        }
        queued_.erase(item.request.key);
        retryAfter_.erase(item.request.key);
        if (const auto existing = loaded_.find(item.request.key); existing != loaded_.end()) {
            performance_.terrainCpuBytes -= terrainBytes(*existing->second);
            performance_.vegetationCpuBytes -= vegetationBytes(*existing->second);
        }
        performance_.latestFetchMilliseconds = item.fetchMilliseconds;
        performance_.latestBuildMilliseconds = item.buildMilliseconds;
        constexpr double smoothing = 0.12;
        performance_.averageFetchMilliseconds = performance_.completedTiles == 0U
            ? item.fetchMilliseconds
            : std::lerp(performance_.averageFetchMilliseconds, item.fetchMilliseconds, smoothing);
        performance_.averageDecodeMilliseconds = performance_.completedTiles == 0U
            ? item.decodeMilliseconds
            : std::lerp(performance_.averageDecodeMilliseconds, item.decodeMilliseconds, smoothing);
        performance_.averageDerivedMilliseconds = performance_.completedTiles == 0U
            ? item.tile->derivedMilliseconds
            : std::lerp(performance_.averageDerivedMilliseconds, item.tile->derivedMilliseconds, smoothing);
        performance_.averageVegetationMilliseconds = performance_.completedTiles == 0U
            ? item.tile->vegetationMilliseconds
            : std::lerp(performance_.averageVegetationMilliseconds, item.tile->vegetationMilliseconds, smoothing);
        performance_.averageBuildMilliseconds = performance_.completedTiles == 0U
            ? item.buildMilliseconds
            : std::lerp(performance_.averageBuildMilliseconds, item.buildMilliseconds, smoothing);
        ++performance_.completedTiles;
        performance_.terrainCpuBytes += terrainBytes(*item.tile);
        performance_.vegetationCpuBytes += vegetationBytes(*item.tile);
        loaded_.insert_or_assign(item.request.key, item.tile);
        deferredEvents_.push_back(TerrainEvent{
            .type = TerrainEvent::Type::Added,
            .key = item.request.key,
            .tile = std::move(item.tile),
        });
        ++additions;
        uploadBytes += itemBytes;
    }

    std::vector<TerrainEvent> events;
    events.swap(deferredEvents_);
    return events;
}

void TerrainStreamer::releaseUploadedPayload(const TileKey& key) {
    const auto iterator = loaded_.find(key);
    if (iterator == loaded_.end() || !iterator->second) return;
    auto& tile = *iterator->second;
    const auto previousTerrainBytes = terrainBytes(tile);
    const auto previousVegetationBytes = vegetationBytes(tile);

    // GPU buffers own the render payload after Renderer::addTile. Retain the
    // compact source elevation grid for collision/raycasting and the spatial
    // ranges used for draw culling, but release data that cannot be reused
    // without rebuilding the tile. This is the normal resident state, not a
    // pressure-only emergency path.
    std::vector<float>{}.swap(tile.raw.climate);
    std::vector<TerrainVertex>{}.swap(tile.vertices);
    std::vector<std::uint32_t>{}.swap(tile.indices);
    std::vector<VegetationInstance>{}.swap(tile.trees);
    std::vector<VegetationInstance>{}.swap(tile.grass);

    performance_.terrainCpuBytes -= previousTerrainBytes - terrainBytes(tile);
    performance_.vegetationCpuBytes -= previousVegetationBytes - vegetationBytes(tile);
}

void TerrainStreamer::regenerate(std::uint64_t seed) {
    ++generation_;
    seed_ = seed;
    wanted_.clear();
    retryAfter_.clear();
    lastRebuildPosition_.x = std::numeric_limits<double>::infinity();
    for (const auto& [key, unused] : loaded_) {
        static_cast<void>(unused);
        deferredEvents_.push_back(TerrainEvent{.type = TerrainEvent::Type::Removed, .key = key});
    }
    loaded_.clear();
    performance_ = {};
    {
        std::scoped_lock lock{workMutex_};
        requests_ = {};
        queued_.clear();
        completed_.clear();
    }
}

void TerrainStreamer::setVisualConfig(const WorldVisualConfig& visuals, bool rebuildDerived) {
    visuals_ = visuals;
    lastRebuildPosition_.x = std::numeric_limits<double>::infinity();
    if (rebuildDerived) regenerate(seed_);
}

std::optional<float> TerrainStreamer::sampleHeight(double worldX, double worldZ) const {
    const TileMeshPtr* best{};
    for (const auto& [key, tile] : loaded_) {
        static_cast<void>(key);
        if (tile->raw.spec.source != TileSource::Detailed || !contains(tile->raw, worldX, worldZ)) continue;
        if (best == nullptr || tile->raw.spec.spacing < (*best)->raw.spec.spacing) best = &tile;
    }
    if (best == nullptr) return std::nullopt;
    return sampleRawHeight((*best)->raw, worldX, worldZ);
}

std::optional<glm::dvec3> TerrainStreamer::findWalkableLand(
    const glm::dvec3& center,
    double maximumDistance,
    bool finestOnly) const {
    std::optional<glm::dvec3> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (const auto& [key, tile] : loaded_) {
        const auto& raw = tile->raw;
        if (raw.spec.source != TileSource::Detailed || (finestOnly && key.level != 0)) continue;
        const auto spacing = raw.spec.spacing;
        const auto visibleWidth = raw.width - raw.borderSamples * 2;
        const auto visibleHeight = raw.height - raw.borderSamples * 2;
        const auto sourceScale = static_cast<double>(visuals_.metrics.elevationMetersPerSourceUnit)
            * static_cast<double>(visuals_.metrics.verticalExaggeration);
        for (std::int32_t z = 1; z < visibleHeight - 1; ++z) {
            for (std::int32_t x = 1; x < visibleWidth - 1; ++x) {
                const auto rawX = x + raw.borderSamples;
                const auto rawZ = z + raw.borderSamples;
                const auto index = static_cast<std::size_t>(rawZ * raw.width + rawX);
                const auto elevation = static_cast<double>(raw.elevations[index]) * sourceScale;
                if (elevation < 4.0 || elevation > 3'200.0) continue;
                const auto worldX = raw.origin.x + static_cast<double>(x) * spacing;
                const auto worldZ = raw.origin.y + static_cast<double>(z) * spacing;
                const auto distance = std::hypot(worldX - center.x, worldZ - center.z);
                if (distance > maximumDistance) continue;
                const auto riseX = std::abs(
                    static_cast<double>(raw.elevations[index + 1U])
                    - static_cast<double>(raw.elevations[index - 1U])) * sourceScale / (spacing * 2.0);
                const auto riseZ = std::abs(
                    static_cast<double>(raw.elevations[index + static_cast<std::size_t>(raw.width)])
                    - static_cast<double>(raw.elevations[index - static_cast<std::size_t>(raw.width)])) * sourceScale / (spacing * 2.0);
                const auto slope = std::max(riseX, riseZ);
                if (slope > 0.30) continue;
                const auto score = distance + slope * 180.0 + std::max(0.0, elevation - 2'200.0) * 0.04;
                if (score < bestScore) {
                    bestScore = score;
                    best = glm::dvec3{worldX, elevation, worldZ};
                }
            }
        }
    }
    return best;
}

std::optional<float> TerrainStreamer::sampleRenderedHeight(double worldX, double worldZ) const {
    const TileMeshPtr* best{};
    for (const auto& [key, tile] : loaded_) {
        static_cast<void>(key);
        if (!contains(tile->raw, worldX, worldZ)) continue;
        if (best == nullptr || tile->raw.spec.spacing < (*best)->raw.spec.spacing) best = &tile;
    }
    if (best == nullptr) return std::nullopt;
    return sampleRawHeight((*best)->raw, worldX, worldZ);
}

std::optional<double> TerrainStreamer::measureRay(
    const glm::dvec3& origin,
    const glm::dvec3& direction,
    double maximumDistance) const {
    const auto ray = glm::normalize(direction);
    double previousDistance = 0.25;
    double previousDelta = std::numeric_limits<double>::infinity();
    for (double distance = 0.25; distance <= maximumDistance;) {
        const auto point = origin + ray * distance;
        const auto height = sampleRenderedHeight(point.x, point.z);
        if (height) {
            const auto horizontalDistance = std::hypot(point.x - origin.x, point.z - origin.z);
            const auto curvedHeight = static_cast<double>(*height)
                - horizontalDistance * horizontalDistance / EarthDiameterMetres;
            const auto delta = point.y - curvedHeight;
            if (delta <= 0.0 && previousDelta > 0.0) {
                double low = previousDistance;
                double high = distance;
                for (std::uint32_t iteration = 0; iteration < 10; ++iteration) {
                    const auto middle = (low + high) * 0.5;
                    const auto middlePoint = origin + ray * middle;
                    const auto middleHeight = sampleRenderedHeight(middlePoint.x, middlePoint.z);
                    const auto middleHorizontal = std::hypot(middlePoint.x - origin.x, middlePoint.z - origin.z);
                    const auto middleCurved = middleHeight
                        ? static_cast<double>(*middleHeight) - middleHorizontal * middleHorizontal / EarthDiameterMetres
                        : -std::numeric_limits<double>::infinity();
                    if (middlePoint.y <= middleCurved) high = middle;
                    else low = middle;
                }
                return (low + high) * 0.5;
            }
            previousDelta = delta;
        }
        previousDistance = distance;
        const auto maximumStep = std::clamp(distance * 0.015, 120.0, 5'000.0);
        const auto step = std::isfinite(previousDelta)
            ? std::clamp(std::abs(previousDelta) * 0.45, 1.0, maximumStep)
            : maximumStep;
        distance += step;
    }
    return std::nullopt;
}

bool TerrainStreamer::readyNearPlayer(const glm::dvec3& position) const {
    const auto& spec = specFor(0);
    const auto centerX = static_cast<std::int64_t>(std::floor(position.x / spec.tileWorldSize));
    const auto centerZ = static_cast<std::int64_t>(std::floor(position.z / spec.tileWorldSize));
    for (std::int32_t z = -1; z <= 1; ++z) {
        for (std::int32_t x = -1; x <= 1; ++x) {
            if (!loaded_.contains(TileKey{0, centerX + x, centerZ + z})) return false;
        }
    }
    return true;
}

double TerrainStreamer::installedRadius() const {
    double radius{};
    for (const auto& spec : specs_) {
        bool wantedAtLevel{};
        bool complete = true;
        for (const auto& key : wanted_) {
            if (key.level != spec.level) continue;
            wantedAtLevel = true;
            if (!loaded_.contains(key)) {
                complete = false;
                break;
            }
        }
        if (!wantedAtLevel) continue;
        if (complete) radius = std::max(radius, spec.outerRadius);
    }
    return radius;
}

std::size_t TerrainStreamer::pendingTileCount() const {
    std::scoped_lock lock{workMutex_};
    return queued_.size();
}

std::string TerrainStreamer::debugLodConfiguration() const {
    std::string result;
    for (const auto& spec : specs_) {
        if (!result.empty()) result += " | ";
        const auto source = spec.source == TileSource::Detailed ? "detail"
            : spec.source == TileSource::Latent ? "latent" : "orbital";
        result += std::format("L{}:{} {:.0f}m [{:.1f},{:.1f}]km", spec.level, source,
            spec.spacing, spec.innerRadius / 1'000.0, spec.outerRadius / 1'000.0);
    }
    return result;
}

std::string TerrainStreamer::debugHeightStack(double worldX, double worldZ) const {
    std::string result;
    for (const auto& spec : specs_) {
        const TileMeshPtr* match{};
        for (const auto& [key, tile] : loaded_) {
            if (key.level == spec.level && contains(tile->raw, worldX, worldZ)) {
                match = &tile;
                break;
            }
        }
        if (match == nullptr) continue;
        if (!result.empty()) result += " | ";
        const auto source = spec.source == TileSource::Detailed ? "detail"
            : spec.source == TileSource::Latent ? "latent" : "orbital";
        result += std::format("L{}:{}={:.1f}m", spec.level, source,
            sampleRawHeight((*match)->raw, worldX, worldZ));
    }
    return result.empty() ? "no loaded tile intersects camera" : result;
}

void TerrainStreamer::workerLoop(const std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        Request request;
        {
            std::unique_lock lock{workMutex_};
            if (!workReady_.wait(lock, stopToken, [this] { return !requests_.empty(); })) return;
            request = requests_.top();
            requests_.pop();
        }
        Completed completed{.request = request};
        try {
            TileFetchTiming fetchTiming;
            auto raw = client_.fetchTile(request.key, request.spec, world_, request.seed, &fetchTiming);
            const auto buildStart = std::chrono::steady_clock::now();
            completed.fetchMilliseconds = fetchTiming.requestMilliseconds;
            completed.decodeMilliseconds = fetchTiming.decodeMilliseconds;
            completed.tile = buildTerrainMesh(std::move(raw), request.seed, visuals_);
            completed.buildMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - buildStart).count();
        } catch (const std::exception& error) {
            completed.error = error.what();
        }
        {
            std::scoped_lock lock{workMutex_};
            completed_.push_back(std::move(completed));
        }
    }
}

void TerrainStreamer::rebuildWanted(const glm::dvec3& cameraPosition) {
    const auto localGround = sampleHeight(cameraPosition.x, cameraPosition.z);
    const auto height = std::max(1.68, cameraPosition.y - static_cast<double>(localGround.value_or(0.0F)));
    const auto horizonAngle = std::acos(EarthRadiusMetres / (EarthRadiusMetres + height));
    const auto surfaceHorizon = EarthRadiusMetres * horizonAngle;
    requestedRadius_ = std::clamp(surfaceHorizon * 1.12, 6'000.0, 5'500'000.0);

    // Select the innermost clipmap from projected ground resolution. The
    // underlying samples remain the same scale-8 Terrain Diffusion surface;
    // altitude only changes the decimation used to render it. Roughly 512
    // samples span the camera-to-ground distance, keeping sub-pixel geometry
    // out of aircraft/orbital views while restoring it automatically during
    // descent.
    const auto finestSpacing = static_cast<double>(world_.nativeResolution) / 8.0;
    const auto targetSpacing = std::max(finestSpacing, height / 512.0);
    activeMinimumLevel_ = std::clamp(
        static_cast<std::int32_t>(std::ceil(std::log2(targetSpacing / finestSpacing))),
        0, static_cast<std::int32_t>(specs_.size()) - 1);
    // Above ordinary mountain-flight altitude, a complete continental shell
    // is more valuable than one tiny high-resolution patch beneath the
    // camera. Near detail still streams after the visible horizon is covered.
    prioritizeCoarseFirst_ = height > 10'000.0;

    std::unordered_set<TileKey, TileKeyHash> nextWanted;
    for (const auto& spec : specs_) {
        if (spec.level < activeMinimumLevel_) continue;
        if (spec.innerRadius > requestedRadius_ * 1.08) break;
        const auto outerLimit = std::min(spec.outerRadius * 1.05, requestedRadius_ * 1.08);
        const auto innerLimit = spec.innerRadius * 0.72;
        const auto centerX = static_cast<std::int64_t>(std::floor(cameraPosition.x / spec.tileWorldSize));
        const auto centerZ = static_cast<std::int64_t>(std::floor(cameraPosition.z / spec.tileWorldSize));
        const auto dynamicRadius = std::min(spec.tileRadius,
            static_cast<std::int32_t>(std::ceil(outerLimit / spec.tileWorldSize)) + 1);
        for (std::int32_t dz = -dynamicRadius; dz <= dynamicRadius; ++dz) {
            for (std::int32_t dx = -dynamicRadius; dx <= dynamicRadius; ++dx) {
                const auto tileX = centerX + dx;
                const auto tileZ = centerZ + dz;
                const auto minimumX = static_cast<double>(tileX) * spec.tileWorldSize;
                const auto minimumZ = static_cast<double>(tileZ) * spec.tileWorldSize;
                const auto maximumX = minimumX + spec.tileWorldSize;
                const auto maximumZ = minimumZ + spec.tileWorldSize;

                const auto closestX = std::clamp(cameraPosition.x, minimumX, maximumX);
                const auto closestZ = std::clamp(cameraPosition.z, minimumZ, maximumZ);
                const auto closestDistance = std::hypot(
                    closestX - cameraPosition.x, closestZ - cameraPosition.z);
                const auto farthestDistance = std::max({
                    std::hypot(minimumX - cameraPosition.x, minimumZ - cameraPosition.z),
                    std::hypot(maximumX - cameraPosition.x, minimumZ - cameraPosition.z),
                    std::hypot(minimumX - cameraPosition.x, maximumZ - cameraPosition.z),
                    std::hypot(maximumX - cameraPosition.x, maximumZ - cameraPosition.z),
                });
                if (closestDistance <= outerLimit && farthestDistance >= innerLimit) {
                    nextWanted.insert(TileKey{spec.level, tileX, tileZ});
                }
            }
        }
    }

    // Uploaded tiles form a bounded resident cache. Previously every tile
    // outside the current wanted set was destroyed immediately, so revisiting
    // cached terrain still repeated HTTP, decode, derivation and GPU upload.
    // Retain dormant tiles until the explicit resident limit is exceeded.
    if (loaded_.size() > visuals_.streaming.maxResidentTiles) {
        struct ResidentCandidate final { TileKey key; double distance; };
        std::vector<ResidentCandidate> candidates;
        candidates.reserve(loaded_.size());
        for (const auto& [key, tile] : loaded_) {
            if (nextWanted.contains(key)) continue;
            const auto centerX = tile->raw.origin.x + tile->raw.spec.tileWorldSize * 0.5;
            const auto centerZ = tile->raw.origin.y + tile->raw.spec.tileWorldSize * 0.5;
            candidates.push_back({key, std::hypot(centerX - cameraPosition.x, centerZ - cameraPosition.z)});
        }
        std::ranges::sort(candidates, std::greater{}, &ResidentCandidate::distance);
        for (const auto& candidate : candidates) {
            if (loaded_.size() <= visuals_.streaming.maxResidentTiles) break;
            const auto loaded = loaded_.find(candidate.key);
            if (loaded == loaded_.end()) continue;
            deferredEvents_.push_back(TerrainEvent{.type = TerrainEvent::Type::Removed, .key = loaded->first});
            performance_.terrainCpuBytes -= terrainBytes(*loaded->second);
            performance_.vegetationCpuBytes -= vegetationBytes(*loaded->second);
            loaded_.erase(loaded);
        }
    }

    const auto terrainBudget = visuals_.streaming.terrainMemoryMegabytes * 1024U * 1024U;
    const auto vegetationBudget = visuals_.streaming.vegetationMemoryMegabytes * 1024U * 1024U;
    if (performance_.terrainCpuBytes > terrainBudget
        || performance_.vegetationCpuBytes > vegetationBudget) {
        struct EvictionCandidate final { TileKey key; double score; };
        std::vector<EvictionCandidate> candidates;
        candidates.reserve(loaded_.size());
        for (const auto& [key, tile] : loaded_) {
            const auto centerX = tile->raw.origin.x + tile->raw.spec.tileWorldSize * 0.5;
            const auto centerZ = tile->raw.origin.y + tile->raw.spec.tileWorldSize * 0.5;
            const auto distanceInTiles = std::hypot(
                centerX - cameraPosition.x, centerZ - cameraPosition.z)
                / tile->raw.spec.tileWorldSize;
            // Fine tiles are the first graceful degradation under pressure;
            // coarse parent coverage is retained to avoid holes.
            candidates.push_back({key,
                10'000.0 - static_cast<double>(key.level) * 500.0 + distanceInTiles});
        }
        std::ranges::sort(candidates, {}, &EvictionCandidate::score);
        for (auto iterator = candidates.rbegin(); iterator != candidates.rend()
            && (performance_.terrainCpuBytes > terrainBudget
                || performance_.vegetationCpuBytes > vegetationBudget); ++iterator) {
            const auto loaded = loaded_.find(iterator->key);
            if (loaded == loaded_.end()) continue;
            deferredEvents_.push_back(TerrainEvent{.type = TerrainEvent::Type::Removed, .key = loaded->first});
            performance_.terrainCpuBytes -= terrainBytes(*loaded->second);
            performance_.vegetationCpuBytes -= vegetationBytes(*loaded->second);
            nextWanted.erase(loaded->first);
            loaded_.erase(loaded);
        }
    }

    // Fast flight can cross several near tiles per frame. Keep the worker's
    // priority queue aligned with the newest viewer neighborhood instead of
    // allowing obsolete requests to accumulate ahead of visible terrain.
    {
        std::scoped_lock lock{workMutex_};
        std::priority_queue<Request, std::vector<Request>, RequestLater> retained;
        while (!requests_.empty()) {
            auto request = requests_.top();
            requests_.pop();
            if (request.generation == generation_ && nextWanted.contains(request.key)) {
                retained.push(std::move(request));
            } else {
                queued_.erase(request.key);
            }
        }
        requests_ = std::move(retained);
    }
    wanted_ = std::move(nextWanted);
}

void TerrainStreamer::scheduleMissing(const glm::dvec3& cameraPosition) {
    const auto now = std::chrono::steady_clock::now();
    bool added{};
    std::scoped_lock lock{workMutex_};
    for (const auto& key : wanted_) {
        if (loaded_.contains(key) || queued_.contains(key)) continue;
        const auto retry = retryAfter_.find(key);
        if (retry != retryAfter_.end() && retry->second > now) continue;
        const auto& spec = specFor(key.level);
        const auto centerX = (static_cast<double>(key.x) + 0.5) * spec.tileWorldSize;
        const auto centerZ = (static_cast<double>(key.z) + 0.5) * spec.tileWorldSize;
        const auto distance = std::hypot(centerX - cameraPosition.x, centerZ - cameraPosition.z);
        // Start at the finest level that can still affect a pixel, then expand
        // outward. Normalising distance by tile size avoids kilometre-scale
        // rings permanently starving behind a few local requests.
        const auto levelDistance = prioritizeCoarseFirst_
            ? specs_.back().level - key.level
            : key.level - activeMinimumLevel_;
        const auto levelPenalty = static_cast<double>(levelDistance) * 1'200.0;
        const glm::dvec2 tileDirection{centerX - cameraPosition.x, centerZ - cameraPosition.z};
        const auto directionLength = glm::length(tileDirection);
        const auto travelAlignment = directionLength > 1.0 && glm::length(preferredTravelDirection_) > 0.5
            ? std::max(0.0, glm::dot(tileDirection / directionLength, preferredTravelDirection_))
            : 0.0;
        const auto viewAlignment = directionLength > 1.0
            ? std::max(0.0, glm::dot(tileDirection / directionLength, preferredViewDirection_))
            : 1.0;
        // Missing geometry in front of the player is much more visible than
        // equal-distance side or rear coverage. Distance still guarantees a
        // hole-free local footprint; this term controls async request order.
        const auto viewPenalty = (1.0 - viewAlignment) * 6'000.0;
        const auto priority = levelPenalty + distance / spec.tileWorldSize * 1'000.0
            + viewPenalty - travelAlignment * 1'200.0;
        requests_.push(Request{key, spec, generation_, seed_, priority});
        queued_.insert(key);
        added = true;
    }
    if (added) workReady_.notify_all();
}

std::vector<LodSpec> TerrainStreamer::makeSpecs(const WorldInfo& world) {
    std::vector<LodSpec> specs;
    constexpr std::int32_t scale = 8;
    constexpr std::int32_t segments = 128;
    constexpr std::int32_t levelCount = 11;
    constexpr double baseOuterRadius = 1'200.0;
    const auto finestSpacing = static_cast<double>(world.nativeResolution) / scale;
    double previousOuter{};
    for (std::int32_t level = 0; level < levelCount; ++level) {
        TileSource source{TileSource::Detailed};
        auto sourceScale = scale;
        auto stride = 1 << level;
        auto spacing = finestSpacing * stride;
        if (level >= 6 && level <= 9) {
            // Exact source pages remain practical through L5. Past that, a
            // single render tile would touch 4, 16, 64... independent 32 MiB
            // pages. Terrain Diffusion's generated latent stage is the bounded
            // horizon preview while detailed pages stream near the viewer.
            source = TileSource::Latent;
            sourceScale = 1;
            stride = 1 << (level - 6);
            spacing = static_cast<double>(world.latentResolution) * stride;
        } else if (level == 10) {
            source = TileSource::Orbital;
            sourceScale = 1;
            stride = 1;
            spacing = static_cast<double>(world.coarseResolution);
        }
        const auto tileSize = spacing * segments;
        auto outer = baseOuterRadius * std::pow(2.0, level);
        // The last ring is the aircraft/orbital horizon shell. Widening this
        // single very coarse annulus reaches the ~3,200 km horizon at 800 km
        // altitude without multiplying every inner ring's vertex count.
        if (level == levelCount - 1) outer = 3'700'000.0;
        const auto inner = level == 0 ? 0.0 : previousOuter * 0.82;
        specs.push_back(LodSpec{
            .level = level,
            .source = source,
            .segments = segments,
            .scale = sourceScale,
            .stride = stride,
            .spacing = spacing,
            .tileWorldSize = tileSize,
            .innerRadius = inner,
            .outerRadius = outer,
            .tileRadius = static_cast<std::int32_t>(std::ceil(outer / tileSize)) + 1,
            .castsShadow = level <= 1,
            .vegetation = level <= 1,
        });
        previousOuter = outer;
    }
    return specs;
}

const LodSpec& TerrainStreamer::specFor(std::int32_t level) const {
    const auto iterator = std::ranges::find_if(specs_, [level](const auto& spec) { return spec.level == level; });
    if (iterator == specs_.end()) throw std::logic_error("Unknown terrain LOD level");
    return *iterator;
}

std::uint64_t TerrainStreamer::parseSeed(const std::string& value) {
    try {
        return std::stoull(value);
    } catch (...) {
        return static_cast<std::uint64_t>(std::hash<std::string>{}(value));
    }
}

bool TerrainStreamer::contains(const RawTile& tile, double worldX, double worldZ) noexcept {
    return worldX >= tile.origin.x && worldZ >= tile.origin.y
        && worldX <= tile.origin.x + tile.spec.tileWorldSize
        && worldZ <= tile.origin.y + tile.spec.tileWorldSize;
}

float TerrainStreamer::sampleRawHeight(const RawTile& tile, double worldX, double worldZ) const noexcept {
    const auto visibleWidth = tile.width - tile.borderSamples * 2;
    const auto visibleHeight = tile.height - tile.borderSamples * 2;
    const auto x = std::clamp((worldX - tile.origin.x) / tile.spec.spacing, 0.0, static_cast<double>(visibleWidth - 1));
    const auto z = std::clamp((worldZ - tile.origin.y) / tile.spec.spacing, 0.0, static_cast<double>(visibleHeight - 1));
    const auto x0 = std::min(static_cast<std::int32_t>(std::floor(x)), visibleWidth - 2);
    const auto z0 = std::min(static_cast<std::int32_t>(std::floor(z)), visibleHeight - 2);
    const auto tx = static_cast<float>(x - x0);
    const auto tz = static_cast<float>(z - z0);
    const auto at = [&tile](std::int32_t sx, std::int32_t sz) {
        const auto rawX = sx + tile.borderSamples;
        const auto rawZ = sz + tile.borderSamples;
        return static_cast<float>(tile.elevations[static_cast<std::size_t>(rawZ * tile.width + rawX)]);
    };
    const auto a = std::lerp(at(x0, z0), at(x0 + 1, z0), tx);
    const auto b = std::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx);
    return std::lerp(a, b, tz) * visuals_.metrics.elevationMetersPerSourceUnit
        * visuals_.metrics.verticalExaggeration;
}

} // namespace terrain
