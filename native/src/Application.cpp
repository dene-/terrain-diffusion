#include "Application.hpp"

#include "TerrainClient.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace terrain {

Application::Application(Config config)
    : config_(std::move(config)), renderer_(config_), controller_(config_) {}

Application::~Application() {
    streamer_.reset();
    renderer_.shutdown();
    if (window_) SDL_DestroyWindow(window_);
    if (sdlInitialized_) SDL_Quit();
}

int Application::run() {
    initialize();
    bool running = true;
    auto previous = std::chrono::steady_clock::now();
    startTime_ = previous;
    while (running) {
        const auto now = std::chrono::steady_clock::now();
        const auto delta = std::chrono::duration<double>(now - previous).count();
        previous = now;
        processEvents(running);
        if (!running) break;
        update(delta);
        updateWindowTitle(delta);
    }
    return 0;
}

void Application::initialize() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        throw std::runtime_error(std::string{"SDL initialization failed: "} + SDL_GetError());
    }
    sdlInitialized_ = true;
    window_ = SDL_CreateWindow(
        "Terrain Native — connecting to Terrain Diffusion",
        static_cast<int>(config_.windowWidth), static_cast<int>(config_.windowHeight),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window_) throw std::runtime_error(std::string{"Window creation failed: "} + SDL_GetError());
    renderer_.initialize(window_);

    TerrainClient client{config_.serverUrl};
    auto world = client.fetchWorld();
    SDL_Log("Terrain Diffusion seed: %s", world.seed.c_str());
    SDL_Log("Native renderer: %s", renderer_.driverName());
    streamer_ = std::make_unique<TerrainStreamer>(std::move(client), std::move(world), config_.visuals);
    SDL_Log("Terrain LOD configuration: %s", streamer_->debugLodConfiguration().c_str());
    controller_.setMouseCaptured(false);
}

void Application::processEvents(bool& running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        const auto action = controller_.handleEvent(event);
        if (hasAction(action, InputAction::Quit)) running = false;
        if (hasAction(action, InputAction::Regenerate) && spawned_) regenerate();
        if (hasAction(action, InputAction::ToggleCapture)) toggleMouseCapture();
        if (hasAction(action, InputAction::DebugSnapshot)) logDebugSnapshot();
        if (hasAction(action, InputAction::CycleDebugView)) {
            debugView_ = (debugView_ + 1U) % 14U;
            SDL_Log("Terrain debug view: %u", debugView_);
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && spawned_) {
            if (event.key.scancode == SDL_SCANCODE_K) {
                cloudsEnabled_ = !cloudsEnabled_;
                renderer_.setCloudsEnabled(cloudsEnabled_);
                SDL_Log("Cloud rendering: %s", cloudsEnabled_ ? "enabled" : "disabled");
            }
            if (event.key.scancode >= SDL_SCANCODE_1 && event.key.scancode <= SDL_SCANCODE_6) {
                applyCameraBookmark(static_cast<std::uint32_t>(event.key.scancode - SDL_SCANCODE_1 + 1));
            }
            if (event.key.scancode == SDL_SCANCODE_T) {
                visualTuningVisible_ = true;
                visualTuningParameter_ = (visualTuningParameter_ + 1U) % 25U;
                SDL_Log("Visual tuning: %s", visualTuningLabel().c_str());
            }
            if (event.key.scancode == SDL_SCANCODE_LEFTBRACKET) adjustVisualTuning(-1.0F);
            if (event.key.scancode == SDL_SCANCODE_RIGHTBRACKET) adjustVisualTuning(1.0F);
            if (event.key.scancode == SDL_SCANCODE_P) {
                config_.visuals.save(config_.visualConfigPath);
                SDL_Log("Saved visual configuration to %s", config_.visualConfigPath.string().c_str());
            }
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && !mouseCaptured_) {
            handleHudClick(event.button.x, event.button.y);
        }
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) renderer_.resize();
    }
}

void Application::adjustVisualTuning(float direction) {
    visualTuningVisible_ = true;
    auto& visuals = config_.visuals;
    const auto rebuildDerived = visualTuningParameter_ <= 9U;
    switch (visualTuningParameter_) {
    case 0: visuals.metrics.verticalExaggeration = std::clamp(
        visuals.metrics.verticalExaggeration + direction * 0.05F, 0.5F, 2.0F); break;
    case 1: visuals.terrain.rockSlopeStart = std::clamp(
        visuals.terrain.rockSlopeStart + direction * 0.02F, 0.0F,
        visuals.terrain.rockSlopeFull - 0.02F); break;
    case 2: visuals.terrain.rockSlopeFull = std::clamp(
        visuals.terrain.rockSlopeFull + direction * 0.02F,
        visuals.terrain.rockSlopeStart + 0.02F, 1.0F); break;
    case 3: visuals.terrain.screeSlopeStart = std::clamp(
        visuals.terrain.screeSlopeStart + direction * 0.02F, 0.0F,
        visuals.terrain.screeSlopeFull - 0.02F); break;
    case 4: visuals.terrain.screeSlopeFull = std::clamp(
        visuals.terrain.screeSlopeFull + direction * 0.02F,
        visuals.terrain.screeSlopeStart + 0.02F, 1.0F); break;
    case 5: visuals.forest.globalDensity = std::clamp(
        visuals.forest.globalDensity + direction * 0.04F, 0.0F, 1.0F); break;
    case 6: visuals.forest.clearingScaleMeters = std::clamp(
        visuals.forest.clearingScaleMeters * std::pow(1.15F, direction), 80.0F, 4'000.0F); break;
    case 7: visuals.forest.minimumTemperature = std::clamp(
        visuals.forest.minimumTemperature + direction, -30.0F, 12.0F); break;
    case 8: visuals.forest.treeLineStartMeters = std::clamp(
        visuals.forest.treeLineStartMeters + direction * 100.0F, 0.0F,
        visuals.forest.treeLineEndMeters - 100.0F); break;
    case 9: visuals.forest.treeLineEndMeters = std::clamp(
        visuals.forest.treeLineEndMeters + direction * 100.0F,
        visuals.forest.treeLineStartMeters + 100.0F, 8'000.0F); break;
    case 10: visuals.forest.billboardSizeMultiplier = std::clamp(
        visuals.forest.billboardSizeMultiplier + direction * 0.05F, 0.5F, 1.8F); break;
    case 11: visuals.forest.nearPixelThreshold = std::clamp(
        visuals.forest.nearPixelThreshold + direction, visuals.forest.midPixelThreshold + 0.5F, 80.0F); break;
    case 12: visuals.forest.midPixelThreshold = std::clamp(
        visuals.forest.midPixelThreshold + direction * 0.25F,
        visuals.forest.geometryCullPixelThreshold + 0.2F,
        visuals.forest.nearPixelThreshold - 0.5F); break;
    case 13: visuals.forest.geometryCullPixelThreshold = std::clamp(
        visuals.forest.geometryCullPixelThreshold + direction * 0.1F, 0.2F,
        visuals.forest.midPixelThreshold - 0.2F); break;
    case 14: visuals.terrain.maxScreenSpaceErrorPixels = std::clamp(
        visuals.terrain.maxScreenSpaceErrorPixels + direction * 0.2F, 0.5F, 8.0F); break;
    case 15: visuals.atmosphere.distanceDensity = std::clamp(
        visuals.atmosphere.distanceDensity * std::pow(1.18F, direction), 0.000001F, 0.002F); break;
    case 16: visuals.atmosphere.heightFalloff = std::clamp(
        visuals.atmosphere.heightFalloff * std::pow(1.18F, direction), 0.000001F, 0.002F); break;
    case 17: visuals.lighting.sunAzimuthDegrees = std::fmod(
        visuals.lighting.sunAzimuthDegrees + direction * 5.0F + 360.0F, 360.0F); break;
    case 18: visuals.lighting.sunElevationDegrees = std::clamp(
        visuals.lighting.sunElevationDegrees + direction * 3.0F, -8.0F, 88.0F); break;
    case 19: visuals.lighting.skyIntensity = std::clamp(
        visuals.lighting.skyIntensity + direction * 0.05F, 0.05F, 3.0F); break;
    case 20: visuals.lighting.exposure = std::clamp(
        visuals.lighting.exposure + direction * 0.05F, 0.1F, 4.0F); break;
    case 21: visuals.atmosphere.turbidity = std::clamp(
        visuals.atmosphere.turbidity + direction * 0.2F, 1.0F, 8.0F); break;
    case 22: visuals.clouds.coverage = std::clamp(
        visuals.clouds.coverage + direction * 0.025F, 0.0F, 1.0F); break;
    case 23: visuals.clouds.density = std::clamp(
        visuals.clouds.density + direction * 0.025F, 0.0F, 1.0F); break;
    case 24: {
        const auto factor = std::pow(1.16F, direction);
        visuals.clouds.weatherScaleMeters = std::clamp(
            visuals.clouds.weatherScaleMeters * factor, 4'000.0F, 120'000.0F);
        visuals.clouds.detailScaleMeters = std::clamp(
            visuals.clouds.detailScaleMeters * factor, 600.0F, 24'000.0F);
        break;
    }
    default: break;
    }
    renderer_.setVisualConfig(visuals);
    streamer_->setVisualConfig(visuals, rebuildDerived);
    SDL_Log("Visual tuning: %s", visualTuningLabel().c_str());
}

std::string Application::visualTuningLabel() const {
    const auto& visuals = config_.visuals;
    switch (visualTuningParameter_) {
    case 0: return std::format("TUNE VERTICAL SCALE {:.2f}", visuals.metrics.verticalExaggeration);
    case 1: return std::format("TUNE ROCK START {:.2f}", visuals.terrain.rockSlopeStart);
    case 2: return std::format("TUNE ROCK FULL {:.2f}", visuals.terrain.rockSlopeFull);
    case 3: return std::format("TUNE SCREE START {:.2f}", visuals.terrain.screeSlopeStart);
    case 4: return std::format("TUNE SCREE FULL {:.2f}", visuals.terrain.screeSlopeFull);
    case 5: return std::format("TUNE FOREST DENSITY {:.2f}", visuals.forest.globalDensity);
    case 6: return std::format("TUNE CLEARING SCALE {:.0f} M", visuals.forest.clearingScaleMeters);
    case 7: return std::format("TUNE TREE MIN TEMP {:.0f} C", visuals.forest.minimumTemperature);
    case 8: return std::format("TUNE TREE LINE START {:.0f} M", visuals.forest.treeLineStartMeters);
    case 9: return std::format("TUNE TREE LINE END {:.0f} M", visuals.forest.treeLineEndMeters);
    case 10: return std::format("TUNE TREE SIZE {:.2f}", visuals.forest.billboardSizeMultiplier);
    case 11: return std::format("TUNE TREE NEAR {:.1f} PX", visuals.forest.nearPixelThreshold);
    case 12: return std::format("TUNE TREE MID {:.1f} PX", visuals.forest.midPixelThreshold);
    case 13: return std::format("TUNE TREE CULL {:.2f} PX", visuals.forest.geometryCullPixelThreshold);
    case 14: return std::format("TUNE TERRAIN SSE {:.1f} PX", visuals.terrain.maxScreenSpaceErrorPixels);
    case 15: return std::format("TUNE FOG DENSITY {:.7f}", visuals.atmosphere.distanceDensity);
    case 16: return std::format("TUNE FOG HEIGHT {:.7f}", visuals.atmosphere.heightFalloff);
    case 17: return std::format("TUNE SUN AZIMUTH {:.0f} DEG", visuals.lighting.sunAzimuthDegrees);
    case 18: return std::format("TUNE SUN ELEVATION {:.0f} DEG", visuals.lighting.sunElevationDegrees);
    case 19: return std::format("TUNE SKY INTENSITY {:.2f}", visuals.lighting.skyIntensity);
    case 20: return std::format("TUNE EXPOSURE {:.2f}", visuals.lighting.exposure);
    case 21: return std::format("TUNE TURBIDITY {:.1f}", visuals.atmosphere.turbidity);
    case 22: return std::format("TUNE CLOUD COVERAGE {:.2f}", visuals.clouds.coverage);
    case 23: return std::format("TUNE CLOUD DENSITY {:.2f}", visuals.clouds.density);
    case 24: return std::format("TUNE CLOUD SCALE {:.0f} M", visuals.clouds.weatherScaleMeters);
    default: return {};
    }
}

void Application::applyCameraBookmark(std::uint32_t bookmark) {
    auto position = controller_.position();
    const auto ground = streamer_->sampleHeight(position.x, position.z).value_or(0.0F);
    // The two orbital bookmarks deliberately share altitude: 5 is a nadir
    // coverage/LOD inspection and 6 is a horizon/curvature inspection. This
    // makes visual validation repeatable without relying on pointer capture.
    static constexpr std::array<double, 6> altitudes{
        1.68, 35.0, 3'000.0, 30'000.0, 160.0, 800'000.0};
    const auto index = std::min<std::size_t>(bookmark - 1U, altitudes.size() - 1U);
    position.y = static_cast<double>(ground) + altitudes[index];
    controller_.setPosition(position);
    controller_.setFlying(bookmark != 1U);
    const auto pitch = bookmark <= 2U ? -0.10
        : bookmark == 3U ? -0.48
        : bookmark == 4U ? -0.88
        : bookmark == 5U ? -1.32
        : -0.14;
    controller_.setLookDirection(0.0, pitch);
    SDL_Log("Camera bookmark %u: altitude %.0f m, pitch %.2f rad", bookmark, altitudes[index], pitch);
}

void Application::logDebugSnapshot() const {
    const auto& position = controller_.position();
    const auto forward = controller_.forward();
    const auto ground = streamer_->sampleHeight(position.x, position.z);
    const auto groundText = ground ? std::format("{:.2f}", *ground) : "missing";
    const auto aglText = ground ? std::format("{:.2f}", position.y - static_cast<double>(*ground)) : "unknown";
    SDL_Log(
        "DIAGNOSTIC camera=(%.2f, %.2f, %.2f) forward=(%.4f, %.4f, %.4f) ground=%s agl=%s "
        "mode=%s radius=%.1f/%.1fkm tiles=%zu pending=%zu stack=[%s]",
        position.x, position.y, position.z, forward.x, forward.y, forward.z,
        groundText.c_str(), aglText.c_str(), controller_.flying() ? "fly" : "walk",
        streamer_->installedRadius() / 1'000.0, streamer_->requestedRadius() / 1'000.0,
        streamer_->loadedTileCount(), streamer_->pendingTileCount(),
        streamer_->debugHeightStack(position.x, position.z).c_str());
}

void Application::update(double deltaSeconds) {
    auto streamPosition = controller_.position();
    if (!spawned_) {
        // The controller waits safely above unknown terrain during spawn
        // search, but that provisional height must not expand the clipmap to
        // an aircraft-sized horizon. Bootstrap the single local scale-8 page;
        // the real altitude takes over as soon as a grounded spawn exists.
        streamPosition.y = 1.68;
    }
    streamer_->update(streamPosition, controller_.forward());
    for (auto& event : streamer_->drainEvents(
        config_.visuals.streaming.maxGpuUploadsPerFrame,
        config_.visuals.streaming.maxGpuUploadBytesPerFrame)) {
        if (event.type == TerrainEvent::Type::Added && event.tile) {
            const auto uploadStart = std::chrono::steady_clock::now();
            renderer_.addTile(event.tile);
            const auto uploadMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - uploadStart).count();
            averageGpuUploadMilliseconds_ = averageGpuUploadMilliseconds_ == 0.0
                ? uploadMilliseconds
                : std::lerp(averageGpuUploadMilliseconds_, uploadMilliseconds, 0.12);
            streamer_->releaseUploadedPayload(event.key);
        }
        else if (event.type == TerrainEvent::Type::Removed) renderer_.removeTile(event.key);
        else if (event.type == TerrainEvent::Type::Failed) SDL_Log("Terrain stream: %s", event.message.c_str());
    }

    if (!spawned_ && streamer_->readyNearPlayer(controller_.position())) {
        if (const auto spawn = findSafeSpawn()) {
            controller_.setPosition(*spawn);
            controller_.setFlying(false);
            spawned_ = true;
            SDL_Log("Spawn ready at %.0f, %.0f m (elevation %.0f m) after %u relocation%s",
                spawn->x, spawn->z, spawn->y - config_.eyeHeight, spawnSearchAttempts_,
                spawnSearchAttempts_ == 1 ? "" : "s");
        } else {
            relocateSpawnSearch();
        }
    }
    controller_.update(deltaSeconds, *streamer_);

    int width{};
    int height{};
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    auto camera = controller_.camera(
        static_cast<std::uint32_t>(std::max(width, 1)),
        static_cast<std::uint32_t>(std::max(height, 1)));
    if (const auto ground = streamer_->sampleHeight(camera.absolutePosition.x, camera.absolutePosition.z)) {
        camera.altitudeAboveGround = static_cast<float>(camera.absolutePosition.y - *ground);
    } else {
        camera.altitudeAboveGround = static_cast<float>(camera.absolutePosition.y);
    }
    const auto elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime_).count();
    HudState hud;
    hud.seed = std::to_string(streamer_->seed());
    hud.message = spawned_
        ? streamer_->pendingTileCount() > 0 ? "STREAMING TERRAIN" : "TERRAIN READY"
        : spawnSearchAttempts_ == 0
            ? "GENERATING FIRST TERRAIN"
            : std::format("SEARCHING LAND REGION {}", spawnSearchAttempts_);
    hud.position = controller_.position();
    hud.aimDistance = measuredDistance_;
    hud.installedRadius = streamer_->installedRadius();
    hud.requestedRadius = streamer_->requestedRadius();
    hud.flySpeed = controller_.flySpeed();
    hud.fps = static_cast<float>(lastFps_);
    hud.onePercentLowFps = static_cast<float>(onePercentLowFps_);
    hud.loadedTiles = streamer_->loadedTileCount();
    hud.pendingTiles = streamer_->pendingTileCount();
    hud.trees = renderer_.treeCount();
    hud.grass = renderer_.grassCount();
    hud.drawCalls = renderer_.drawCallCount();
    hud.terrainDraws = renderer_.terrainTileDrawCount();
    const auto& performance = streamer_->performance();
    hud.streamFetchMilliseconds = performance.averageFetchMilliseconds;
    hud.streamDecodeMilliseconds = performance.averageDecodeMilliseconds;
    hud.streamDerivedMilliseconds = performance.averageDerivedMilliseconds;
    hud.streamVegetationMilliseconds = performance.averageVegetationMilliseconds;
    hud.streamBuildMilliseconds = performance.averageBuildMilliseconds;
    hud.gpuUploadMilliseconds = averageGpuUploadMilliseconds_;
    hud.terrainMemoryMegabytes = static_cast<double>(performance.terrainCpuBytes) / (1024.0 * 1024.0);
    hud.vegetationMemoryMegabytes = static_cast<double>(performance.vegetationCpuBytes) / (1024.0 * 1024.0);
    hud.flying = controller_.flying();
    hud.ready = spawned_;
    hud.captured = mouseCaptured_;
    hud.cloudsEnabled = cloudsEnabled_;
    hud.tuningLabel = visualTuningVisible_ ? visualTuningLabel() : std::string{};
    hud.debugView = debugView_;
    renderer_.render(camera, elapsed, hud);

    titleAccumulator_ += deltaSeconds;
    if (titleAccumulator_ >= 0.18) {
        measuredDistance_ = streamer_->measureRay(
            camera.absolutePosition,
            glm::dvec3{camera.forward},
            std::min(streamer_->requestedRadius() * 1.05, 800'000.0));
    }
}

void Application::updateWindowTitle(double deltaSeconds) {
    frameTimeSamples_[frameTimeSampleIndex_] = std::clamp(deltaSeconds, 0.0, 0.25);
    frameTimeSampleIndex_ = (frameTimeSampleIndex_ + 1U) % frameTimeSamples_.size();
    frameTimeSampleCount_ = std::min(frameTimeSampleCount_ + 1U, frameTimeSamples_.size());
    ++frameCounter_;
    frameAccumulator_ += deltaSeconds;
    if (frameAccumulator_ >= 0.5) {
        lastFps_ = static_cast<double>(frameCounter_) / frameAccumulator_;
        if (frameTimeSampleCount_ >= 30U) {
            std::vector<double> ordered(
                frameTimeSamples_.begin(), frameTimeSamples_.begin() + static_cast<std::ptrdiff_t>(frameTimeSampleCount_));
            std::ranges::sort(ordered);
            const auto percentileIndex = static_cast<std::size_t>(
                std::floor(static_cast<double>(ordered.size() - 1U) * 0.99));
            onePercentLowFps_ = 1.0 / std::max(ordered[percentileIndex], 0.0001);
        }
        frameCounter_ = 0;
        frameAccumulator_ = 0.0;
    }
    if (titleAccumulator_ < 0.18) return;
    titleAccumulator_ = 0.0;
    const auto position = controller_.position();
    const auto ground = streamer_->sampleHeight(position.x, position.z);
    const auto altitude = position.y - static_cast<double>(ground.value_or(0.0F));
    const auto measurement = !measuredDistance_
        ? std::string{}
        : *measuredDistance_ < 1'000.0
            ? std::format(" | aim {:.1f} m", *measuredDistance_)
            : std::format(" | aim {:.2f} km", *measuredDistance_ / 1'000.0);
    const auto title = std::format(
        "Terrain Native [{}] | {:.0f} fps | {} | alt {:.0f} m | speed {:.1f} m/s{} | LOD {:.0f}/{:.0f} km | {} tiles + {} loading | {} trees | {} grass | WASD, Shift, Space, C, F, wheel, R, Esc",
        renderer_.driverName(), lastFps_, controller_.flying() ? "FLY" : "WALK", altitude,
        controller_.flying() ? controller_.flySpeed() : controller_.horizontalSpeed(), measurement,
        streamer_->installedRadius() / 1'000.0, streamer_->requestedRadius() / 1'000.0,
        streamer_->loadedTileCount(), streamer_->pendingTileCount(), renderer_.treeCount(), renderer_.grassCount());
    SDL_SetWindowTitle(window_, title.c_str());
}

void Application::regenerate() {
    std::random_device random;
    const auto seed = (static_cast<std::uint64_t>(random()) << 32U) ^ random();
    renderer_.clearTiles();
    streamer_->regenerate(seed);
    controller_.setPosition(glm::dvec3{240.0, 600.0, 240.0});
    controller_.setFlying(true);
    spawned_ = false;
    spawnSearchAttempts_ = 0;
    if (mouseCaptured_) toggleMouseCapture();
}

void Application::toggleMouseCapture() {
    if (!mouseCaptured_ && !spawned_) return;
    mouseCaptured_ = !mouseCaptured_;
    if (!SDL_SetWindowRelativeMouseMode(window_, mouseCaptured_)) {
        SDL_Log("Could not change relative mouse mode: %s", SDL_GetError());
        mouseCaptured_ = !mouseCaptured_;
    }
    controller_.setMouseCaptured(mouseCaptured_);
}

void Application::handleHudClick(float x, float y) {
    int width{};
    SDL_GetWindowSize(window_, &width, nullptr);
    if (spawned_ && x >= static_cast<float>(width - 270) && y <= 70.0F) {
        regenerate();
        return;
    }
    if (spawned_) toggleMouseCapture();
}

void Application::relocateSpawnSearch() {
    const auto center = controller_.position();
    std::optional<glm::dvec3> target;
    if (spawnSearchAttempts_ == 0) {
        // L1 is generated by the same detailed decoder at native spacing. It
        // is accurate enough to select nearby land, after which the streamer
        // installs L0 before the final spawn is accepted.
        target = streamer_->findWalkableLand(center, 5'800.0, false);
    }
    if (!target) {
        std::mt19937_64 generator{
            streamer_->seed() ^ (0x9e3779b97f4a7c15ULL * static_cast<std::uint64_t>(spawnSearchAttempts_ + 1U))};
        std::uniform_real_distribution<double> angleDistribution{0.0, std::numbers::pi_v<double> * 2.0};
        std::uniform_real_distribution<double> distanceDistribution{20'000.0, 220'000.0};
        const auto angle = angleDistribution(generator);
        const auto distance = distanceDistribution(generator);
        target = glm::dvec3{
            center.x + std::cos(angle) * distance,
            0.0,
            center.z + std::sin(angle) * distance,
        };
    }
    ++spawnSearchAttempts_;
    controller_.setPosition(glm::dvec3{target->x, 1'200.0, target->z});
    controller_.setFlying(true);
    SDL_Log("Spawn search: loading land candidate %u at %.0f, %.0f",
        spawnSearchAttempts_, target->x, target->z);
}

std::optional<glm::dvec3> Application::findSafeSpawn() const {
    const auto land = streamer_->findWalkableLand(controller_.position(), 820.0, true);
    if (!land) return std::nullopt;
    return glm::dvec3{land->x, land->y + config_.eyeHeight, land->z};
}

} // namespace terrain
