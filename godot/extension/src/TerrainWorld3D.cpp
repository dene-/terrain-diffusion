#include "TerrainWorld3D.hpp"

#include "TerrainGodotAdapter.hpp"

#include <terrain/TerrainMesh.hpp>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <ranges>
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

[[nodiscard]] godot::String tile_key_text(const TileKey& key) {
    return godot::String::num_int64(key.level) + ":"
        + godot::String::num_int64(key.x) + ":" + godot::String::num_int64(key.z);
}

[[nodiscard]] godot::String tile_node_name(const TileKey& key) {
    return godot::String{"Tile_L"} + godot::String::num_int64(key.level) + "_"
        + godot::String::num_int64(key.x) + "_" + godot::String::num_int64(key.z);
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
    if (result) install_initial_tile(std::move(*result));
    if (streamer_) update_streaming();
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
            result.streamer = std::make_unique<TerrainStreamer>(
                std::move(client), result.world, visuals);
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

    const auto tile_key = result.tile->raw.key;
    floating_origin_ = result.tile->raw.origin;
    if (add_tile(result.tile) == nullptr) {
        set_status("terrain_error");
        godot::UtilityFunctions::push_error("[TerrainWorld3D] Could not install initial terrain mesh");
        return;
    }

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
        static_cast<godot::real_t>(result.tile->raw.origin.x - floating_origin_.x) + spawn.x,
        spawn.y + 0.05F,
        static_cast<godot::real_t>(result.tile->raw.origin.y - floating_origin_.y) + spawn.z,
    };
    if (auto* player = godot::Object::cast_to<godot::Node3D>(
            get_node_or_null(godot::NodePath{"../../Player"})); player != nullptr) {
        player->set_global_position(spawn_world);
    }

    loaded_tile_ = std::move(result.tile);
    streamer_ = std::move(result.streamer);
    resident_tile_count_ = static_cast<std::int64_t>(resident_tiles_.size());
    set_status("terrain_ready");
    emit_signal("world_info_ready", godot::String{result.world.seed.c_str()});
    const auto key_text = tile_key_text(tile_key);
    emit_signal("terrain_event", "added", key_text);
    godot::UtilityFunctions::print(
        "[TerrainWorld3D] Initial tile ready; seed=", godot::String{result.world.seed.c_str()},
        ", tile=", key_text,
        ", locator_attempts=", result.spawnSearchAttempts,
        ", spacing=", loaded_tile_->raw.spec.spacing,
        " m, extent=", loaded_tile_->raw.spec.tileWorldSize,
        " m, vertices=", static_cast<std::int64_t>(loaded_tile_->vertices.size()),
        ", spawn=", used_walkable_spawn ? "walkable" : "center-fallback",
        " at ", spawn_world,
        ", request=", result.timing.requestMilliseconds,
        " ms, decode=", result.timing.decodeMilliseconds, " ms");

    if (!streamer_) {
        set_status("terrain_error");
        godot::UtilityFunctions::push_error("[TerrainWorld3D] Terrain streamer was not initialized");
        return;
    }
    lod_readiness_dirty_ = true;
}

godot::MeshInstance3D* TerrainWorld3D::add_tile(const TileMeshPtr& tile) {
    if (!tile) return nullptr;
    const auto mesh = make_array_mesh(*tile);
    if (mesh.is_null()) return nullptr;
    if (terrain_material_.is_valid()) mesh->surface_set_material(0, terrain_material_);

    auto* terrain_tiles = godot::Object::cast_to<godot::Node3D>(
        get_node_or_null(godot::NodePath{"TerrainTiles"}));
    if (terrain_tiles == nullptr) return nullptr;

    const auto key = tile->raw.key;
    if (const auto existing = resident_tiles_.find(key); existing != resident_tiles_.end()) {
        if (existing->second.node != nullptr) {
            if (auto* parent = existing->second.node->get_parent(); parent != nullptr) {
                parent->remove_child(existing->second.node);
            }
            existing->second.node->queue_free();
        }
        resident_tiles_.erase(existing);
    }

    auto* tile_node = memnew(godot::MeshInstance3D);
    tile_node->set_name(tile_node_name(key));
    tile_node->set_mesh(mesh);
    tile_node->set_cast_shadows_setting(tile->raw.spec.castsShadow
        ? godot::GeometryInstance3D::SHADOW_CASTING_SETTING_ON
        : godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    tile_node->set_position({
        static_cast<godot::real_t>(tile->raw.origin.x - floating_origin_.x),
        0.0F,
        static_cast<godot::real_t>(tile->raw.origin.y - floating_origin_.y),
    });
    tile_node->set_instance_shader_parameter("lod_inner_radius", 0.0F);
    tile_node->set_instance_shader_parameter(
        "lod_outer_radius", static_cast<float>(tile->raw.spec.outerRadius));
    tile_node->set_instance_shader_parameter(
        "floating_origin_xz",
        godot::Vector2{
            static_cast<godot::real_t>(floating_origin_.x),
            static_cast<godot::real_t>(floating_origin_.y),
        });
    terrain_tiles->add_child(tile_node);
    if (key.level == 0) tile_node->create_trimesh_collision();

    resident_tiles_.insert_or_assign(key, ResidentTile{
        .node = tile_node,
        .spec = tile->raw.spec,
        .absoluteOrigin = tile->raw.origin,
    });
    lod_readiness_dirty_ = true;
    return tile_node;
}

void TerrainWorld3D::remove_tile(const TileKey& key) {
    const auto iterator = resident_tiles_.find(key);
    if (iterator == resident_tiles_.end()) return;
    if (iterator->second.node != nullptr) {
        if (auto* parent = iterator->second.node->get_parent(); parent != nullptr) {
            parent->remove_child(iterator->second.node);
        }
        iterator->second.node->queue_free();
    }
    resident_tiles_.erase(iterator);
    lod_readiness_dirty_ = true;
}

void TerrainWorld3D::update_streaming() {
    auto* player = godot::Object::cast_to<godot::Node3D>(
        get_node_or_null(godot::NodePath{"../../Player"}));
    if (player == nullptr) return;
    rebase_if_needed(*player);

    const auto local_position = player->get_global_position();
    const auto absolute_position = glm::dvec3{
        floating_origin_.x + static_cast<double>(local_position.x),
        static_cast<double>(local_position.y),
        floating_origin_.y + static_cast<double>(local_position.z),
    };
    glm::dvec3 view_direction{0.0, 0.0, -1.0};
    if (auto* camera = godot::Object::cast_to<godot::Camera3D>(
            get_node_or_null(godot::NodePath{"../../Player/Camera3D"})); camera != nullptr) {
        const auto direction = -camera->get_global_basis().get_column(2);
        view_direction = {direction.x, direction.y, direction.z};
    }

    auto stream_position = absolute_position;
    if (!streamer_->sampleHeight(absolute_position.x, absolute_position.z)) {
        stream_position.y = 1.68;
    }
    streamer_->update(stream_position, view_direction);

    bool tiles_changed{};
    for (auto& event : streamer_->drainEvents(
            visuals_.streaming.maxGpuUploadsPerFrame,
            visuals_.streaming.maxGpuUploadBytesPerFrame)) {
        const auto key_text = tile_key_text(event.key);
        if (event.type == TerrainEvent::Type::Added && event.tile) {
            if (add_tile(event.tile) == nullptr) {
                godot::UtilityFunctions::push_error(
                    godot::String{"[TerrainWorld3D] Could not upload streamed tile "} + key_text);
                continue;
            }
            streamer_->releaseUploadedPayload(event.key);
            if (loaded_tile_ && loaded_tile_->raw.key == event.key) loaded_tile_.reset();
            tiles_changed = true;
            godot::UtilityFunctions::print(
                "[TerrainWorld3D] Tile added; key=", key_text,
                ", spacing=", event.tile->raw.spec.spacing,
                " m, resident=", static_cast<std::int64_t>(resident_tiles_.size()));
            emit_signal("terrain_event", "added", key_text);
        } else if (event.type == TerrainEvent::Type::Removed) {
            remove_tile(event.key);
            tiles_changed = true;
            godot::UtilityFunctions::print(
                "[TerrainWorld3D] Tile removed; key=", key_text,
                ", resident=", static_cast<std::int64_t>(resident_tiles_.size()));
            emit_signal("terrain_event", "removed", key_text);
        } else if (event.type == TerrainEvent::Type::Failed) {
            godot::UtilityFunctions::push_warning(
                godot::String{"[TerrainWorld3D] Terrain stream failed for "}
                + key_text + ": " + godot::String{event.message.c_str()});
            emit_signal("terrain_event", "failed", key_text);
        }
    }

    pending_tile_count_ = static_cast<std::int64_t>(streamer_->pendingTileCount());
    resident_tile_count_ = static_cast<std::int64_t>(resident_tiles_.size());
    const auto lod_distance = glm::length(glm::dvec2{absolute_position.x, absolute_position.z}
        - last_lod_update_position_);
    if (tiles_changed || lod_readiness_dirty_ || !std::isfinite(lod_distance) || lod_distance >= 32.0) {
        update_lod_readiness(absolute_position);
    }
}

void TerrainWorld3D::update_lod_readiness(const glm::dvec3& camera_position) {
    const auto point_covered_at_level = [this](
        const std::int32_t level, const double world_x, const double world_z) {
        return std::ranges::any_of(resident_tiles_, [=](const auto& entry) {
            const auto& tile = entry.second;
            return tile.spec.level == level
                && world_x >= tile.absoluteOrigin.x && world_z >= tile.absoluteOrigin.y
                && world_x <= tile.absoluteOrigin.x + tile.spec.tileWorldSize
                && world_z <= tile.absoluteOrigin.y + tile.spec.tileWorldSize;
        });
    };

    std::unordered_map<std::int32_t, float> ready_inner_radius;
    for (const auto& [key, tile] : resident_tiles_) {
        if (key.level <= 0 || ready_inner_radius.contains(key.level)) continue;
        if (!point_covered_at_level(key.level - 1, camera_position.x, camera_position.z)) {
            ready_inner_radius.emplace(key.level, 0.0F);
            continue;
        }
        const auto child = std::ranges::find_if(resident_tiles_, [level = key.level - 1](const auto& entry) {
            return entry.first.level == level;
        });
        if (child == resident_tiles_.end()) {
            ready_inner_radius.emplace(key.level, 0.0F);
            continue;
        }

        const auto step = std::max(32.0, child->second.spec.tileWorldSize * 0.20);
        auto covered_radius = tile.spec.innerRadius;
        constexpr std::int32_t direction_count = 16;
        for (std::int32_t direction_index = 0; direction_index < direction_count; ++direction_index) {
            const auto angle = static_cast<double>(direction_index) * std::numbers::pi_v<double> * 2.0
                / static_cast<double>(direction_count);
            const auto direction = glm::dvec2{std::cos(angle), std::sin(angle)};
            double direction_radius{};
            for (double radius = step; radius <= tile.spec.innerRadius + step; radius += step) {
                const auto point = glm::dvec2{camera_position.x, camera_position.z}
                    + direction * radius;
                if (!point_covered_at_level(key.level - 1, point.x, point.y)) break;
                direction_radius = radius;
            }
            covered_radius = std::min(covered_radius, direction_radius);
        }
        ready_inner_radius.emplace(key.level, static_cast<float>(std::clamp(
            covered_radius * 0.86, 0.0, tile.spec.innerRadius)));
    }

    for (const auto& [key, tile] : resident_tiles_) {
        if (tile.node == nullptr) continue;
        const auto inner = ready_inner_radius.contains(key.level)
            ? ready_inner_radius.at(key.level) : 0.0F;
        tile.node->set_instance_shader_parameter("lod_inner_radius", inner);
    }
    last_lod_update_position_ = {camera_position.x, camera_position.z};
    lod_readiness_dirty_ = false;
}

void TerrainWorld3D::rebase_if_needed(godot::Node3D& player) {
    constexpr double rebase_threshold = 4'096.0;
    constexpr double rebase_quantum = 480.0;
    const auto local_position = player.get_global_position();
    if (std::abs(static_cast<double>(local_position.x)) < rebase_threshold
        && std::abs(static_cast<double>(local_position.z)) < rebase_threshold) return;

    const auto delta = glm::dvec2{
        std::round(static_cast<double>(local_position.x) / rebase_quantum) * rebase_quantum,
        std::round(static_cast<double>(local_position.z) / rebase_quantum) * rebase_quantum,
    };
    floating_origin_ += delta;
    player.set_global_position({
        local_position.x - static_cast<godot::real_t>(delta.x),
        local_position.y,
        local_position.z - static_cast<godot::real_t>(delta.y),
    });

    const auto floating_origin = godot::Vector2{
        static_cast<godot::real_t>(floating_origin_.x),
        static_cast<godot::real_t>(floating_origin_.y),
    };
    for (auto& [unused, tile] : resident_tiles_) {
        static_cast<void>(unused);
        if (tile.node == nullptr) continue;
        tile.node->set_position({
            static_cast<godot::real_t>(tile.absoluteOrigin.x - floating_origin_.x),
            0.0F,
            static_cast<godot::real_t>(tile.absoluteOrigin.y - floating_origin_.y),
        });
        tile.node->set_instance_shader_parameter("floating_origin_xz", floating_origin);
    }
    godot::UtilityFunctions::print(
        "[TerrainWorld3D] Floating origin rebased to (", floating_origin.x, ", ", floating_origin.y,
        "); resident=", static_cast<std::int64_t>(resident_tiles_.size()));
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
