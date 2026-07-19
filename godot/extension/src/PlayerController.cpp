#include "PlayerController.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace terrain::godot_adapter {

void PlayerController::_bind_methods() {
    using godot::ClassDB;
    using godot::PropertyInfo;
    using godot::Variant;

    ClassDB::bind_method(godot::D_METHOD("set_walk_speed", "value"), &PlayerController::set_walk_speed);
    ClassDB::bind_method(godot::D_METHOD("get_walk_speed"), &PlayerController::get_walk_speed);
    ClassDB::bind_method(godot::D_METHOD("set_run_speed", "value"), &PlayerController::set_run_speed);
    ClassDB::bind_method(godot::D_METHOD("get_run_speed"), &PlayerController::get_run_speed);
    ClassDB::bind_method(godot::D_METHOD("set_jump_speed", "value"), &PlayerController::set_jump_speed);
    ClassDB::bind_method(godot::D_METHOD("get_jump_speed"), &PlayerController::get_jump_speed);
    ClassDB::bind_method(godot::D_METHOD("set_fly_speed", "value"), &PlayerController::set_fly_speed);
    ClassDB::bind_method(godot::D_METHOD("get_fly_speed"), &PlayerController::get_fly_speed);
    ClassDB::bind_method(godot::D_METHOD("is_flying"), &PlayerController::is_flying);
    ClassDB::bind_method(godot::D_METHOD("get_aim_distance"), &PlayerController::get_aim_distance);

    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "walk_speed", godot::PROPERTY_HINT_RANGE, "0.1,20.0,0.05,suffix:m/s"),
        "set_walk_speed", "get_walk_speed");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "run_speed", godot::PROPERTY_HINT_RANGE, "0.1,40.0,0.05,suffix:m/s"),
        "set_run_speed", "get_run_speed");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "jump_speed", godot::PROPERTY_HINT_RANGE, "0.1,20.0,0.05,suffix:m/s"),
        "set_jump_speed", "get_jump_speed");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fly_speed", godot::PROPERTY_HINT_RANGE, "2.0,20000.0,0.1,or_greater,suffix:m/s"),
        "set_fly_speed", "get_fly_speed");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "flying"), "", "is_flying");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "aim_distance"), "", "get_aim_distance");
}

void PlayerController::_ready() {
    if (godot::Engine::get_singleton()->is_editor_hint()) {
        set_physics_process(false);
        set_process_unhandled_input(false);
        return;
    }

    camera_ = godot::Object::cast_to<godot::Camera3D>(
        get_node_or_null(godot::NodePath{"Camera3D"}));
    aim_label_ = godot::Object::cast_to<godot::Label>(
        get_node_or_null(godot::NodePath{"../HUD/AimDistance"}));
    godot::Input::get_singleton()->set_mouse_mode(godot::Input::MOUSE_MODE_CAPTURED);
}

void PlayerController::_physics_process(const double delta) {
    if (godot::Engine::get_singleton()->is_editor_hint()) return;

    auto* input = godot::Input::get_singleton();
    if (input->is_action_just_pressed("toggle_flight")) {
        flying_ = !flying_;
        set_velocity({});
    }

    const auto input_axis = input->get_vector("move_left", "move_right", "move_forward", "move_backward");
    const auto running = input->is_action_pressed("run");
    auto velocity = get_velocity();

    if (flying_) {
        double vertical{};
        if (input->is_action_pressed("jump")) vertical += 1.0;
        if (input->is_action_pressed("descend")) vertical -= 1.0;
        auto direction = godot::Vector3{input_axis.x, static_cast<godot::real_t>(vertical), input_axis.y};
        if (direction.length_squared() > 1.0) direction = direction.normalized();
        if (camera_ != nullptr) direction = camera_->get_global_basis().xform(direction);
        velocity = direction * static_cast<godot::real_t>(fly_speed_ * (running ? 3.0 : 1.0));
    } else {
        auto direction = godot::Vector3{input_axis.x, 0.0F, input_axis.y};
        if (direction.length_squared() > 1.0) direction = direction.normalized();
        direction = get_basis().xform(direction);
        const auto speed = static_cast<godot::real_t>(running ? run_speed_ : walk_speed_);
        velocity.x = direction.x * speed;
        velocity.z = direction.z * speed;
        if (!is_on_floor()) velocity.y -= static_cast<godot::real_t>(9.81 * std::min(delta, 0.05));
        if (is_on_floor() && input->is_action_just_pressed("jump")) {
            velocity.y = static_cast<godot::real_t>(jump_speed_);
        }
    }

    set_velocity(velocity);
    move_and_slide();
    update_aim_distance();
}

void PlayerController::_unhandled_input(const godot::Ref<godot::InputEvent>& event) {
    if (godot::Engine::get_singleton()->is_editor_hint()) return;

    auto* input = godot::Input::get_singleton();
    const godot::Ref<godot::InputEventMouseMotion> motion = event;
    if (motion.is_valid() && input->get_mouse_mode() == godot::Input::MOUSE_MODE_CAPTURED) {
        const auto relative = motion->get_relative();
        rotate_y(static_cast<godot::real_t>(-relative.x * mouse_sensitivity_));
        camera_pitch_ = std::clamp(
            camera_pitch_ - static_cast<double>(relative.y) * mouse_sensitivity_,
            -std::numbers::pi_v<double> * 0.495,
            std::numbers::pi_v<double> * 0.495);
        if (camera_ != nullptr) camera_->set_rotation({static_cast<godot::real_t>(camera_pitch_), 0.0F, 0.0F});
        return;
    }

    const godot::Ref<godot::InputEventMouseButton> mouse_button = event;
    if (mouse_button.is_valid() && mouse_button->is_pressed()) {
        if (mouse_button->get_button_index() == godot::MOUSE_BUTTON_WHEEL_UP) {
            set_fly_speed(fly_speed_ * 1.22);
        } else if (mouse_button->get_button_index() == godot::MOUSE_BUTTON_WHEEL_DOWN) {
            set_fly_speed(fly_speed_ / 1.22);
        } else if (mouse_button->get_button_index() == godot::MOUSE_BUTTON_LEFT
            && input->get_mouse_mode() != godot::Input::MOUSE_MODE_CAPTURED) {
            input->set_mouse_mode(godot::Input::MOUSE_MODE_CAPTURED);
        }
        return;
    }

    const godot::Ref<godot::InputEventKey> key = event;
    if (key.is_valid() && key->is_pressed() && !key->is_echo()
        && key->get_keycode() == godot::KEY_ESCAPE) {
        input->set_mouse_mode(godot::Input::MOUSE_MODE_VISIBLE);
    }
}

void PlayerController::update_aim_distance() {
    if (camera_ == nullptr || get_world_3d().is_null()) {
        aim_distance_ = 0.0;
        return;
    }
    const auto from = camera_->get_global_position();
    const auto direction = -camera_->get_global_basis().get_column(2);
    const auto query = godot::PhysicsRayQueryParameters3D::create(from, from + direction * 1'000'000.0F);
    const auto result = get_world_3d()->get_direct_space_state()->intersect_ray(query);
    if (result.has("position")) {
        const godot::Vector3 position = result["position"];
        aim_distance_ = static_cast<double>(from.distance_to(position));
    } else {
        aim_distance_ = 0.0;
    }
    if (aim_label_ != nullptr && std::abs(displayed_aim_distance_ - aim_distance_) >= 0.1) {
        displayed_aim_distance_ = aim_distance_;
        if (aim_distance_ <= 0.0) {
            aim_label_->set_text("");
        } else if (aim_distance_ >= 1'000.0) {
            aim_label_->set_text(godot::String::num(aim_distance_ / 1'000.0, 2) + " km");
        } else {
            aim_label_->set_text(godot::String::num(aim_distance_, 1) + " m");
        }
    }
}

void PlayerController::set_walk_speed(const double value) noexcept { walk_speed_ = std::max(value, 0.1); }
double PlayerController::get_walk_speed() const noexcept { return walk_speed_; }
void PlayerController::set_run_speed(const double value) noexcept { run_speed_ = std::max(value, 0.1); }
double PlayerController::get_run_speed() const noexcept { return run_speed_; }
void PlayerController::set_jump_speed(const double value) noexcept { jump_speed_ = std::max(value, 0.1); }
double PlayerController::get_jump_speed() const noexcept { return jump_speed_; }
void PlayerController::set_fly_speed(const double value) noexcept { fly_speed_ = std::clamp(value, 2.0, 20'000.0); }
double PlayerController::get_fly_speed() const noexcept { return fly_speed_; }
bool PlayerController::is_flying() const noexcept { return flying_; }
double PlayerController::get_aim_distance() const noexcept { return aim_distance_; }

} // namespace terrain::godot_adapter
