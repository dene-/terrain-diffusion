#include <terrain/TerrainStreamer.hpp>

#include <terrain/TerrainMesh.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace terrain {
namespace {

[[nodiscard]] std::size_t terrainBytes(const TileMesh& tile) noexcept {
    return tile.raw.elevations.size() * sizeof(std::int16_t)
        + tile.raw.renderElevations.size() * sizeof(float)
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
    measuredGeometricErrorSourceUnits_.resize(specs_.size());
    presentationRanges_.reserve(specs_.size());
    for (const auto& spec : specs_) {
        presentationRanges_.push_back({spec.innerRadius, spec.outerRadius});
    }
    workers_.reserve(visuals_.streaming.maxConcurrentRequests);
    for (std::size_t index = 0; index < visuals_.streaming.maxConcurrentRequests; ++index) {
        workers_.emplace_back([this](const std::stop_token token) { workerLoop(token); });
    }
}

TerrainStreamer::~TerrainStreamer() {
    for (auto& worker : workers_) worker.request_stop();
    workReady_.notify_all();
}

void TerrainStreamer::update(
    const glm::dvec3& cameraPosition,
    const glm::dvec3& viewDirection,
    const double verticalFovRadians,
    const double viewportHeightPixels) {
    static_cast<void>(viewDirection);
    const auto validFov = std::isfinite(verticalFovRadians)
        && verticalFovRadians > 0.1 && verticalFovRadians < 3.0;
    const auto validViewportHeight = std::isfinite(viewportHeightPixels)
        && viewportHeightPixels >= 1.0;
    const auto nextFov = validFov ? verticalFovRadians : verticalFovRadians_;
    const auto nextViewportHeight = validViewportHeight ? viewportHeightPixels : viewportHeightPixels_;
    const auto projectionChanged = std::abs(nextFov - verticalFovRadians_) > 0.001
        || std::abs(nextViewportHeight - viewportHeightPixels_)
            > std::max(1.0, viewportHeightPixels_ * 0.02);
    verticalFovRadians_ = nextFov;
    viewportHeightPixels_ = nextViewportHeight;
    if (projectionChanged) lastRebuildPosition_.x = std::numeric_limits<double>::infinity();

    const glm::dvec2 horizontalDelta{
        cameraPosition.x - lastRebuildPosition_.x,
        cameraPosition.z - lastRebuildPosition_.z,
    };
    const auto horizontalMoved = glm::length(horizontalDelta);
    bool wantedRebuilt{};
    if (!std::isfinite(lastRebuildPosition_.x)
        || horizontalMoved > 120.0
        || std::abs(cameraPosition.y - lastRebuildAltitude_) > 500.0) {
        rebuildWanted(cameraPosition);
        lastRebuildPosition_ = cameraPosition;
        lastRebuildAltitude_ = cameraPosition.y;
        wantedRebuilt = true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (wantedRebuilt || now - lastMissingSchedule_ >= std::chrono::milliseconds{100}) {
        scheduleMissing(cameraPosition);
        lastMissingSchedule_ = now;
    }
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
        const auto level = item.tile->raw.key.level;
        const auto sourceError = item.tile->raw.geometricErrorSourceUnits;
        if (level >= 0
            && static_cast<std::size_t>(level) < measuredGeometricErrorSourceUnits_.size()
            && std::isfinite(sourceError) && sourceError >= 0.0) {
            auto& measured = measuredGeometricErrorSourceUnits_[static_cast<std::size_t>(level)];
            const auto materiallyLarger = sourceError > measured * 1.20 + 0.25;
            measured = std::max(measured, sourceError);
            if (materiallyLarger) {
                // Re-evaluate the clipmap only when a newly observed source
                // error materially changes its projection. This is event
                // driven and avoids per-tile/per-frame diagnostic churn.
                lastRebuildPosition_.x = std::numeric_limits<double>::infinity();
            }
        }
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

void TerrainStreamer::recordResidentRenderBytes(const TileKey& key, const std::size_t bytes) {
    if (const auto existing = residentRenderBytes_.find(key);
        existing != residentRenderBytes_.end()) {
        performance_.residentRenderBytes -= existing->second;
    }
    residentRenderBytes_.insert_or_assign(key, bytes);
    performance_.residentRenderBytes += bytes;
}

void TerrainStreamer::releaseResidentRenderBytes(const TileKey& key) {
    const auto existing = residentRenderBytes_.find(key);
    if (existing == residentRenderBytes_.end()) return;
    performance_.residentRenderBytes -= existing->second;
    residentRenderBytes_.erase(existing);
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
    residentRenderBytes_.clear();
    std::ranges::fill(measuredGeometricErrorSourceUnits_, 0.0);
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
    // Clipmap tiles are aligned to their level's world-space grid. Resolve at
    // most one hash lookup per detailed level instead of scanning every
    // resident tile, starting with the finest representation.
    for (const auto& spec : specs_) {
        if (spec.source != TileSource::Detailed) continue;
        const TileKey key{
            spec.level,
            static_cast<std::int64_t>(std::floor(worldX / spec.tileWorldSize)),
            static_cast<std::int64_t>(std::floor(worldZ / spec.tileWorldSize)),
        };
        const auto tile = loaded_.find(key);
        if (tile != loaded_.end() && tile->second
            && contains(tile->second->raw, worldX, worldZ)) {
            return sampleRawHeight(tile->second->raw, worldX, worldZ);
        }
    }
    return std::nullopt;
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
                const auto sourceElevation = raw.renderElevations.size() == raw.elevations.size()
                    ? static_cast<double>(raw.renderElevations[index])
                    : static_cast<double>(raw.elevations[index]);
                const auto elevation = sourceElevation * sourceScale;
                if (elevation < 4.0 || elevation > 3'200.0) continue;
                const auto worldX = raw.origin.x + static_cast<double>(x) * spacing;
                const auto worldZ = raw.origin.y + static_cast<double>(z) * spacing;
                const auto distance = std::hypot(worldX - center.x, worldZ - center.z);
                if (distance > maximumDistance) continue;
                const auto sourceAt = [&raw](const std::size_t sampleIndex) {
                    return raw.renderElevations.size() == raw.elevations.size()
                        ? static_cast<double>(raw.renderElevations[sampleIndex])
                        : static_cast<double>(raw.elevations[sampleIndex]);
                };
                const auto riseX = std::abs(sourceAt(index + 1U)
                    - sourceAt(index - 1U)) * sourceScale / (spacing * 2.0);
                const auto riseZ = std::abs(sourceAt(index + static_cast<std::size_t>(raw.width))
                    - sourceAt(index - static_cast<std::size_t>(raw.width))) * sourceScale / (spacing * 2.0);
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
    for (const auto& spec : specs_) {
        const TileKey key{
            spec.level,
            static_cast<std::int64_t>(std::floor(worldX / spec.tileWorldSize)),
            static_cast<std::int64_t>(std::floor(worldZ / spec.tileWorldSize)),
        };
        const auto tile = loaded_.find(key);
        if (tile != loaded_.end() && tile->second
            && contains(tile->second->raw, worldX, worldZ)) {
            return sampleRawHeight(tile->second->raw, worldX, worldZ);
        }
    }
    return std::nullopt;
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
            const auto delta = point.y - static_cast<double>(*height);
            if (delta <= 0.0 && previousDelta > 0.0) {
                double low = previousDistance;
                double high = distance;
                for (std::uint32_t iteration = 0; iteration < 10; ++iteration) {
                    const auto middle = (low + high) * 0.5;
                    const auto middlePoint = origin + ray * middle;
                    const auto middleHeight = sampleRenderedHeight(middlePoint.x, middlePoint.z);
                    const auto middleSurface = middleHeight
                        ? static_cast<double>(*middleHeight)
                        : -std::numeric_limits<double>::infinity();
                    if (middlePoint.y <= middleSurface) high = middle;
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
        if (complete) radius = std::max(radius, presentationRange(spec.level).outerRadius);
    }
    return radius;
}

LodPresentationRange TerrainStreamer::presentationRange(const std::int32_t level) const noexcept {
    if (level < 0 || static_cast<std::size_t>(level) >= presentationRanges_.size()) return {};
    return presentationRanges_[static_cast<std::size_t>(level)];
}

std::size_t TerrainStreamer::pendingTileCount() const {
    return static_cast<std::size_t>(
        std::ranges::count_if(wanted_, [this](const auto& key) {
            return !loaded_.contains(key);
        }));
}

bool TerrainStreamer::hasWantedAncestor(TileKey key) const noexcept {
    const auto parentCoordinate = [](const std::int64_t value) {
        return value >= 0 ? value / 2 : -((-value + 1) / 2);
    };
    while (key.level < specs_.back().level) {
        key = TileKey{
            key.level + 1,
            parentCoordinate(key.x),
            parentCoordinate(key.z),
        };
        if (wanted_.contains(key)) return true;
    }
    return false;
}

std::string TerrainStreamer::debugLodConfiguration() const {
    std::string result;
    for (const auto& spec : specs_) {
        if (!result.empty()) result += " | ";
        const auto source = spec.source == TileSource::Detailed ? "detail"
            : spec.source == TileSource::Latent ? "latent" : "orbital";
        const auto projectionScale = viewportHeightPixels_
            / (2.0 * std::tan(verticalFovRadians_ * 0.5));
        const auto errorMetres = geometricErrorMetres(spec);
        const auto projectedError = errorMetres * projectionScale
            / std::max(lastViewerHeight_, 1.0);
        result += std::format("L{}:{} {:.0f}m err~{:.1f}m/{:.2f}px [{:.1f},{:.1f}]km", spec.level, source,
            spec.spacing, errorMetres, projectedError,
            presentationRange(spec.level).innerRadius / 1'000.0,
            presentationRange(spec.level).outerRadius / 1'000.0);
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
    // Screen-space LOD depends on eye height above the local surface. Flat
    // world coverage is bounded by atmospheric visibility, not a geometric
    // planet horizon.
    const auto lodHeight = std::max(
        1.68, cameraPosition.y - static_cast<double>(localGround.value_or(0.0F)));
    requestedRadius_ = 240'000.0;
    lastViewerHeight_ = lodHeight;

    // Choose the innermost active clipmap from geometric error projected into
    // the actual camera. Detailed levels measure their deviation from cached
    // scale-8 Terrain Diffusion samples; generated latent/orbital levels use
    // conservative source-frequency estimates until a comparable hierarchy
    // is available. Hysteresis makes altitude/FOV threshold crossings stable.
    const auto projectionScale = viewportHeightPixels_
        / (2.0 * std::tan(verticalFovRadians_ * 0.5));
    const auto threshold = std::max(
        static_cast<double>(visuals_.terrain.maxScreenSpaceErrorPixels), 0.25);
    const auto hysteresis = std::clamp(
        static_cast<double>(visuals_.terrain.lodHysteresis), 0.0, 0.95);
    const auto projectedError = [&](const std::int32_t level) {
        return geometricErrorMetres(specFor(level)) * projectionScale / lodHeight;
    };
    const auto maximumLevel = static_cast<std::int32_t>(specs_.size()) - 1;
    const auto previousMinimumLevel = activeMinimumLevel_;
    activeMinimumLevel_ = std::clamp(activeMinimumLevel_, 0, maximumLevel);
    while (activeMinimumLevel_ < maximumLevel
        && projectedError(activeMinimumLevel_ + 1) <= threshold * (1.0 - hysteresis)) {
        ++activeMinimumLevel_;
    }
    while (activeMinimumLevel_ > 0
        && projectedError(activeMinimumLevel_) > threshold * (1.0 + hysteresis)) {
        --activeMinimumLevel_;
    }
    if (activeMinimumLevel_ != previousMinimumLevel) {
        std::fprintf(stderr,
            "Terrain SSE LOD changed: L%d -> L%d, height=%.1fm, error=%.2fpx, threshold=%.2fpx, viewport=%.0fpx\n",
            previousMinimumLevel,
            activeMinimumLevel_,
            lodHeight,
            projectedError(activeMinimumLevel_),
            threshold,
            viewportHeightPixels_);
    }

    // Convert the same projected-error rule into horizontal wandering-clipmap
    // bands. The relevant camera distance is slant range, so altitude removes
    // the horizontal component with Pythagoras. Immutable LodSpec radii remain
    // cache/source metadata; presentation ranges are allowed to evolve.
    std::vector<LodPresentationRange> nextPresentationRanges(specs_.size());
    const auto horizonLimit = requestedRadius_ * 1.08;
    double previousOuter{};
    for (std::int32_t level = 0; level <= maximumLevel; ++level) {
        const auto& spec = specFor(level);
        if (level < activeMinimumLevel_) {
            nextPresentationRanges[static_cast<std::size_t>(level)] = {};
            continue;
        }
        if (level > activeMinimumLevel_ && previousOuter >= horizonLimit - 4.0) {
            nextPresentationRanges[static_cast<std::size_t>(level)] = {
                horizonLimit,
                horizonLimit,
            };
            continue;
        }

        const auto inner = level == activeMinimumLevel_ ? 0.0 : previousOuter * 0.82;
        double outer = horizonLimit;
        if (level < maximumLevel) {
            const auto acceptableSlantDistance = geometricErrorMetres(specFor(level + 1))
                * projectionScale / threshold;
            const auto horizontalDistanceSquared = acceptableSlantDistance * acceptableSlantDistance
                - lodHeight * lodHeight;
            const auto projectedOuter = horizontalDistanceSquared > 0.0
                ? std::sqrt(horizontalDistanceSquared) : 0.0;
            // Authored outer radii are hard streaming budgets. Measured error
            // may pull a transition inward or keep a finer active centre, but
            // one rugged sample must not multiply every tile and churn the
            // bounded scale-8 page cache.
            const auto capacity = spec.outerRadius;
            outer = std::min({
                std::max(projectedOuter, spec.tileWorldSize * 1.05),
                capacity,
                horizonLimit,
            });
        }
        outer = std::max(outer, std::min(inner + spec.tileWorldSize * 0.25, horizonLimit));
        nextPresentationRanges[static_cast<std::size_t>(level)] = {inner, outer};
        previousOuter = outer;
    }

    const auto rangeChanged = presentationRanges_.size() != nextPresentationRanges.size()
        || !std::ranges::equal(presentationRanges_, nextPresentationRanges, [](const auto& left, const auto& right) {
            const auto materiallyDifferent = [](const double a, const double b) {
                return std::abs(a - b) > std::max(4.0, std::max(std::abs(a), std::abs(b)) * 0.005);
            };
            return !materiallyDifferent(left.innerRadius, right.innerRadius)
                && !materiallyDifferent(left.outerRadius, right.outerRadius);
        });
    if (rangeChanged) {
        presentationRanges_ = std::move(nextPresentationRanges);
        ++presentationRevision_;
    }
    // Coverage parents must arrive before their children. This guarantees a
    // complete surface during progressive refinement at every altitude; fine
    // tiles replace an already-visible parent only as complete sibling groups.
    std::unordered_set<TileKey, TileKeyHash> nextWanted;
    for (const auto& spec : specs_) {
        if (spec.level < activeMinimumLevel_) continue;
        const auto range = presentationRange(spec.level);
        if (range.innerRadius >= horizonLimit || range.outerRadius <= range.innerRadius) break;
        const auto outerLimit = std::min(range.outerRadius * 1.05, horizonLimit);
        // The selected minimum level is the centre of the wandering clipmap,
        // not one of its annular parents. It must cover the camera even when
        // finer cached levels are deliberately dormant at this projection.
        const auto innerLimit = spec.level == activeMinimumLevel_
            ? 0.0 : range.innerRadius * 0.72;
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

    // The presentation bands are radial, while crack-free replacement is a
    // quadtree operation. Expand every requested child to its complete sibling
    // group and retain the parent through the coarsest active band. Without
    // this closure a single annular child could suppress only part of a parent,
    // producing the large skirt walls and holes visible at LOD boundaries.
    std::int32_t maximumWantedLevel = activeMinimumLevel_;
    for (const auto& key : nextWanted) {
        maximumWantedLevel = std::max(maximumWantedLevel, key.level);
    }
    const auto parentCoordinate = [](const std::int64_t value) {
        return value >= 0 ? value / 2 : -((-value + 1) / 2);
    };
    for (std::int32_t level = activeMinimumLevel_; level < maximumWantedLevel; ++level) {
        std::vector<TileKey> levelKeys;
        for (const auto& key : nextWanted) {
            if (key.level == level) levelKeys.push_back(key);
        }
        for (const auto& key : levelKeys) {
            const auto parentX = parentCoordinate(key.x);
            const auto parentZ = parentCoordinate(key.z);
            const auto childX = parentX * 2;
            const auto childZ = parentZ * 2;
            nextWanted.insert(TileKey{level, childX, childZ});
            nextWanted.insert(TileKey{level, childX + 1, childZ});
            nextWanted.insert(TileKey{level, childX, childZ + 1});
            nextWanted.insert(TileKey{level, childX + 1, childZ + 1});
            nextWanted.insert(TileKey{level + 1, parentX, parentZ});
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
            releaseResidentRenderBytes(loaded->first);
            loaded_.erase(loaded);
        }
    }

    const auto terrainBudget = visuals_.streaming.terrainMemoryMegabytes * 1024U * 1024U;
    const auto vegetationBudget = visuals_.streaming.vegetationMemoryMegabytes * 1024U * 1024U;
    const auto renderBudget = terrainBudget + vegetationBudget;
    if (performance_.terrainCpuBytes > terrainBudget
        || performance_.vegetationCpuBytes > vegetationBudget
        || performance_.residentRenderBytes > renderBudget) {
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
                || performance_.vegetationCpuBytes > vegetationBudget
                || performance_.residentRenderBytes > renderBudget); ++iterator) {
            const auto loaded = loaded_.find(iterator->key);
            if (loaded == loaded_.end()) continue;
            deferredEvents_.push_back(TerrainEvent{.type = TerrainEvent::Type::Removed, .key = loaded->first});
            performance_.terrainCpuBytes -= terrainBytes(*loaded->second);
            performance_.vegetationCpuBytes -= vegetationBytes(*loaded->second);
            releaseResidentRenderBytes(loaded->first);
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
                request.priority = requestPriority(request.key, request.spec, cameraPosition);
                retained.push(std::move(request));
            } else {
                queued_.erase(request.key);
            }
        }
        requests_ = std::move(retained);
    }
    wanted_ = std::move(nextWanted);
}

double TerrainStreamer::requestPriority(
    const TileKey& key,
    const LodSpec& spec,
    const glm::dvec3& cameraPosition) const {
    const auto minimumX = static_cast<double>(key.x) * spec.tileWorldSize;
    const auto minimumZ = static_cast<double>(key.z) * spec.tileWorldSize;
    const auto maximumX = minimumX + spec.tileWorldSize;
    const auto maximumZ = minimumZ + spec.tileWorldSize;
    const auto closestX = std::clamp(cameraPosition.x, minimumX, maximumX);
    const auto closestZ = std::clamp(cameraPosition.z, minimumZ, maximumZ);

    // Parent readiness is enforced by scheduleMissing(). Among the eligible
    // tiles, footprint distance produces a radial, player-outward load order.
    return std::hypot(closestX - cameraPosition.x, closestZ - cameraPosition.z);
}

void TerrainStreamer::scheduleMissing(const glm::dvec3& cameraPosition) {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{workMutex_};
    const auto queueCapacity = std::max<std::size_t>(
        static_cast<std::size_t>(visuals_.streaming.maxConcurrentRequests) * 8U,
        16U);
    if (queued_.size() >= queueCapacity) return;

    std::int32_t maximumWantedLevel = activeMinimumLevel_;
    for (const auto& key : wanted_) maximumWantedLevel = std::max(maximumWantedLevel, key.level);
    const auto parentCoordinate = [](const std::int64_t value) {
        return value >= 0 ? value / 2 : -((-value + 1) / 2);
    };
    struct Candidate final {
        TileKey key;
        double priority{};
    };
    std::vector<Candidate> candidates;
    candidates.reserve(wanted_.size());
    for (const auto& key : wanted_) {
        if (loaded_.contains(key) || queued_.contains(key)) continue;
        const auto retry = retryAfter_.find(key);
        if (retry != retryAfter_.end() && retry->second > now) continue;
        if (key.level < maximumWantedLevel) {
            const TileKey parent{
                key.level + 1,
                parentCoordinate(key.x),
                parentCoordinate(key.z),
            };
            if (wanted_.contains(parent) && !loaded_.contains(parent)) continue;
        }
        const auto& spec = specFor(key.level);
        candidates.push_back({key, requestPriority(key, spec, cameraPosition)});
    }
    std::ranges::sort(candidates, {}, &Candidate::priority);
    const auto available = queueCapacity - queued_.size();
    const auto count = std::min(available, candidates.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& candidate = candidates[index];
        const auto& spec = specFor(candidate.key.level);
        requests_.push(Request{
            candidate.key,
            spec,
            generation_,
            seed_,
            candidate.priority,
        });
        queued_.insert(candidate.key);
    }
    if (count > 0U) workReady_.notify_all();
}

std::vector<LodSpec> TerrainStreamer::makeSpecs(const WorldInfo& world) {
    std::vector<LodSpec> specs;
    constexpr std::int32_t scale = 8;
    constexpr std::int32_t baseSegments = 128;
    constexpr std::int32_t levelCount = 10;
    constexpr double baseOuterRadius = 960.0;
    const auto finestSpacing = static_cast<double>(world.nativeResolution) / scale;
    double previousOuter{};
    for (std::int32_t level = 0; level < levelCount; ++level) {
        // Keep full density underfoot and enough density in the first coarse
        // ring for silhouettes. More distant rings otherwise feed sub-pixel
        // triangles to RenderingServer and occasionally exceed the main-thread
        // upload budget. The measured geometric error/SSE logic still decides
        // how close each representation may be shown.
        // GPU heightmap tiles retain a dense near patch, then halve grid
        // density aggressively because displacement and normal reconstruction
        // happen in the shader. Far rings no longer submit millions of unique
        // CPU-authored vertices whose projected triangles are sub-pixel.
        // Keep full density near the viewer, then halve the grid through the
        // middle and horizon bands. Tile world sizes remain unchanged, so
        // only geometry density changes.
        const std::int32_t segments = level == 0 ? 128
            : level <= 4 ? 64
            : level <= 7 ? 32
            : 16;
        const auto originalLevelTileSize = finestSpacing
            * static_cast<double>(baseSegments)
            * static_cast<double>(std::uint64_t{1} << level);
        TileSource source{TileSource::Detailed};
        // Every detailed LOD samples one canonical native-resolution height
        // field in TerrainClient. Fine levels interpolate it locally and
        // coarse levels decimate it, so shared vertices remain identical and
        // no ring needs a 4096-square upsampled source page.
        auto sourceScale = 1;
        auto spacing = originalLevelTileSize / static_cast<double>(segments);
        auto stride = static_cast<std::int32_t>(std::llround(spacing / finestSpacing));
        if (level >= 6 && level <= 9) {
            // Exact source pages remain practical through L5. Past that, a
            // single render tile would touch 4, 16, 64... independent 32 MiB
            // pages. Terrain Diffusion's generated latent stage is the bounded
            // horizon preview while detailed pages stream near the viewer.
            source = TileSource::Latent;
            sourceScale = 1;
            const auto originalStride = 1 << (level - 6);
            const auto originalTileSize = static_cast<double>(world.latentResolution)
                * static_cast<double>(originalStride * baseSegments);
            spacing = originalTileSize / static_cast<double>(segments);
            stride = std::max(1, static_cast<std::int32_t>(std::llround(
                spacing / static_cast<double>(world.latentResolution))));
        }
        const auto tileSize = spacing * segments;
        auto outer = baseOuterRadius * std::pow(2.0, level);
        // Ten levels cover the finite map and the physical horizon at the
        // 15 km aircraft ceiling. The obsolete orbital shell is intentionally
        // absent.
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
    const auto sourceAt = [&tile, visibleWidth, visibleHeight](std::int32_t sx, std::int32_t sz) {
        sx = std::clamp(sx, -tile.borderSamples, visibleWidth - 1 + tile.borderSamples);
        sz = std::clamp(sz, -tile.borderSamples, visibleHeight - 1 + tile.borderSamples);
        const auto rawX = sx + tile.borderSamples;
        const auto rawZ = sz + tile.borderSamples;
        const auto index = static_cast<std::size_t>(rawZ * tile.width + rawX);
        return tile.renderElevations.size() == tile.elevations.size()
            ? tile.renderElevations[index]
            : static_cast<float>(tile.elevations[index]);
    };
    const auto at = [&tile, &sourceAt](std::int32_t sx, std::int32_t sz) {
        if (tile.spec.source != TileSource::Detailed || tile.spec.stride != 1
            || !tile.renderElevations.empty()) {
            return sourceAt(sx, sz);
        }
        constexpr std::array<float, 3> weights{1.0F, 2.0F, 1.0F};
        float result{};
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                result += sourceAt(sx + dx, sz + dz)
                    * weights[static_cast<std::size_t>(dx + 1)]
                    * weights[static_cast<std::size_t>(dz + 1)];
            }
        }
        return result * (1.0F / 16.0F);
    };
    const auto a = std::lerp(at(x0, z0), at(x0 + 1, z0), tx);
    const auto b = std::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx);
    return std::lerp(a, b, tz) * visuals_.metrics.elevationMetersPerSourceUnit
        * visuals_.metrics.verticalExaggeration;
}

double TerrainStreamer::geometricErrorMetres(const LodSpec& spec) const noexcept {
    const auto verticalScale = std::abs(
        static_cast<double>(visuals_.metrics.elevationMetersPerSourceUnit)
        * static_cast<double>(visuals_.metrics.verticalExaggeration));
    const auto level = static_cast<std::size_t>(std::max(spec.level, 0));
    const auto measuredSourceError = level < measuredGeometricErrorSourceUnits_.size()
        ? measuredGeometricErrorSourceUnits_[level] : 0.0;
    const auto measuredError = measuredSourceError * verticalScale;

    // These are conservative startup estimates, not replacement terrain.
    // Measured detailed-page error can only raise them as real tiles arrive.
    double fallbackError{};
    if (spec.source == TileSource::Detailed) {
        fallbackError = spec.level == 0 ? 0.5 * verticalScale
            : spec.spacing * 0.5 * std::abs(static_cast<double>(visuals_.metrics.verticalExaggeration));
    } else if (spec.source == TileSource::Latent) {
        fallbackError = spec.spacing * std::abs(static_cast<double>(visuals_.metrics.verticalExaggeration));
    } else {
        fallbackError = spec.spacing * 1.5
            * std::abs(static_cast<double>(visuals_.metrics.verticalExaggeration));
    }
    return std::max(measuredError, fallbackError);
}

} // namespace terrain
