#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/character_body3d.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>

namespace terrain::godot_adapter {

class PlayerController final : public godot::CharacterBody3D {
    GDCLASS(PlayerController, godot::CharacterBody3D)

public:
    void _ready() override;
    void _physics_process(double delta) override;
    void _unhandled_input(const godot::Ref<godot::InputEvent>& event) override;

    void set_walk_speed(double value) noexcept;
    [[nodiscard]] double get_walk_speed() const noexcept;
    void set_run_speed(double value) noexcept;
    [[nodiscard]] double get_run_speed() const noexcept;
    void set_jump_speed(double value) noexcept;
    [[nodiscard]] double get_jump_speed() const noexcept;
    void set_fly_speed(double value) noexcept;
    [[nodiscard]] double get_fly_speed() const noexcept;
    void set_flying(bool value) noexcept;
    [[nodiscard]] bool is_flying() const noexcept;
    void set_look_direction(double yaw_radians, double pitch_radians) noexcept;
    void set_terrain_aim_distance(double value);
    [[nodiscard]] double get_aim_distance() const noexcept;

protected:
    static void _bind_methods();

private:
    void update_aim_distance();
    void update_aim_label();

    static constexpr double MaximumEyeAltitudeMeters = 15'000.0;
    static constexpr double EyeHeightMeters = 1.68;

    godot::Camera3D* camera_{};
    godot::Label* aim_label_{};
    double walk_speed_{1.45};
    double run_speed_{5.5};
    double jump_speed_{5.2};
    double fly_speed_{24.0};
    double mouse_sensitivity_{0.00175};
    double camera_pitch_{};
    double aim_distance_{};
    double terrain_aim_distance_{};
    double displayed_aim_distance_{-1.0};
    double aim_update_accumulator_{};
    godot::Vector3 last_aim_origin_{};
    godot::Vector3 last_aim_direction_{};
    bool aim_state_initialized_{};
    bool flying_{};
    bool static_capture_{};
};

} // namespace terrain::godot_adapter
