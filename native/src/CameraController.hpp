#pragma once

#include "Config.hpp"
#include <terrain/TerrainStreamer.hpp>
#include <terrain/TerrainTypes.hpp>

#include <SDL3/SDL_events.h>

#include <cstdint>

namespace terrain {

enum class InputAction : std::uint8_t {
    None = 0,
    Quit = 1U << 0U,
    Regenerate = 1U << 1U,
    ToggleCapture = 1U << 2U,
    DebugSnapshot = 1U << 3U,
    CycleDebugView = 1U << 4U,
};

[[nodiscard]] constexpr InputAction operator|(InputAction left, InputAction right) noexcept {
    return static_cast<InputAction>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
    }

[[nodiscard]] constexpr bool hasAction(InputAction value, InputAction action) noexcept {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(action)) != 0U;
}

class CameraController final {
public:
    explicit CameraController(const Config& config);

    [[nodiscard]] InputAction handleEvent(const SDL_Event& event);
    void update(double deltaSeconds, const TerrainStreamer& terrain);
    void setMouseCaptured(bool captured) noexcept { mouseCaptured_ = captured; }
    void setPosition(const glm::dvec3& position) noexcept;
    void setFlying(bool flying) noexcept { flying_ = flying; verticalVelocity_ = 0.0; }
    void setLookDirection(double yawRadians, double pitchRadians) noexcept;

    [[nodiscard]] CameraState camera(std::uint32_t width, std::uint32_t height) const;
    [[nodiscard]] const glm::dvec3& position() const noexcept { return position_; }
    [[nodiscard]] glm::dvec3 forward() const noexcept;
    [[nodiscard]] bool flying() const noexcept { return flying_; }
    [[nodiscard]] float flySpeed() const noexcept { return flySpeed_; }
    [[nodiscard]] float horizontalSpeed() const noexcept { return horizontalSpeed_; }

private:
    Config config_;
    glm::dvec3 position_{240.0, 250.0, 240.0};
    double yaw_{};
    double pitch_{-0.18};
    double verticalVelocity_{};
    float flySpeed_{};
    float horizontalSpeed_{};
    bool flying_{true};
    bool mouseCaptured_{};
    bool grounded_{};
};

} // namespace terrain
