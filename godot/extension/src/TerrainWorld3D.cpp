#include "TerrainWorld3D.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace terrain::godot_adapter {

void TerrainWorld3D::_bind_methods() {
    using godot::ClassDB;
    using godot::MethodInfo;
    using godot::PropertyHint;
    using godot::PropertyInfo;
    using godot::Variant;

    ClassDB::bind_method(godot::D_METHOD("set_server_url", "value"), &TerrainWorld3D::set_server_url);
    ClassDB::bind_method(godot::D_METHOD("get_server_url"), &TerrainWorld3D::get_server_url);
    ClassDB::bind_method(
        godot::D_METHOD("set_streaming_enabled", "value"), &TerrainWorld3D::set_streaming_enabled);
    ClassDB::bind_method(
        godot::D_METHOD("is_streaming_enabled"), &TerrainWorld3D::is_streaming_enabled);
    ClassDB::bind_method(godot::D_METHOD("get_status"), &TerrainWorld3D::get_status);
    ClassDB::bind_method(
        godot::D_METHOD("get_resident_tile_count"), &TerrainWorld3D::get_resident_tile_count);
    ClassDB::bind_method(
        godot::D_METHOD("get_pending_tile_count"), &TerrainWorld3D::get_pending_tile_count);

    ADD_PROPERTY(
        PropertyInfo(Variant::STRING, "server_url", PropertyHint::PROPERTY_HINT_NONE, ""),
        "set_server_url",
        "get_server_url");
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "streaming_enabled"),
        "set_streaming_enabled",
        "is_streaming_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "status"), "", "get_status");
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "resident_tile_count"), "", "get_resident_tile_count");
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "pending_tile_count"), "", "get_pending_tile_count");

    ADD_SIGNAL(MethodInfo("world_info_ready", PropertyInfo(Variant::STRING, "seed")));
    ADD_SIGNAL(MethodInfo("streaming_state_changed", PropertyInfo(Variant::STRING, "state")));
    ADD_SIGNAL(MethodInfo(
        "terrain_event",
        PropertyInfo(Variant::STRING, "kind"),
        PropertyInfo(Variant::STRING, "tile_key")));
}

void TerrainWorld3D::_ready() {
    godot::UtilityFunctions::print(
        "[TerrainWorld3D] GDExtension ready; server=", server_url_,
        ", streaming=", streaming_enabled_);
    emit_signal("streaming_state_changed", status_);
}

void TerrainWorld3D::set_server_url(const godot::String& value) {
    server_url_ = value.strip_edges();
}

godot::String TerrainWorld3D::get_server_url() const {
    return server_url_;
}

void TerrainWorld3D::set_streaming_enabled(const bool value) {
    streaming_enabled_ = value;
}

bool TerrainWorld3D::is_streaming_enabled() const noexcept {
    return streaming_enabled_;
}

godot::String TerrainWorld3D::get_status() const {
    return status_;
}

std::int64_t TerrainWorld3D::get_resident_tile_count() const noexcept {
    return resident_tile_count_;
}

std::int64_t TerrainWorld3D::get_pending_tile_count() const noexcept {
    return pending_tile_count_;
}

} // namespace terrain::godot_adapter

