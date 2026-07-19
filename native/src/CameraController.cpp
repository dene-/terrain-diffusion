#include "CameraController.hpp"

#include <SDL3/SDL_keyboard.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace terrain {

CameraController::CameraController(const Config& config)
    : config_(config), flySpeed_(config.initialFlySpeed) {}

InputAction CameraController::handleEvent(const SDL_Event& event) {
    InputAction action = InputAction::None;
    if (event.type == SDL_EVENT_QUIT) action = action | InputAction::Quit;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        if (event.key.scancode == SDL_SCANCODE_ESCAPE) action = action | InputAction::ToggleCapture;
        if (event.key.scancode == SDL_SCANCODE_R) action = action | InputAction::Regenerate;
        if (event.key.scancode == SDL_SCANCODE_G) action = action | InputAction::DebugSnapshot;
        if (event.key.scancode == SDL_SCANCODE_V) action = action | InputAction::CycleDebugView;
        if (event.key.scancode == SDL_SCANCODE_F) {
            flying_ = !flying_;
            verticalVelocity_ = 0.0;
        }
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && mouseCaptured_) {
        constexpr double sensitivity = 0.00175;
        yaw_ += static_cast<double>(event.motion.xrel) * sensitivity;
        pitch_ -= static_cast<double>(event.motion.yrel) * sensitivity;
        pitch_ = std::clamp(pitch_, -std::numbers::pi_v<double> * 0.495, std::numbers::pi_v<double> * 0.495);
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        flySpeed_ *= std::pow(1.22F, event.wheel.y);
        flySpeed_ = std::clamp(flySpeed_, 2.0F, 20'000.0F);
    }
    return action;
}

void CameraController::update(double deltaSeconds, const TerrainStreamer& terrain) {
    const auto dt = std::clamp(deltaSeconds, 0.0, 0.05);
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const auto fullForward = forward();
    auto flatForward = glm::dvec3{fullForward.x, 0.0, fullForward.z};
    if (glm::dot(flatForward, flatForward) > 1.0e-8) flatForward = glm::normalize(flatForward);
    const auto right = glm::normalize(glm::cross(glm::dvec3{0.0, 1.0, 0.0}, flatForward));
    glm::dvec3 movement{};
    if (keys[SDL_SCANCODE_W]) movement += flying_ ? fullForward : flatForward;
    if (keys[SDL_SCANCODE_S]) movement -= flying_ ? fullForward : flatForward;
    if (keys[SDL_SCANCODE_D]) movement += right;
    if (keys[SDL_SCANCODE_A]) movement -= right;
    if (flying_ && keys[SDL_SCANCODE_SPACE]) movement.y += 1.0;
    if (flying_ && (keys[SDL_SCANCODE_C] || keys[SDL_SCANCODE_LCTRL])) movement.y -= 1.0;
    if (glm::dot(movement, movement) > 1.0) movement = glm::normalize(movement);

    const auto running = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    const auto speed = flying_ ? flySpeed_ * (running ? 3.0F : 1.0F) : (running ? config_.runSpeed : config_.walkSpeed);
    const auto oldPosition = position_;
    position_ += movement * static_cast<double>(speed) * dt;

    if (!flying_) {
        auto ground = terrain.sampleHeight(position_.x, position_.z);
        if (!ground) {
            // Do not let the player walk or fall into a tile that is still
            // streaming. Keep the last valid horizontal position until its
            // detailed collision height is installed.
            position_.x = oldPosition.x;
            position_.z = oldPosition.z;
            ground = terrain.sampleHeight(position_.x, position_.z);
        }
        if (!ground) {
            position_.y = oldPosition.y;
            verticalVelocity_ = 0.0;
            grounded_ = false;
        }
        const auto targetEye = ground ? static_cast<double>(*ground) + config_.eyeHeight : position_.y;
        if (grounded_ && keys[SDL_SCANCODE_SPACE]) {
            verticalVelocity_ = config_.jumpSpeed;
            grounded_ = false;
        }
        if (ground) {
            verticalVelocity_ -= 9.81 * dt;
            position_.y += verticalVelocity_ * dt;
            if (position_.y <= targetEye) {
                position_.y = targetEye;
                verticalVelocity_ = 0.0;
                grounded_ = true;
            }
        }
    }
    horizontalSpeed_ = static_cast<float>(std::hypot(position_.x - oldPosition.x, position_.z - oldPosition.z) / std::max(dt, 1.0e-5));
}

void CameraController::setPosition(const glm::dvec3& position) noexcept {
    position_ = position;
    verticalVelocity_ = 0.0;
}

void CameraController::setLookDirection(double yawRadians, double pitchRadians) noexcept {
    yaw_ = yawRadians;
    pitch_ = std::clamp(pitchRadians,
        -std::numbers::pi_v<double> * 0.495,
        std::numbers::pi_v<double> * 0.495);
}

CameraState CameraController::camera(std::uint32_t width, std::uint32_t height) const {
    CameraState state;
    state.absolutePosition = position_;
    constexpr double floatingGrid = 4'096.0;
    state.floatingOrigin = glm::dvec2{
        std::floor(position_.x / floatingGrid) * floatingGrid,
        std::floor(position_.z / floatingGrid) * floatingGrid,
    };
    state.localPosition = glm::vec3{
        static_cast<float>(position_.x - state.floatingOrigin.x),
        static_cast<float>(position_.y),
        static_cast<float>(position_.z - state.floatingOrigin.y),
    };
    state.forward = glm::vec3{forward()};
    state.right = glm::normalize(glm::cross(glm::vec3{0.0F, 1.0F, 0.0F}, state.forward));
    state.aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1U));
    state.verticalFovRadians = glm::radians(config_.verticalFovDegrees);
    // Reverse the depth range. With a human-scale near plane and an 800 km
    // horizon, conventional Z loses precision across overlapping distant LODs.
    state.projection = glm::perspectiveLH_ZO(
        state.verticalFovRadians, state.aspect, state.farPlane, state.nearPlane);
    state.view = glm::lookAtLH(state.localPosition, state.localPosition + state.forward, glm::vec3{0.0F, 1.0F, 0.0F});
    state.viewProjection = state.projection * state.view;
    return state;
}

glm::dvec3 CameraController::forward() const noexcept {
    const auto cosPitch = std::cos(pitch_);
    return glm::normalize(glm::dvec3{
        std::sin(yaw_) * cosPitch,
        std::sin(pitch_),
        std::cos(yaw_) * cosPitch,
    });
}

} // namespace terrain
