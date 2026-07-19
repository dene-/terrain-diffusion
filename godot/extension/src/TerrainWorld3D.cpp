#include "TerrainWorld3D.hpp"

#include "TerrainGodotAdapter.hpp"

#include <terrain/TerrainMesh.hpp>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <utility>

namespace terrain::godot_adapter {
namespace {

[[nodiscard]] std::optional<glm::dvec2> find_walkable_location(
    const RawTile& tile,
    const TerrainMetricConfig& metrics) {
    const auto visible_width = tile.width - tile.borderSamples * 2;
    const auto visible_height = tile.height - tile.borderSamples * 2;
    if (visible_width < 3 || visible_height < 3 || tile.elevations.empty()) return std::nullopt;

    const auto raw_index = [&tile](const std::int32_t x, const std::int32_t z) {
        return static_cast<std::size_t>(
            (z + tile.borderSamples) * tile.width + x + tile.borderSamples);
    };
    const auto elevation_scale = static_cast<double>(metrics.elevationMetersPerSourceUnit)
        * static_cast<double>(metrics.verticalExaggeration);
    const auto derivative_span = tile.spec.spacing
        * static_cast<double>(metrics.metersPerWorldUnit) * 2.0;
    const auto center_x = static_cast<double>(visible_width - 1) * 0.5;
    const auto center_z = static_cast<double>(visible_height - 1) * 0.5;
    double best_score = std::numeric_limits<double>::infinity();
    std::optional<glm::dvec2> best;

    for (std::int32_t z = 1; z < visible_height - 1; ++z) {
        for (std::int32_t x = 1; x < visible_width - 1; ++x) {
            const auto elevation = static_cast<double>(tile.elevations[raw_index(x, z)])
                * elevation_scale;
            if (elevation <= 8.0) continue;

            const auto dx = static_cast<double>(tile.elevations[raw_index(x + 1, z)]
                - tile.elevations[raw_index(x - 1, z)]) * elevation_scale;
            const auto dz = static_cast<double>(tile.elevations[raw_index(x, z + 1)]
                - tile.elevations[raw_index(x, z - 1)]) * elevation_scale;
            const auto normal_y = derivative_span
                / std::sqrt(dx * dx + derivative_span * derivative_span + dz * dz);
            if (normal_y < 0.90) continue;

            const auto score = std::hypot(
                static_cast<double>(x) - center_x,
                static_cast<double>(z) - center_z);
            if (score >= best_score) continue;
            best_score = score;
            best = tile.origin + glm::dvec2{
                static_cast<double>(x) * tile.spec.spacing,
                static_cast<double>(z) * tile.spec.spacing,
            };
        }
    }
    return best;
}

} // namespace

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
    ClassDB::bind_method(
        godot::D_METHOD("set_terrain_material", "value"), &TerrainWorld3D::set_terrain_material);
    ClassDB::bind_method(
        godot::D_METHOD("get_terrain_material"), &TerrainWorld3D::get_terrain_material);
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
    ADD_PROPERTY(
        PropertyInfo(Variant::OBJECT, "terrain_material", PropertyHint::PROPERTY_HINT_RESOURCE_TYPE, "Material"),
        "set_terrain_material",
        "get_terrain_material");
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

TerrainWorld3D::~TerrainWorld3D() {
    if (load_thread_.joinable()) {
        load_thread_.request_stop();
        load_thread_.join();
    }
}

void TerrainWorld3D::_ready() {
    if (godot::Engine::get_singleton()->is_editor_hint()) {
        set_process(false);
        return;
    }

    godot::UtilityFunctions::print(
        "[TerrainWorld3D] GDExtension ready; server=", server_url_,
        ", streaming=", streaming_enabled_);
    emit_signal("streaming_state_changed", status_);
    if (streaming_enabled_) start_initial_load();
}

void TerrainWorld3D::_process(const double delta) {
    if (godot::Engine::get_singleton()->is_editor_hint()) return;

    static_cast<void>(delta);
    std::optional<LoadResult> result;
    {
        std::scoped_lock lock{result_mutex_};
        result.swap(completed_load_);
    }
    if (!result) return;
    install_initial_tile(std::move(*result));
    set_process(false);
}

void TerrainWorld3D::set_server_url(const godot::String& value) {
    server_url_ = value.strip_edges();
}

godot::String TerrainWorld3D::get_server_url() const {
    return server_url_;
}

void TerrainWorld3D::set_streaming_enabled(const bool value) {
    streaming_enabled_ = value;
    if (value && is_inside_tree() && !load_started_
        && !godot::Engine::get_singleton()->is_editor_hint()) {
        start_initial_load();
    }
}

bool TerrainWorld3D::is_streaming_enabled() const noexcept {
    return streaming_enabled_;
}

void TerrainWorld3D::set_terrain_material(const godot::Ref<godot::Material>& value) {
    terrain_material_ = value;
}

godot::Ref<godot::Material> TerrainWorld3D::get_terrain_material() const {
    return terrain_material_;
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

void TerrainWorld3D::start_initial_load() {
    if (load_started_ || godot::Engine::get_singleton()->is_editor_hint()) return;
    load_started_ = true;
    pending_tile_count_ = 1;
    set_status("discovering_world");
    set_process(true);

    const auto encoded_url = server_url_.utf8();
    const std::string server_url{encoded_url.get_data(), static_cast<std::size_t>(encoded_url.length())};
    const auto visuals = visuals_;
    load_thread_ = std::jthread([this, server_url, visuals](const std::stop_token stop_token) {
        LoadResult result;
        try {
            TerrainClient client{server_url};
            result.world = client.fetchWorld();
            if (stop_token.stop_requested()) return;

            constexpr std::int32_t detailed_scale = 8;
            constexpr std::int32_t segments = 128;
            const auto spacing = static_cast<double>(result.world.nativeResolution) / detailed_scale;
            const LodSpec detailed_spec{
                .level = 0,
                .source = TileSource::Detailed,
                .segments = segments,
                .scale = detailed_scale,
                .stride = 1,
                .spacing = spacing,
                .tileWorldSize = spacing * segments,
                .innerRadius = 0.0,
                .outerRadius = 1'200.0,
                .tileRadius = 4,
                .castsShadow = true,
                .vegetation = true,
            };
            const LodSpec locator_spec{
                .level = 6,
                .source = TileSource::Latent,
                .segments = segments,
                .scale = 1,
                .stride = 1,
                .spacing = static_cast<double>(result.world.latentResolution),
                .tileWorldSize = static_cast<double>(result.world.latentResolution) * segments,
                .innerRadius = 0.0,
                .outerRadius = 0.0,
                .tileRadius = 0,
                .castsShadow = false,
                .vegetation = false,
            };
            std::uint64_t seed{};
            try {
                seed = std::stoull(result.world.seed);
            } catch (...) {
                seed = static_cast<std::uint64_t>(std::hash<std::string>{}(result.world.seed));
            }

            TileKey detailed_key{0, 0, 0};
            std::mt19937_64 random{seed};
            std::uniform_int_distribution<std::int64_t> locator_coordinate{-7, 7};
            constexpr std::int32_t maximum_locator_attempts = 9;
            for (std::int32_t attempt = 0; attempt < maximum_locator_attempts; ++attempt) {
                const auto locator_key = attempt == 0
                    ? TileKey{locator_spec.level, 0, 0}
                    : TileKey{locator_spec.level, locator_coordinate(random), locator_coordinate(random)};
                auto locator_tile = client.fetchTile(
                    locator_key, locator_spec, result.world, seed, nullptr);
                ++result.spawnSearchAttempts;
                const auto location = find_walkable_location(locator_tile, visuals.metrics);
                if (!location) continue;
                detailed_key.x = static_cast<std::int64_t>(
                    std::floor(location->x / detailed_spec.tileWorldSize));
                detailed_key.z = static_cast<std::int64_t>(
                    std::floor(location->y / detailed_spec.tileWorldSize));
                break;
            }

            auto raw = client.fetchTile(
                detailed_key, detailed_spec, result.world, seed, &result.timing);
            if (stop_token.stop_requested()) return;
            result.tile = buildTerrainMesh(std::move(raw), seed, visuals);
        } catch (const std::exception& exception) {
            result.error = exception.what();
        }
        if (stop_token.stop_requested()) return;
        std::scoped_lock lock{result_mutex_};
        completed_load_ = std::move(result);
    });
}

void TerrainWorld3D::install_initial_tile(LoadResult result) {
    pending_tile_count_ = 0;
    if (!result.error.empty()) {
        set_status("terrain_error");
        godot::UtilityFunctions::push_error(
            godot::String{"[TerrainWorld3D] Initial terrain load failed: "}
            + godot::String{result.error.c_str()});
        emit_signal("terrain_event", "failed", godot::String{result.error.c_str()});
        return;
    }
    if (!result.tile) {
        set_status("terrain_error");
        godot::UtilityFunctions::push_error("[TerrainWorld3D] Initial terrain load returned no tile");
        return;
    }

    const auto mesh = make_array_mesh(*result.tile);
    if (mesh.is_null()) {
        set_status("terrain_error");
        godot::UtilityFunctions::push_error("[TerrainWorld3D] Could not convert initial terrain mesh");
        return;
    }
    if (terrain_material_.is_valid()) mesh->surface_set_material(0, terrain_material_);

    auto* terrain_tiles = godot::Object::cast_to<godot::Node3D>(
        get_node_or_null(godot::NodePath{"TerrainTiles"}));
    if (terrain_tiles == nullptr) {
        set_status("scene_error");
        godot::UtilityFunctions::push_error("[TerrainWorld3D] TerrainTiles child is missing");
        return;
    }

    auto* tile_node = memnew(godot::MeshInstance3D);
    const auto tile_key = result.tile->raw.key;
    tile_node->set_name(godot::String{"Tile_L0_"}
        + godot::String::num_int64(tile_key.x) + "_" + godot::String::num_int64(tile_key.z));
    tile_node->set_mesh(mesh);
    tile_node->set_cast_shadows_setting(godot::GeometryInstance3D::SHADOW_CASTING_SETTING_ON);
    tile_node->set_position({
        static_cast<godot::real_t>(result.tile->raw.origin.x),
        0.0F,
        static_cast<godot::real_t>(result.tile->raw.origin.y),
    });
    terrain_tiles->add_child(tile_node);
    tile_node->create_trimesh_collision();

    const auto visible_width = result.tile->raw.width - result.tile->raw.borderSamples * 2;
    const auto visible_height = result.tile->raw.height - result.tile->raw.borderSamples * 2;
    const auto center_x = static_cast<double>(visible_width - 1) * 0.5;
    const auto center_z = static_cast<double>(visible_height - 1) * 0.5;
    const auto center_sample_x = visible_width / 2;
    const auto center_sample_z = visible_height / 2;
    std::size_t spawn_index = static_cast<std::size_t>(
        center_sample_z * visible_width + center_sample_x);
    double spawn_score = std::numeric_limits<double>::infinity();
    for (std::int32_t z = 0; z < visible_height; ++z) {
        for (std::int32_t x = 0; x < visible_width; ++x) {
            const auto index = static_cast<std::size_t>(z * visible_width + x);
            const auto& vertex = result.tile->vertices[index];
            if (vertex.position.y <= 4.0F || vertex.normal.y < 0.92F) continue;
            const auto score = std::hypot(static_cast<double>(x) - center_x, static_cast<double>(z) - center_z);
            if (score < spawn_score) {
                spawn_score = score;
                spawn_index = index;
            }
        }
    }
    const auto used_walkable_spawn = spawn_score < std::numeric_limits<double>::infinity();
    const auto& spawn = result.tile->vertices[spawn_index].position;
    const auto spawn_world = godot::Vector3{
        static_cast<godot::real_t>(result.tile->raw.origin.x) + spawn.x,
        spawn.y + 0.05F,
        static_cast<godot::real_t>(result.tile->raw.origin.y) + spawn.z,
    };
    if (auto* player = godot::Object::cast_to<godot::Node3D>(
            get_node_or_null(godot::NodePath{"../../Player"})); player != nullptr) {
        player->set_global_position(spawn_world);
    }

    loaded_tile_ = std::move(result.tile);
    resident_tile_count_ = 1;
    set_status("terrain_ready");
    emit_signal("world_info_ready", godot::String{result.world.seed.c_str()});
    const auto tile_key_text = godot::String::num_int64(tile_key.level) + ":"
        + godot::String::num_int64(tile_key.x) + ":" + godot::String::num_int64(tile_key.z);
    emit_signal("terrain_event", "added", tile_key_text);
    godot::UtilityFunctions::print(
        "[TerrainWorld3D] Initial tile ready; seed=", godot::String{result.world.seed.c_str()},
        ", tile=", tile_key_text,
        ", locator_attempts=", result.spawnSearchAttempts,
        ", spacing=", loaded_tile_->raw.spec.spacing,
        " m, extent=", loaded_tile_->raw.spec.tileWorldSize,
        " m, vertices=", static_cast<std::int64_t>(loaded_tile_->vertices.size()),
        ", spawn=", used_walkable_spawn ? "walkable" : "center-fallback",
        " at ", spawn_world,
        ", request=", result.timing.requestMilliseconds,
        " ms, decode=", result.timing.decodeMilliseconds, " ms");
}

void TerrainWorld3D::set_status(const godot::String& value) {
    if (status_ == value) return;
    status_ = value;
    update_status_label();
    emit_signal("streaming_state_changed", status_);
}

void TerrainWorld3D::update_status_label() const {
    if (auto* label = godot::Object::cast_to<godot::Label>(
            get_node_or_null(godot::NodePath{"../../HUD/Status"})); label != nullptr) {
        label->set_text(status_.replace("_", " ").to_upper());
    }
}

} // namespace terrain::godot_adapter
