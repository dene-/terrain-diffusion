#pragma once

#include "CameraController.hpp"
#include "Config.hpp"
#include "Renderer.hpp"
#include "TerrainStreamer.hpp"

#include <SDL3/SDL_video.h>

#include <chrono>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace terrain {

class Application final {
public:
    explicit Application(Config config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();

private:
    void initialize();
    void processEvents(bool& running);
    void update(double deltaSeconds);
    void updateWindowTitle(double deltaSeconds);
    void regenerate();
    void relocateSpawnSearch();
    void toggleMouseCapture();
    void handleHudClick(float x, float y);
    void logDebugSnapshot() const;
    void applyCameraBookmark(std::uint32_t bookmark);
    void adjustVisualTuning(float direction);
    [[nodiscard]] std::string visualTuningLabel() const;
    [[nodiscard]] std::optional<glm::dvec3> findSafeSpawn() const;

    Config config_;
    SDL_Window* window_{};
    Renderer renderer_;
    CameraController controller_;
    std::unique_ptr<TerrainStreamer> streamer_;
    std::chrono::steady_clock::time_point startTime_{};
    bool sdlInitialized_{};
    bool mouseCaptured_{};
    bool spawned_{};
    std::uint32_t spawnSearchAttempts_{};
    double titleAccumulator_{};
    double frameAccumulator_{};
    std::uint32_t frameCounter_{};
    double lastFps_{};
    double onePercentLowFps_{};
    double averageGpuUploadMilliseconds_{};
    std::array<double, 240> frameTimeSamples_{};
    std::size_t frameTimeSampleCount_{};
    std::size_t frameTimeSampleIndex_{};
    std::optional<double> measuredDistance_;
    std::uint32_t debugView_{};
    bool cloudsEnabled_{true};
    std::uint32_t visualTuningParameter_{24U};
    bool visualTuningVisible_{};
};

} // namespace terrain
