#include "TerrainWorld3D.hpp"

#include "TerrainGodotAdapter.hpp"
#include "PlayerController.hpp"

#include <terrain/TerrainMesh.hpp>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/immediate_mesh.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/occluder_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/physical_sky_material.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/progress_bar.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <unordered_set>
#include <utility>

namespace terrain::godot_adapter {
namespace {

constexpr double FlatWorldViewDistanceMetres = 240'000.0;

[[nodiscard]] float terrain_cull_margin(
    const TileKey& key,
    const LodSpec& spec,
    const LodPresentationRange& presentation_range) {
    static_cast<void>(key);
    static_cast<void>(spec);
    static_cast<void>(presentation_range);
    return 8.0F;
}

[[nodiscard]] std::size_t estimate_render_resource_bytes(const TileMesh& tile) {
    // Conservative retained-resource estimate. The visible grid is shared;
    // per-tile terrain data is one RF height texture and two RGBA8 field
    // textures, plus coarse occluder/collision data and vegetation buffers.
    constexpr std::size_t multimesh_instance_bytes = 80U;
    constexpr std::size_t object_allowance_bytes = 4U * 1024U;
    const auto height_sample_count = static_cast<std::size_t>(tile.raw.width)
        * static_cast<std::size_t>(tile.raw.height);
    const auto visible_width = tile.raw.width - tile.raw.borderSamples * 2;
    const auto visible_height = tile.raw.height - tile.raw.borderSamples * 2;
    const auto field_sample_count = static_cast<std::size_t>(visible_width)
        * static_cast<std::size_t>(visible_height);
    auto bytes = height_sample_count * sizeof(float)
        + field_sample_count * 8U
        + (tile.trees.size() + tile.grass.size()) * multimesh_instance_bytes;
    if (tile.raw.key.level == 0 && visible_width > 1 && visible_height > 1) {
        const auto sampled_grid_bytes = [visible_width, visible_height](
            const std::int32_t stride,
            const std::size_t vertex_bytes) {
            const auto width = static_cast<std::size_t>(
                (visible_width - 1 + stride - 1) / stride + 1);
            const auto height = static_cast<std::size_t>(
                (visible_height - 1 + stride - 1) / stride + 1);
            return width * height * vertex_bytes
                + (width - 1U) * (height - 1U) * 6U * sizeof(std::uint32_t);
        };
        bytes += sampled_grid_bytes(8, 12U); // occluder
        bytes += sampled_grid_bytes(4, 36U); // concave collision faces
    }
    bytes += (tile.treeRanges.size() + tile.grassRanges.size() + 3U)
        * object_allowance_bytes;
    return bytes;
}

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
            const auto source_at = [&tile, &raw_index](const std::int32_t sx, const std::int32_t sz) {
                const auto index = raw_index(sx, sz);
                return tile.renderElevations.size() == tile.elevations.size()
                    ? static_cast<double>(tile.renderElevations[index])
                    : static_cast<double>(tile.elevations[index]);
            };
            const auto elevation = source_at(x, z) * elevation_scale;
            if (elevation <= 8.0) continue;

            const auto dx = (source_at(x + 1, z) - source_at(x - 1, z)) * elevation_scale;
            const auto dz = (source_at(x, z + 1) - source_at(x, z - 1)) * elevation_scale;
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

[[nodiscard]] godot::String seed_text(const std::uint64_t seed) {
    const auto text = std::to_string(seed);
    return godot::String{text.c_str()};
}

constexpr std::array<const char*, 14> debug_view_names{
    "SHADED",
    "ELEVATION",
    "NORMALS",
    "SLOPE",
    "CURVATURE",
    "WETNESS",
    "FOREST",
    "ROCK",
    "SCREE",
    "SNOW",
    "TERRAIN LOD",
    "FOG AMOUNT",
    "SHADOW RANGE",
    "VEGETATION LOD",
};

} // namespace

void TerrainWorld3D::_bind_methods() {
    using godot::ClassDB;
    using godot::MethodInfo;
    using godot::PropertyHint;
    using godot::PropertyInfo;
    using godot::Variant;

    ClassDB::bind_method(godot::D_METHOD("set_server_url", "value"), &TerrainWorld3D::set_server_url);
    ClassDB::bind_method(godot::D_METHOD("get_server_url"), &TerrainWorld3D::get_server_url);
    ClassDB::bind_method(godot::D_METHOD("set_map_path", "value"), &TerrainWorld3D::set_map_path);
    ClassDB::bind_method(godot::D_METHOD("get_map_path"), &TerrainWorld3D::get_map_path);
    ClassDB::bind_method(
        godot::D_METHOD("set_streaming_enabled", "value"), &TerrainWorld3D::set_streaming_enabled);
    ClassDB::bind_method(
        godot::D_METHOD("is_streaming_enabled"), &TerrainWorld3D::is_streaming_enabled);
    ClassDB::bind_method(
        godot::D_METHOD("set_editor_streaming_mode", "value"),
        &TerrainWorld3D::set_editor_streaming_mode);
    ClassDB::bind_method(
        godot::D_METHOD("is_editor_streaming_mode"),
        &TerrainWorld3D::is_editor_streaming_mode);
    ClassDB::bind_method(
        godot::D_METHOD(
            "set_editor_observer",
            "position",
            "view_direction",
            "vertical_fov_degrees",
            "viewport_height"),
        &TerrainWorld3D::set_editor_observer);
    ClassDB::bind_method(
        godot::D_METHOD("set_terrain_material", "value"), &TerrainWorld3D::set_terrain_material);
    ClassDB::bind_method(
        godot::D_METHOD("get_terrain_material"), &TerrainWorld3D::get_terrain_material);
    ClassDB::bind_method(
        godot::D_METHOD("set_visual_settings", "value"), &TerrainWorld3D::set_visual_settings);
    ClassDB::bind_method(
        godot::D_METHOD("get_visual_settings"), &TerrainWorld3D::get_visual_settings);
    ClassDB::bind_method(
        godot::D_METHOD("apply_visual_settings", "rebuild_derived"),
        &TerrainWorld3D::apply_visual_settings,
        DEFVAL(false));
    ClassDB::bind_method(
        godot::D_METHOD("set_debug_view", "value"), &TerrainWorld3D::set_debug_view);
    ClassDB::bind_method(godot::D_METHOD("get_debug_view"), &TerrainWorld3D::get_debug_view);
    ClassDB::bind_method(
        godot::D_METHOD("apply_camera_bookmark", "bookmark"),
        &TerrainWorld3D::apply_camera_bookmark);
    ClassDB::bind_method(
        godot::D_METHOD("request_random_world"),
        &TerrainWorld3D::request_random_world);
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
        PropertyInfo(Variant::STRING, "map_path", PropertyHint::PROPERTY_HINT_FILE, "*.tmap"),
        "set_map_path",
        "get_map_path");
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "streaming_enabled"),
        "set_streaming_enabled",
        "is_streaming_enabled");
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "editor_streaming_mode"),
        "set_editor_streaming_mode",
        "is_editor_streaming_mode");
    ADD_PROPERTY(
        PropertyInfo(Variant::OBJECT, "terrain_material", PropertyHint::PROPERTY_HINT_RESOURCE_TYPE, "Material"),
        "set_terrain_material",
        "get_terrain_material");
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "visual_settings",
            PropertyHint::PROPERTY_HINT_RESOURCE_TYPE,
            "WorldVisualSettings"),
        "set_visual_settings",
        "get_visual_settings");
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "debug_view",
            PropertyHint::PROPERTY_HINT_ENUM,
            "Shaded,Elevation,Normals,Slope,Curvature,Wetness,Forest,Rock,Scree,Snow,Terrain LOD,Fog Amount,Shadow Range,Vegetation LOD"),
        "set_debug_view",
        "get_debug_view");
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
    if (auto* rendering_server = godot::RenderingServer::get_singleton();
        rendering_server != nullptr) {
        rendering_server->global_shader_parameter_remove("terrain_camera_absolute_xz");
    }
}

void TerrainWorld3D::_ready() {
    const auto editor_hint = godot::Engine::get_singleton()->is_editor_hint();
    if (editor_hint && !editor_streaming_mode_) {
        set_process(false);
        set_process_unhandled_input(false);
        return;
    }

    auto* rendering_server = godot::RenderingServer::get_singleton();
    if (rendering_server != nullptr) {
        const godot::StringName camera_parameter{"terrain_camera_absolute_xz"};
        rendering_server->global_shader_parameter_add(
            camera_parameter,
            godot::RenderingServer::GLOBAL_VAR_TYPE_VEC2,
            godot::Vector2{});
    }
    heightmap_terrain_shader_ = godot::ResourceLoader::get_singleton()->load(
        "res://materials/terrain_heightmap.gdshader", "Shader");
    if (heightmap_terrain_shader_.is_null()) {
        godot::UtilityFunctions::push_error(
            "[TerrainWorld3D] GPU heightmap terrain shader is unavailable");
    }

    // These nodes are permanent members of the main scene. Resolve their
    // NodePaths once instead of traversing the scene tree several times per
    // frame and once for every incremental vegetation work unit.
    if (!editor_hint) {
        player_node_ = godot::Object::cast_to<godot::Node3D>(
            get_node_or_null(godot::NodePath{"../../Player"}));
        camera_ = godot::Object::cast_to<godot::Camera3D>(
            get_node_or_null(godot::NodePath{"../../Player/Camera3D"}));
    }
    terrain_tiles_root_ = godot::Object::cast_to<godot::Node3D>(
        get_node_or_null(godot::NodePath{"TerrainTiles"}));
    vegetation_root_ = godot::Object::cast_to<godot::Node3D>(
        get_node_or_null(godot::NodePath{"Vegetation"}));
    ocean_ = godot::Object::cast_to<godot::MeshInstance3D>(
        get_node_or_null(godot::NodePath{"Ocean"}));
    authored_world_root_ = godot::Object::cast_to<godot::Node3D>(
        get_node_or_null(editor_hint
            ? godot::NodePath{"../../AuthoredWorld"}
            : godot::NodePath{"../AuthoredWorld"}));
    atmosphere_ = get_node_or_null(editor_hint
        ? godot::NodePath{"../../../PhysicalAtmosphere"}
        : godot::NodePath{"../../PhysicalAtmosphere"});

    if (terrain_material_.is_null()) {
        godot::UtilityFunctions::push_error(
            "[TerrainWorld3D] terrain_material is unassigned; streamed terrain would use "
            "Godot's fallback material. Restore res://materials/terrain.gdshader on the scene node.");
    }
    if (visual_settings_.is_null()) {
        godot::UtilityFunctions::push_warning(
            "[TerrainWorld3D] visual_settings is unassigned; typed terrain and atmosphere "
            "configuration will not be applied.");
    }
    apply_visual_configuration();
    // The editor preview is an authoring aid. Dense vegetation MultiMeshes
    // remain a game/runtime concern; creating them while the editor is also
    // rendering its own viewport can stall Metal's command queue.
    if (!editor_hint) create_vegetation_resources();
    const auto user_arguments = godot::OS::get_singleton()->get_cmdline_user_args();
    const godot::String bookmark_prefix{"--terrain-bookmark="};
    const godot::String map_prefix{"--terrain-map="};
    for (std::int64_t index = 0; index < user_arguments.size(); ++index) {
        const auto argument = user_arguments[index];
        if (argument.begins_with(bookmark_prefix)) {
            pending_camera_bookmark_ = std::clamp(
                static_cast<std::int32_t>(argument.substr(bookmark_prefix.length()).to_int()), 1, 6);
        } else if (argument.begins_with(map_prefix)) {
            map_path_ = argument.substr(map_prefix.length()).strip_edges();
        }
    }
    set_process_unhandled_input(!editor_hint);
    godot::UtilityFunctions::print(
        "[TerrainWorld3D] GDExtension ready; source=",
        map_path_.is_empty() ? server_url_ : map_path_,
        ", streaming=", streaming_enabled_);
    emit_signal("streaming_state_changed", status_);
    if (streaming_enabled_) start_initial_load();
}

void TerrainWorld3D::_process(const double delta) {
    const auto editor_hint = godot::Engine::get_singleton()->is_editor_hint();
    if (editor_hint && !editor_streaming_mode_) return;

    std::optional<LoadResult> result;
    {
        std::scoped_lock lock{result_mutex_};
        result.swap(completed_load_);
    }
    if (result) install_initial_tile(std::move(*result));
    if (streamer_) {
        if (editor_hint) {
            // Limit editor streaming to twenty update/upload slices per second.
            // Camera observation still updates every frame, while GPU resource
            // creation is deliberately paced to avoid Metal fence timeouts.
            constexpr double EditorStreamingIntervalSeconds = 0.05;
            editor_streaming_accumulator_seconds_ += delta;
            if (editor_streaming_accumulator_seconds_ >= EditorStreamingIntervalSeconds) {
                editor_streaming_accumulator_seconds_ = 0.0;
                update_streaming();
            }
        } else {
            update_streaming();
        }
    }
    if (!editor_hint) update_vegetation_presentation(delta);
}

void TerrainWorld3D::_unhandled_input(const godot::Ref<godot::InputEvent>& event) {
    if (godot::Engine::get_singleton()->is_editor_hint()) return;
    const godot::Ref<godot::InputEventKey> key = event;
    if (!key.is_valid() || !key->is_pressed() || key->is_echo()) return;
    const auto physical_key = key->get_physical_keycode();
    if (physical_key == godot::KEY_G) {
        set_debug_view((debug_view_ + 1) % static_cast<std::int32_t>(debug_view_names.size()));
        if (auto* viewport = get_viewport(); viewport != nullptr) viewport->set_input_as_handled();
        return;
    }
    if (physical_key == godot::KEY_R) {
        request_random_world();
        if (auto* viewport = get_viewport(); viewport != nullptr) viewport->set_input_as_handled();
        return;
    }
    if (physical_key == godot::KEY_9) {
        wireframe_enabled_ = !wireframe_enabled_;
        update_debug_presentation();
        godot::UtilityFunctions::print(
            "[TerrainWorld3D] Wireframe ", wireframe_enabled_ ? "enabled" : "disabled");
        if (auto* viewport = get_viewport(); viewport != nullptr) viewport->set_input_as_handled();
        return;
    }
    if (physical_key >= godot::KEY_1 && physical_key <= godot::KEY_6) {
        apply_camera_bookmark(static_cast<std::int32_t>(physical_key - godot::KEY_1) + 1);
        if (auto* viewport = get_viewport(); viewport != nullptr) viewport->set_input_as_handled();
    }
}

void TerrainWorld3D::set_server_url(const godot::String& value) {
    server_url_ = value.strip_edges();
}

godot::String TerrainWorld3D::get_server_url() const {
    return server_url_;
}

void TerrainWorld3D::set_map_path(const godot::String& value) {
    map_path_ = value.strip_edges();
}

godot::String TerrainWorld3D::get_map_path() const {
    return map_path_;
}

void TerrainWorld3D::set_streaming_enabled(const bool value) {
    streaming_enabled_ = value;
    if (value && is_inside_tree() && !load_started_
        && (!godot::Engine::get_singleton()->is_editor_hint() || editor_streaming_mode_)) {
        start_initial_load();
    }
}

bool TerrainWorld3D::is_streaming_enabled() const noexcept {
    return streaming_enabled_;
}

void TerrainWorld3D::set_editor_streaming_mode(const bool value) {
    editor_streaming_mode_ = value;
}

bool TerrainWorld3D::is_editor_streaming_mode() const noexcept {
    return editor_streaming_mode_;
}

void TerrainWorld3D::set_editor_observer(
    const godot::Vector3& position,
    const godot::Vector3& view_direction,
    const double vertical_fov_degrees,
    const double viewport_height) {
    editor_observer_position_ = {
        static_cast<double>(position.x),
        static_cast<double>(position.y),
        static_cast<double>(position.z),
    };
    const auto direction = glm::dvec3{
        static_cast<double>(view_direction.x),
        static_cast<double>(view_direction.y),
        static_cast<double>(view_direction.z),
    };
    const auto direction_length = glm::length(direction);
    editor_observer_direction_ = direction_length > 0.000001
        ? direction / direction_length
        : glm::dvec3{0.0, 0.0, -1.0};
    editor_observer_vertical_fov_radians_ = std::clamp(
        vertical_fov_degrees, 20.0, 120.0) * std::numbers::pi / 180.0;
    editor_observer_viewport_height_ = std::max(viewport_height, 1.0);
}

void TerrainWorld3D::set_terrain_material(const godot::Ref<godot::Material>& value) {
    terrain_material_ = value;
    apply_material_configuration();
    update_debug_presentation();
}

godot::Ref<godot::Material> TerrainWorld3D::get_terrain_material() const {
    return terrain_material_;
}

void TerrainWorld3D::set_visual_settings(const godot::Ref<WorldVisualSettings>& value) {
    if (visual_settings_ == value) return;
    const auto callback = callable_mp(this, &TerrainWorld3D::on_visual_settings_changed);
    if (visual_settings_.is_valid() && visual_settings_->is_connected("changed", callback)) {
        visual_settings_->disconnect("changed", callback);
    }
    visual_settings_ = value;
    if (visual_settings_.is_valid()) {
        visual_settings_->connect("changed", callback);
        visuals_ = visual_settings_->to_world_config();
        debug_view_ = std::clamp(
            static_cast<std::int32_t>(visual_settings_->get_default_debug_view()),
            0,
            static_cast<std::int32_t>(debug_view_names.size()) - 1);
    }
    if (is_inside_tree()
        && (!godot::Engine::get_singleton()->is_editor_hint() || editor_streaming_mode_)) {
        apply_visual_settings(false);
    }
}

godot::Ref<WorldVisualSettings> TerrainWorld3D::get_visual_settings() const {
    return visual_settings_;
}

void TerrainWorld3D::apply_visual_settings(const bool rebuild_derived) {
    if (visual_settings_.is_null()) return;
    visuals_ = visual_settings_->to_world_config();
    apply_visual_configuration();
    apply_water_configuration();
    if (tree_material_.is_valid()) {
        tree_material_->set_shader_parameter(
            "size_multiplier", visuals_.forest.billboardSizeMultiplier);
        tree_material_->set_shader_parameter(
            "near_pixel_threshold", visuals_.forest.nearPixelThreshold);
        tree_material_->set_shader_parameter(
            "mid_pixel_threshold", visuals_.forest.midPixelThreshold);
        tree_material_->set_shader_parameter(
            "geometry_cull_pixel_threshold", visuals_.forest.geometryCullPixelThreshold);
    }
    if (streamer_) {
        if (rebuild_derived) {
            clear_resident_tiles();
            if (auto* player = godot::Object::cast_to<PlayerController>(player_node_);
                player != nullptr) {
                player->set_flying(true);
            }
            set_status("rebuilding_terrain");
        }
        streamer_->setVisualConfig(visuals_, rebuild_derived);
    }
    update_debug_presentation();
    godot::UtilityFunctions::print(
        "[TerrainWorld3D] Visual settings applied; rebuild_derived=",
        rebuild_derived,
        ", terrain_sse_px=",
        visuals_.terrain.maxScreenSpaceErrorPixels,
        ", upload_budget_mb=",
        static_cast<std::int64_t>(
            visuals_.streaming.maxGpuUploadBytesPerFrame / (1'024U * 1'024U)));
}

void TerrainWorld3D::on_visual_settings_changed() {
    if (!godot::Engine::get_singleton()->is_editor_hint() || editor_streaming_mode_) {
        apply_visual_settings(false);
    }
}

void TerrainWorld3D::set_debug_view(const std::int32_t value) {
    const auto clamped = std::clamp(
        value,
        0,
        static_cast<std::int32_t>(debug_view_names.size()) - 1);
    if (debug_view_ == clamped) return;
    debug_view_ = clamped;
    update_debug_presentation();
    if (is_inside_tree() && !godot::Engine::get_singleton()->is_editor_hint()) {
        godot::UtilityFunctions::print(
            "[TerrainWorld3D] Debug view: ", debug_view_names[static_cast<std::size_t>(debug_view_)]);
    }
}

std::int32_t TerrainWorld3D::get_debug_view() const noexcept {
    return debug_view_;
}

void TerrainWorld3D::apply_camera_bookmark(const std::int32_t bookmark) {
    if (!streamer_ || bookmark < 1 || bookmark > 6) return;
    auto* player = godot::Object::cast_to<PlayerController>(player_node_);
    if (player == nullptr) return;

    const auto local_position = player->get_global_position();
    const auto absolute_x = floating_origin_.x + static_cast<double>(local_position.x);
    const auto absolute_z = floating_origin_.y + static_cast<double>(local_position.z);
    const auto ground = streamer_->sampleHeight(absolute_x, absolute_z);
    if (!ground) {
        pending_camera_bookmark_ = bookmark;
        godot::UtilityFunctions::push_warning(
            "[TerrainWorld3D] Camera bookmark deferred: detailed terrain is not ready here");
        return;
    }
    pending_camera_bookmark_ = 0;

    // Eye altitude is the camera's physical height above sampled terrain.
    // This finite island world is authored for aircraft views, not orbit.
    static constexpr std::array<double, 6> eye_altitudes{
        1.68, 35.0, 3'000.0, 8'000.0, 15'000.0, 15'000.0};
    static constexpr std::array<double, 6> pitches{
        -0.10, -0.10, -0.48, -0.70, -1.32, -0.14};
    // Bookmark two faces back across the loaded basin instead of directly
    // into the nearest slope, making it a useful ground-level visual check.
    static constexpr std::array<double, 6> yaws{
        0.0, std::numbers::pi, 0.0, 0.0, 0.0, 0.0};
    const auto index = static_cast<std::size_t>(bookmark - 1);
    constexpr double eye_height = 1.68;
    constexpr double maximum_eye_altitude_asl = 15'000.0;
    const auto target_eye_altitude = std::min(
        static_cast<double>(*ground) + eye_altitudes[index],
        maximum_eye_altitude_asl);
    player->set_global_position({
        local_position.x,
        static_cast<godot::real_t>(target_eye_altitude - eye_height),
        local_position.z,
    });
    player->set_flying(bookmark != 1);
    player->set_look_direction(yaws[index], pitches[index]);
    godot::UtilityFunctions::print(
        "[TerrainWorld3D] Camera bookmark ", bookmark,
        "; eye_asl=", target_eye_altitude,
        " m, eye_agl=", target_eye_altitude - static_cast<double>(*ground),
        " m, pitch=", pitches[index],
        ", absolute_xz=(", absolute_x, ", ", absolute_z, ")");

}

void TerrainWorld3D::request_random_world() {
    if (!streamer_ || spawn_pending_ || load_started_) return;
    if (!map_path_.is_empty()) {
        godot::UtilityFunctions::push_warning(
            "[TerrainWorld3D] Random terrain is unavailable for a fixed .tmap; "
            "select another entry from the terrain map catalog instead");
        return;
    }

    std::random_device entropy;
    const auto seed = (static_cast<std::uint64_t>(entropy()) << 32U)
        ^ static_cast<std::uint64_t>(entropy());
    clear_resident_tiles();
    loaded_tile_.reset();
    streamer_->regenerate(seed);
    spawn_pending_ = true;
    spawn_search_attempts_ = 0;
    if (auto* player = godot::Object::cast_to<PlayerController>(player_node_);
        player != nullptr) {
        set_player_absolute_position(*player, glm::dvec3{240.0, 600.0, 240.0});
        player->set_flying(true);
    }

    set_status("generating_terrain");
    emit_signal("world_info_ready", seed_text(seed));
    godot::UtilityFunctions::print("[TerrainWorld3D] Random terrain requested; seed=", seed);
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
    const auto editor_hint = godot::Engine::get_singleton()->is_editor_hint();
    if (load_started_ || (editor_hint && !editor_streaming_mode_)) return;
    load_started_ = true;
    pending_tile_count_ = 1;
    set_status("discovering_world");
    set_process(true);

    const auto source_text = map_path_.is_empty() ? server_url_ : map_path_;
    const auto encoded_source = source_text.utf8();
    const std::string terrain_source{
        encoded_source.get_data(), static_cast<std::size_t>(encoded_source.length())};
    const auto visuals = visuals_;
    const auto editor_initial_position = editor_observer_position_;
    const auto use_editor_observer = editor_hint && editor_streaming_mode_;
    load_thread_ = std::jthread([
        this,
        terrain_source,
        visuals,
        editor_initial_position,
        use_editor_observer](const std::stop_token stop_token) {
        LoadResult result;
        try {
            TerrainClient client{terrain_source};
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
            if (use_editor_observer) {
                detailed_key.x = static_cast<std::int64_t>(
                    std::floor(editor_initial_position.x / detailed_spec.tileWorldSize));
                detailed_key.z = static_cast<std::int64_t>(
                    std::floor(editor_initial_position.z / detailed_spec.tileWorldSize));
            } else {
                std::mt19937_64 random{seed};
                std::uniform_int_distribution<std::int64_t> locator_coordinate{-7, 7};
                constexpr std::int32_t maximum_locator_attempts = 9;
                for (std::int32_t attempt = 0; attempt < maximum_locator_attempts; ++attempt) {
                    const auto locator_key = attempt == 0
                        ? TileKey{locator_spec.level, 0, 0}
                        : TileKey{
                            locator_spec.level,
                            locator_coordinate(random),
                            locator_coordinate(random)};
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
    load_started_ = false;
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
    const auto editor_hint = godot::Engine::get_singleton()->is_editor_hint();
    floating_origin_ = editor_hint && editor_streaming_mode_
        ? glm::dvec2{}
        : result.tile->raw.origin;
    update_authored_world_origin();
    const auto install_editor_extras = !(editor_hint && editor_streaming_mode_);
    if (add_tile(result.tile, install_editor_extras, install_editor_extras) == nullptr) {
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
    if (!editor_hint) {
        if (auto* player = player_node_; player != nullptr) {
            player->set_global_position(spawn_world);
        }
    }

    loaded_tile_ = std::move(result.tile);
    streamer_ = std::move(result.streamer);
    const auto initial_observer = editor_hint && editor_streaming_mode_
        ? editor_observer_position_
        : glm::dvec3{
            floating_origin_.x + static_cast<double>(spawn_world.x),
            static_cast<double>(spawn_world.y),
            floating_origin_.y + static_cast<double>(spawn_world.z),
        };
    if (install_editor_extras) update_collision_neighborhood(initial_observer);
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
    if (pending_camera_bookmark_ != 0) {
        apply_camera_bookmark(pending_camera_bookmark_);
    }
}

godot::MeshInstance3D* TerrainWorld3D::add_tile(
    const TileMeshPtr& tile,
    const bool install_vegetation,
    const bool install_auxiliary) {
    if (!tile || heightmap_terrain_shader_.is_null()) return nullptr;
    const auto segments = tile->raw.spec.segments;
    auto grid_iterator = heightmap_grid_meshes_.find(segments);
    if (grid_iterator == heightmap_grid_meshes_.end()) {
        auto grid = make_heightmap_grid_mesh(segments);
        if (grid.is_null()) return nullptr;
        grid_iterator = heightmap_grid_meshes_.emplace(segments, std::move(grid)).first;
    }
    const auto& mesh = grid_iterator->second;
    const auto heightmap = make_heightmap_texture(tile->raw);
    const auto terrain_fields = make_terrain_field_texture(*tile, false);
    const auto biome_fields = make_terrain_field_texture(*tile, true);
    if (heightmap.is_null() || terrain_fields.is_null() || biome_fields.is_null()) return nullptr;

    auto* terrain_tiles = terrain_tiles_root_;
    if (terrain_tiles == nullptr) return nullptr;

    const auto key = tile->raw.key;
    if (resident_tiles_.contains(key)) remove_tile(key);

    const auto presentation_range = streamer_
        ? streamer_->presentationRange(key.level)
        : LodPresentationRange{0.0, tile->raw.spec.outerRadius};
    godot::Ref<godot::ShaderMaterial> material;
    material.instantiate();
    material->set_shader(heightmap_terrain_shader_);
    material->set_shader_parameter("heightmap", heightmap);
    material->set_shader_parameter("terrain_fields", terrain_fields);
    material->set_shader_parameter("biome_fields", biome_fields);
    const auto texture_width = static_cast<float>(tile->raw.width);
    const auto border = static_cast<float>(tile->raw.borderSamples);
    material->set_shader_parameter("height_uv_offset", godot::Vector2{
        (border + 0.5F) / texture_width,
        (border + 0.5F) / texture_width,
    });
    material->set_shader_parameter("height_uv_scale", godot::Vector2{
        static_cast<float>(segments) / texture_width,
        static_cast<float>(segments) / texture_width,
    });
    material->set_shader_parameter("grid_segments", static_cast<float>(segments));
    const auto next_segments = key.level <= 1 ? segments / 2 : segments;
    material->set_shader_parameter(
        "morph_grid_ratio",
        static_cast<float>(segments * 2) / static_cast<float>(next_segments));
    material->set_shader_parameter(
        "tile_world_size", static_cast<float>(tile->raw.spec.tileWorldSize));
    material->set_shader_parameter("tile_absolute_origin", godot::Vector2{
        static_cast<godot::real_t>(tile->raw.origin.x),
        static_cast<godot::real_t>(tile->raw.origin.y),
    });
    material->set_shader_parameter("morph_start_meters", static_cast<float>(
        presentation_range.innerRadius
            + (presentation_range.outerRadius - presentation_range.innerRadius) * 0.68));
    material->set_shader_parameter(
        "morph_end_meters", static_cast<float>(presentation_range.outerRadius));
    material->set_shader_parameter("albedo_gain", visuals_.terrain.albedoGain);
    material->set_shader_parameter("debug_view", debug_view_);

    auto* tile_node = memnew(godot::MeshInstance3D);
    tile_node->set_name(tile_node_name(key));
    tile_node->set_mesh(mesh);
    tile_node->set_material_override(material);
    tile_node->set_cast_shadows_setting(tile->raw.spec.castsShadow
        ? godot::GeometryInstance3D::SHADOW_CASTING_SETTING_ON
        : godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    tile_node->set_position({
        static_cast<godot::real_t>(tile->raw.origin.x - floating_origin_.x),
        0.0F,
        static_cast<godot::real_t>(tile->raw.origin.y - floating_origin_.y),
    });
    tile_node->set_extra_cull_margin(
        terrain_cull_margin(key, tile->raw.spec, presentation_range));
    tile_node->set_instance_shader_parameter("lod_level", static_cast<float>(key.level));
    tile_node->set_instance_shader_parameter(
        "floating_origin_xz",
        godot::Vector2{
            static_cast<godot::real_t>(floating_origin_.x),
            static_cast<godot::real_t>(floating_origin_.y),
        });
    float minimum_height = std::numeric_limits<float>::infinity();
    float maximum_height = -std::numeric_limits<float>::infinity();
    const auto& render_heights = tile->raw.renderElevations;
    if (!render_heights.empty()) {
        const auto extrema = std::ranges::minmax(render_heights);
        minimum_height = extrema.min;
        maximum_height = extrema.max;
    } else {
        const auto extrema = std::ranges::minmax(tile->raw.elevations);
        minimum_height = static_cast<float>(extrema.min);
        maximum_height = static_cast<float>(extrema.max);
    }
    tile_node->set_custom_aabb(godot::AABB{
        godot::Vector3{0.0F, minimum_height - 8.0F, 0.0F},
        godot::Vector3{
            static_cast<float>(tile->raw.spec.tileWorldSize),
            maximum_height - minimum_height + 16.0F,
            static_cast<float>(tile->raw.spec.tileWorldSize),
        },
    });
    terrain_tiles->add_child(tile_node);

    godot::OccluderInstance3D* occluder_node{};
    if (install_auxiliary && key.level == 0) {
        const auto occluder = make_terrain_occluder(*tile);
        if (occluder.is_valid()) {
            occluder_node = memnew(godot::OccluderInstance3D);
            occluder_node->set_name(tile_node_name(key) + "_Occluder");
            occluder_node->set_occluder(occluder);
            occluder_node->set_position(tile_node->get_position());
            terrain_tiles->add_child(occluder_node);
        }
    }

    auto resident = ResidentTile{
        .node = tile_node,
        .occluderNode = occluder_node,
        .collisionShape = install_auxiliary && key.level == 0
            ? make_terrain_collision_shape(*tile)
            : godot::Ref<godot::ConcavePolygonShape3D>{},
        .terrainMaterial = material,
        .spec = tile->raw.spec,
        .absoluteOrigin = tile->raw.origin,
        .estimatedRenderBytes = estimate_render_resource_bytes(*tile),
    };
    resident_tiles_.insert_or_assign(key, std::move(resident));
    if (streamer_ != nullptr) {
        streamer_->recordResidentRenderBytes(
            key, resident_tiles_.at(key).estimatedRenderBytes);
    }
    if (install_vegetation) add_vegetation(tile, resident_tiles_.at(key));
    lod_readiness_dirty_ = true;
    vegetation_debug_dirty_ = true;
    return tile_node;
}

void TerrainWorld3D::update_collision_neighborhood(const glm::dvec3& camera_position) {
    // Concave terrain shapes are expensive to build and only affect the
    // walking player. Keeping one for every cached L0 tile caused avoidable
    // main-thread stalls during flight and retained far-away physics objects.
    constexpr double create_distance_meters = 640.0;
    constexpr double remove_distance_meters = 960.0;
    const auto finest_level_active = streamer_ == nullptr
        || streamer_->activeMinimumLevel() == 0;

    ResidentTile* nearest_missing{};
    double nearest_missing_distance = std::numeric_limits<double>::infinity();
    for (auto& [key, resident] : resident_tiles_) {
        if (key.level != 0 || resident.node == nullptr) continue;
        const auto maximum = resident.absoluteOrigin
            + glm::dvec2{resident.spec.tileWorldSize, resident.spec.tileWorldSize};
        const auto dx = std::max({
            resident.absoluteOrigin.x - camera_position.x,
            0.0,
            camera_position.x - maximum.x,
        });
        const auto dz = std::max({
            resident.absoluteOrigin.y - camera_position.z,
            0.0,
            camera_position.z - maximum.y,
        });
        const auto distance = std::hypot(dx, dz);
        const auto wanted = streamer_ == nullptr || streamer_->isWanted(key);
        const auto should_keep = finest_level_active && wanted
            && distance <= remove_distance_meters;
        if (!should_keep && resident.collisionBody != nullptr) {
            resident.collisionBody->queue_free();
            resident.collisionBody = nullptr;
            continue;
        }
        if (resident.collisionBody == nullptr && finest_level_active && wanted
            && distance <= create_distance_meters && distance < nearest_missing_distance) {
            nearest_missing = &resident;
            nearest_missing_distance = distance;
        }
    }

    // Bound collision construction to one tile per frame. The closest missing
    // tile wins, so the surface under the player is always restored first.
    if (nearest_missing == nullptr || nearest_missing->collisionShape.is_null()) return;

    auto* body = memnew(godot::StaticBody3D);
    body->set_name("TerrainCollision");
    auto* shape = memnew(godot::CollisionShape3D);
    shape->set_shape(nearest_missing->collisionShape);
    body->add_child(shape);
    nearest_missing->node->add_child(body);
    nearest_missing->collisionBody = body;
}

void TerrainWorld3D::add_vegetation(const TileMeshPtr& tile, ResidentTile& resident) {
    static_cast<void>(resident);
    PendingVegetationInstall install{.tile = tile};
    while (!advance_vegetation_install(install)) {}
}

bool TerrainWorld3D::advance_vegetation_install(PendingVegetationInstall& install) {
    const auto& tile = install.tile;
    if (!tile || tile->raw.key.level > 1 || near_tree_billboard_mesh_.is_null()
        || far_tree_billboard_mesh_.is_null() || grass_clump_mesh_.is_null()) return true;
    const auto resident_iterator = resident_tiles_.find(tile->raw.key);
    if (resident_iterator == resident_tiles_.end()) return true;
    auto& resident = resident_iterator->second;
    auto* vegetation = vegetation_root_;
    if (vegetation == nullptr) return true;

    if (tile->raw.key.level == 0 && install.auxiliaryPhase < 2U) {
        if (install.auxiliaryPhase == 0U && resident.node != nullptr) {
            const auto occluder = make_terrain_occluder(*tile);
            if (occluder.is_valid()) {
                auto* terrain_tiles = terrain_tiles_root_;
                if (terrain_tiles != nullptr) {
                    auto* occluder_node = memnew(godot::OccluderInstance3D);
                    occluder_node->set_name(tile_node_name(tile->raw.key) + "_Occluder");
                    occluder_node->set_occluder(occluder);
                    occluder_node->set_position(resident.node->get_position());
                    terrain_tiles->add_child(occluder_node);
                    resident.occluderNode = occluder_node;
                }
            }
        } else if (install.auxiliaryPhase == 1U) {
            resident.collisionShape = make_terrain_collision_shape(*tile);
        }
        ++install.auxiliaryPhase;
        return false;
    }
    install.auxiliaryPhase = 3U;

    const auto tile_position = godot::Vector3{
        static_cast<godot::real_t>(tile->raw.origin.x - floating_origin_.x),
        0.0F,
        static_cast<godot::real_t>(tile->raw.origin.y - floating_origin_.y),
    };
    if (install.treeRangeIndex < tile->treeRanges.size()
        && !tile->trees.empty() && tree_material_.is_valid()) {
        const auto& billboard_mesh = tile->raw.key.level == 0
            ? near_tree_billboard_mesh_ : far_tree_billboard_mesh_;
        const auto batch_index = install.treeRangeIndex++;
        const auto& range = tile->treeRanges[batch_index];
        if (static_cast<std::size_t>(range.offset) + range.count <= tile->trees.size()) {
            const auto batch = std::span<const VegetationInstance>{tile->trees}.subspan(
                range.offset, range.count);
            auto multimesh = make_vegetation_multimesh(batch, billboard_mesh, range.center);
            if (multimesh.is_valid()) {
                auto* node = memnew(godot::MultiMeshInstance3D);
                node->set_name(tile_node_name(tile->raw.key) + "_Trees_"
                    + godot::String::num_int64(static_cast<std::int64_t>(batch_index)));
                node->set_multimesh(multimesh);
                node->set_material_override(tree_material_);
                node->set_position(tile_position + godot::Vector3{range.center.x, 0.0F, range.center.y});
                node->set_extra_cull_margin(28.0F);
                node->set_instance_shader_parameter(
                    "camera_facing", tile->raw.key.level == 0 ? 0.0F : 1.0F);
                if (tile->raw.key.level == 0) {
                    node->set_visibility_range_end(650.0F);
                    node->set_visibility_range_end_margin(90.0F);
                } else {
                    node->set_visibility_range_begin(550.0F);
                    node->set_visibility_range_begin_margin(90.0F);
                    node->set_visibility_range_end(2'800.0F);
                    node->set_visibility_range_end_margin(240.0F);
                }
                node->set_cast_shadows_setting(
                    godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
                node->set_visibility_range_fade_mode(
                    godot::GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
                vegetation->add_child(node);
                resident.vegetationNodes.push_back({
                    .node = node,
                    .localOffset = range.center,
                    .radius = range.radius,
                    .instanceCount = range.count,
                    .kind = tile->raw.key.level == 0
                        ? VegetationKind::NearTrees : VegetationKind::FarTrees,
                    .shadowEligible = tile->raw.key.level == 0,
                });
            }
        }
        return false;
    }
    resident.treeCount = tile->trees.size();

    if (tile->raw.key.level == 0 && !tile->grass.empty() && grass_material_.is_valid()
        && install.grassRowBegin < tile->grassRanges.size()) {
        // Keep the core's 64 m grass chunks as separate batches. Combining a
        // whole 480 m row made Godot retain distant blades whenever the row
        // origin was nearby, producing dense contour-like lines and defeating
        // the visibility range. One chunk is installed per work unit so a
        // dense tile still cannot monopolize a frame.
        const auto row_begin = install.grassRowBegin;
        const auto row_end = row_begin + 1U;
        install.grassRowBegin = row_end;
        const auto offset = tile->grassRanges[row_begin].offset;
        const auto end_offset = offset + tile->grassRanges[row_begin].count;
        if (end_offset > tile->grass.size() || end_offset <= offset) {
            return false;
        }
        const auto local_origin = tile->grassRanges[row_begin].center;
        const auto batch = std::span<const VegetationInstance>{tile->grass}.subspan(
            offset, end_offset - offset);
        auto multimesh = make_vegetation_multimesh(batch, grass_clump_mesh_, local_origin);
        if (multimesh.is_null()) {
            return false;
        }
        auto* node = memnew(godot::MultiMeshInstance3D);
        node->set_name(tile_node_name(tile->raw.key) + "_Grass_"
            + godot::String::num_int64(static_cast<std::int64_t>(row_begin)));
        node->set_multimesh(multimesh);
        node->set_material_override(grass_material_);
        node->set_position(tile_position + godot::Vector3{local_origin.x, 0.0F, local_origin.y});
        node->set_extra_cull_margin(4.0F);
        node->set_visibility_range_end(160.0F);
        node->set_visibility_range_end_margin(28.0F);
        node->set_visibility_range_fade_mode(
            godot::GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
        node->set_cast_shadows_setting(
            godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
        vegetation->add_child(node);
        resident.vegetationNodes.push_back({
            .node = node,
            .localOffset = local_origin,
            .radius = tile->grassRanges[row_begin].radius,
            .instanceCount = end_offset - offset,
            .kind = VegetationKind::Grass,
        });
        return false;
    }
    resident.grassCount = tile->grass.size();
    return true;
}

void TerrainWorld3D::update_pending_vegetation_installs() {
    constexpr double maximum_slice_milliseconds = 4.0;
    const auto started = std::chrono::steady_clock::now();
    while (!pending_vegetation_installs_.empty()) {
        auto& install = pending_vegetation_installs_.front();
        const auto key = install.tile ? install.tile->raw.key : TileKey{};
        const auto complete = advance_vegetation_install(install);
        if (complete) {
            if (streamer_ != nullptr) streamer_->releaseUploadedPayload(key);
            if (loaded_tile_ && loaded_tile_->raw.key == key) loaded_tile_.reset();
            emit_signal("terrain_event", "added", tile_key_text(key));
            pending_vegetation_installs_.pop_front();
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (elapsed >= maximum_slice_milliseconds) break;
    }
}

void TerrainWorld3D::remove_tile(const TileKey& key) {
    std::erase_if(pending_vegetation_installs_, [&key](const auto& install) {
        return install.tile && install.tile->raw.key == key;
    });
    const auto iterator = resident_tiles_.find(key);
    if (iterator == resident_tiles_.end()) return;
    if (streamer_ != nullptr) streamer_->releaseResidentRenderBytes(key);
    if (iterator->second.node != nullptr) {
        if (auto* parent = iterator->second.node->get_parent(); parent != nullptr) {
            parent->remove_child(iterator->second.node);
        }
        iterator->second.node->queue_free();
    }
    if (iterator->second.occluderNode != nullptr) {
        if (auto* parent = iterator->second.occluderNode->get_parent(); parent != nullptr) {
            parent->remove_child(iterator->second.occluderNode);
        }
        iterator->second.occluderNode->queue_free();
    }
    for (const auto& vegetation : iterator->second.vegetationNodes) {
        if (vegetation.node == nullptr) continue;
        if (auto* parent = vegetation.node->get_parent(); parent != nullptr) {
            parent->remove_child(vegetation.node);
        }
        vegetation.node->queue_free();
    }
    resident_tiles_.erase(iterator);
    lod_readiness_dirty_ = true;
    vegetation_debug_dirty_ = true;
}

void TerrainWorld3D::update_streaming() {
    const auto editor_hint = godot::Engine::get_singleton()->is_editor_hint();
    auto* player = player_node_;
    glm::dvec3 absolute_position{};
    if (editor_hint && editor_streaming_mode_) {
        absolute_position = editor_observer_position_;
    } else {
        if (player == nullptr) return;
        rebase_if_needed(*player);
        const auto local_position = player->get_global_position();
        absolute_position = {
            floating_origin_.x + static_cast<double>(local_position.x),
            static_cast<double>(local_position.y),
            floating_origin_.y + static_cast<double>(local_position.z),
        };
    }
    update_flat_world_anchor({absolute_position.x, absolute_position.z});
    auto view_direction = editor_hint && editor_streaming_mode_
        ? editor_observer_direction_
        : glm::dvec3{0.0, 0.0, -1.0};
    auto vertical_fov_radians = editor_hint && editor_streaming_mode_
        ? editor_observer_vertical_fov_radians_
        : std::numbers::pi / 3.0;
    auto* camera = camera_;
    if (!editor_hint && camera != nullptr) {
        const auto direction = -camera->get_global_basis().get_column(2);
        view_direction = {direction.x, direction.y, direction.z};
        vertical_fov_radians = static_cast<double>(camera->get_fov())
            * std::numbers::pi / 180.0;
    }
    const auto camera_height = editor_hint && editor_streaming_mode_
        ? absolute_position.y
        : camera != nullptr
        ? static_cast<double>(camera->get_global_position().y)
        : absolute_position.y + 1.68;
    // Atmospheric density uses altitude above mean sea level, not local AGL.
    const auto viewer_altitude = std::max(
        0.0, camera_height - static_cast<double>(visuals_.atmosphere.baseHeightMeters));
    const auto signed_viewer_height =
        camera_height - static_cast<double>(visuals_.atmosphere.baseHeightMeters);
    const auto terrain_height = streamer_->sampleHeight(absolute_position.x, absolute_position.z);
    const auto viewer_height_agl = terrain_height
        ? std::max(0.0, camera_height - static_cast<double>(*terrain_height))
        : viewer_altitude;
    if (camera != nullptr) {
        if (std::abs(static_cast<double>(camera->get_far())
            - FlatWorldViewDistanceMetres) >= 1.0) {
            camera->set_far(static_cast<godot::real_t>(FlatWorldViewDistanceMetres));
        }
    }
    update_flat_world_presentation(
        viewer_altitude,
        camera != nullptr ? static_cast<double>(camera->get_far()) : 1'000'000.0,
        viewer_height_agl,
        signed_viewer_height);
    const auto* viewport = get_viewport();
    const auto viewport_height = editor_hint && editor_streaming_mode_
        ? editor_observer_viewport_height_
        : viewport != nullptr
        ? std::max(1.0, static_cast<double>(viewport->get_visible_rect().size.y))
        : 900.0;

    auto stream_position = absolute_position;
    if (!editor_hint
        && !streamer_->sampleHeight(absolute_position.x, absolute_position.z)) {
        stream_position.y = 1.68;
    }
    streamer_->update(
        stream_position, view_direction, vertical_fov_radians, viewport_height);

    const auto editor_preview = editor_hint && editor_streaming_mode_;
    if (!editor_preview) update_pending_vegetation_installs();

    bool tiles_changed{};
    const auto install_slice_started = std::chrono::steady_clock::now();
    constexpr double maximum_install_slice_milliseconds = 4.0;
    const auto maximum_uploads = std::max<std::size_t>(
        visuals_.streaming.maxGpuUploadsPerFrame, 1U);
    std::size_t processed_uploads{};
    while (processed_uploads < maximum_uploads) {
        if (pending_vegetation_installs_.size() >= 4U) break;
        auto events = streamer_->drainEvents(
            1U, visuals_.streaming.maxGpuUploadBytesPerFrame);
        if (events.empty()) break;
        for (auto& event : events) {
        const auto key_text = tile_key_text(event.key);
        if (event.type == TerrainEvent::Type::Added && event.tile) {
            if (add_tile(event.tile, false, false) == nullptr) {
                godot::UtilityFunctions::push_error(
                    godot::String{"[TerrainWorld3D] Could not upload streamed tile "} + key_text);
                continue;
            }
            if (editor_preview) {
                streamer_->releaseUploadedPayload(event.key);
                emit_signal("terrain_event", "added", key_text);
            } else {
                pending_vegetation_installs_.push_back({.tile = event.tile});
            }
            tiles_changed = true;
            ++processed_uploads;
        } else if (event.type == TerrainEvent::Type::Removed) {
            remove_tile(event.key);
            tiles_changed = true;
            emit_signal("terrain_event", "removed", key_text);
        } else if (event.type == TerrainEvent::Type::Failed) {
            godot::UtilityFunctions::push_warning(
                godot::String{"[TerrainWorld3D] Terrain stream failed for "}
                + key_text + ": " + godot::String{event.message.c_str()});
            emit_signal("terrain_event", "failed", key_text);
        }
        }
        const auto slice_milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - install_slice_started).count();
        if (processed_uploads > 0U && slice_milliseconds >= maximum_install_slice_milliseconds) {
            break;
        }
    }

    pending_tile_count_ = static_cast<std::int64_t>(streamer_->pendingTileCount());
    resident_tile_count_ = static_cast<std::int64_t>(resident_tiles_.size());
    if (pending_camera_bookmark_ != 0 && tiles_changed) {
        apply_camera_bookmark(pending_camera_bookmark_);
    }
    if (spawn_pending_ && player != nullptr) update_spawn_search(*player, absolute_position);
    if (presented_minimum_lod_ != streamer_->activeMinimumLevel()) {
        lod_readiness_dirty_ = true;
    }
    if (presented_lod_revision_ != streamer_->presentationRevision()) {
        lod_readiness_dirty_ = true;
    }
    const auto lod_distance = glm::length(glm::dvec2{absolute_position.x, absolute_position.z}
        - last_lod_update_position_);
    if (tiles_changed || lod_readiness_dirty_ || !std::isfinite(lod_distance) || lod_distance >= 32.0) {
        update_lod_readiness(absolute_position);
    }
    if (!editor_preview) update_collision_neighborhood(absolute_position);
}

void TerrainWorld3D::update_flat_world_anchor(const glm::dvec2& camera_absolute_xz) {
    const auto anchor = godot::Vector2{
        static_cast<godot::real_t>(camera_absolute_xz.x),
        static_cast<godot::real_t>(camera_absolute_xz.y),
    };
    if (auto* rendering_server = godot::RenderingServer::get_singleton();
        rendering_server != nullptr) {
        rendering_server->global_shader_parameter_set(
            "terrain_camera_absolute_xz", anchor);
    }
    const godot::Ref<godot::ShaderMaterial> terrain_material = terrain_material_;
    if (terrain_material.is_valid()) {
        terrain_material->set_shader_parameter("camera_absolute_xz", anchor);
    }

    auto* ocean = ocean_;
    if (ocean == nullptr) return;
    const godot::Ref<godot::ShaderMaterial> water_material = ocean->get_active_material(0);
    if (water_material.is_null()) return;
    water_material->set_shader_parameter("camera_absolute_xz", anchor);
}

void TerrainWorld3D::update_authored_world_origin() {
    if (authored_world_root_ == nullptr) return;
    authored_world_root_->set_position({
        static_cast<godot::real_t>(-floating_origin_.x),
        0.0F,
        static_cast<godot::real_t>(-floating_origin_.y),
    });
}

void TerrainWorld3D::update_flat_world_presentation(
    const double viewer_altitude_meters,
    const double camera_far_meters,
    const double viewer_height_agl_meters,
    const double viewer_height_signed_meters) {
    constexpr double BaseOceanHalfExtentMetres = 32'768.0;
    const auto viewer_underwater = viewer_height_signed_meters < 0.0;
    const auto required_half_extent = std::max(
        BaseOceanHalfExtentMetres,
        camera_far_meters * 1.05);
    const auto ocean_extent_scale = required_half_extent / BaseOceanHalfExtentMetres;
    auto* ocean = ocean_;
    if (ocean != nullptr) {
        const godot::Ref<godot::ShaderMaterial> water_material =
            ocean->get_active_material(0);
        if (water_material.is_valid()) {
            water_material->set_shader_parameter(
                "ocean_view_distance_meters",
                static_cast<float>(camera_far_meters));
            water_material->set_shader_parameter(
                "viewer_underwater", viewer_underwater ? 1.0F : 0.0F);
        }
    }
    const auto ocean_scale_changed = last_ocean_extent_scale_ < 0.0
        || std::abs(ocean_extent_scale - last_ocean_extent_scale_)
            >= std::max(0.02, last_ocean_extent_scale_ * 0.02);
    if (ocean_scale_changed) {
        if (ocean != nullptr) {
            const godot::Ref<godot::ShaderMaterial> water_material =
                ocean->get_active_material(0);
            if (water_material.is_valid()) {
                water_material->set_shader_parameter(
                    "ocean_extent_scale", static_cast<float>(ocean_extent_scale));
            }
            ocean->set_custom_aabb(godot::AABB{
                godot::Vector3{
                    static_cast<godot::real_t>(-required_half_extent),
                    -8.0F,
                    static_cast<godot::real_t>(-required_half_extent)},
                godot::Vector3{
                    static_cast<godot::real_t>(required_half_extent * 2.0),
                    16.0F,
                    static_cast<godot::real_t>(required_half_extent * 2.0)}});
        }
        last_ocean_extent_scale_ = ocean_extent_scale;
    }

    if (atmosphere_ != nullptr && atmosphere_->has_method("set_viewer_state")) {
        atmosphere_->call(
            "set_viewer_state",
            viewer_altitude_meters,
            camera_far_meters * 0.98,
            viewer_height_agl_meters,
            viewer_height_signed_meters);
    }
}

void TerrainWorld3D::update_spawn_search(
    godot::Node3D& player,
    const glm::dvec3& absolute_position) {
    if (!streamer_ || !streamer_->readyNearPlayer(absolute_position)) return;

    if (const auto land = streamer_->findWalkableLand(absolute_position, 820.0, true)) {
        set_player_absolute_position(player, {land->x, land->y + 0.05, land->z});
        if (auto* controller = godot::Object::cast_to<PlayerController>(&player);
            controller != nullptr) {
            controller->set_flying(false);
        }
        spawn_pending_ = false;
        set_status("terrain_ready");
        godot::UtilityFunctions::print(
            "[TerrainWorld3D] Random-world spawn ready; relocations=",
            static_cast<std::int64_t>(spawn_search_attempts_),
            ", absolute=(", land->x, ", ", land->z,
            "), elevation=", land->y, " m");
        return;
    }

    relocate_spawn_search(player, absolute_position);
}

void TerrainWorld3D::relocate_spawn_search(
    godot::Node3D& player,
    const glm::dvec3& absolute_position) {
    std::optional<glm::dvec3> target;
    if (spawn_search_attempts_ == 0 && streamer_) {
        target = streamer_->findWalkableLand(absolute_position, 5'800.0, false);
    }
    if (!target) {
        const auto seed = streamer_ ? streamer_->seed() : 0U;
        std::mt19937_64 generator{
            seed ^ (0x9e3779b97f4a7c15ULL
                * static_cast<std::uint64_t>(spawn_search_attempts_ + 1U))};
        std::uniform_real_distribution<double> angle_distribution{
            0.0, std::numbers::pi_v<double> * 2.0};
        std::uniform_real_distribution<double> distance_distribution{20'000.0, 220'000.0};
        const auto angle = angle_distribution(generator);
        const auto distance = distance_distribution(generator);
        target = glm::dvec3{
            absolute_position.x + std::cos(angle) * distance,
            1'200.0,
            absolute_position.z + std::sin(angle) * distance,
        };
    }

    ++spawn_search_attempts_;
    target->y = 1'200.0;
    set_player_absolute_position(player, *target);
    if (auto* controller = godot::Object::cast_to<PlayerController>(&player);
        controller != nullptr) {
        controller->set_flying(true);
    }
    set_status(
        godot::String{"searching_land_region_"}
        + godot::String::num_int64(static_cast<std::int64_t>(spawn_search_attempts_)));
    godot::UtilityFunctions::print(
        "[TerrainWorld3D] Spawn search relocation ",
        static_cast<std::int64_t>(spawn_search_attempts_),
        "; absolute_xz=(", target->x, ", ", target->z, ")");
}

void TerrainWorld3D::set_player_absolute_position(
    godot::Node3D& player,
    const glm::dvec3& absolute_position) {
    player.set_global_position({
        static_cast<godot::real_t>(absolute_position.x - floating_origin_.x),
        static_cast<godot::real_t>(absolute_position.y),
        static_cast<godot::real_t>(absolute_position.z - floating_origin_.y),
    });
    rebase_if_needed(player);
}

void TerrainWorld3D::clear_resident_tiles() {
    std::vector<TileKey> keys;
    keys.reserve(resident_tiles_.size());
    for (const auto& [key, unused] : resident_tiles_) {
        static_cast<void>(unused);
        keys.push_back(key);
    }
    for (const auto& key : keys) remove_tile(key);
    resident_tile_count_ = 0;
    lod_readiness_dirty_ = true;
    vegetation_debug_dirty_ = true;
}

void TerrainWorld3D::update_lod_readiness(const glm::dvec3& camera_position) {
    const auto minimum_lod = streamer_ ? streamer_->activeMinimumLevel() : 0;
    std::unordered_set<TileKey, TileKeyHash> candidates;
    candidates.reserve(resident_tiles_.size());
    std::int32_t maximum_level = minimum_lod;
    for (const auto& [key, tile] : resident_tiles_) {
        static_cast<void>(tile);
        if (key.level < minimum_lod || (streamer_ != nullptr && !streamer_->isWanted(key))) {
            continue;
        }
        candidates.insert(key);
        maximum_level = std::max(maximum_level, key.level);
    }

    const auto parent_coordinate = [](const std::int64_t value) {
        return value >= 0 ? value / 2 : -((-value + 1) / 2);
    };
    const auto parent_key = [&parent_coordinate](const TileKey& key) {
        return TileKey{
            key.level + 1,
            parent_coordinate(key.x),
            parent_coordinate(key.z),
        };
    };
    const auto has_ancestor = [&candidates, &parent_key, maximum_level](TileKey key) {
        while (key.level < maximum_level) {
            key = parent_key(key);
            if (candidates.contains(key)) return true;
        }
        return false;
    };

    // Select a forest of exclusive quadtree roots. A parent is replaced only
    // when all four immediate children are resident and wanted. This gives
    // continuous coverage without rendering overlapping levels or using a
    // fragment shader as a geometry mask.
    std::unordered_set<TileKey, TileKeyHash> selected;
    selected.reserve(candidates.size());
    std::function<void(const TileKey&)> refine = [&](const TileKey& key) {
        if (key.level <= minimum_lod) {
            selected.insert(key);
            return;
        }
        const auto child_level = key.level - 1;
        const std::array<TileKey, 4> children{
            TileKey{child_level, key.x * 2, key.z * 2},
            TileKey{child_level, key.x * 2 + 1, key.z * 2},
            TileKey{child_level, key.x * 2, key.z * 2 + 1},
            TileKey{child_level, key.x * 2 + 1, key.z * 2 + 1},
        };
        if (!std::ranges::all_of(children, [&candidates](const auto& child) {
                return candidates.contains(child);
            })) {
            selected.insert(key);
            return;
        }
        for (const auto& child : children) refine(child);
    };
    for (const auto& key : candidates) {
        if (!has_ancestor(key)
            && (streamer_ == nullptr || !streamer_->hasWantedAncestor(key))) {
            refine(key);
        }
    }

    for (auto& [key, tile] : resident_tiles_) {
        if (tile.node == nullptr) continue;
        // Resident nodes are also the bounded GPU revisit cache. A tile
        // outside the current clipmap used to remain visible and relied on a
        // fragment discard to hide every pixel, which still paid its draw and
        // full vertex cost. Keep the node resident but make it genuinely
        // dormant until the streamer wants that key again.
        const auto active = selected.contains(key);
        const auto wanted = streamer_ == nullptr || streamer_->isWanted(key);
        tile.node->set_visible(active);
        if (tile.occluderNode != nullptr) tile.occluderNode->set_visible(active);
        for (auto& vegetation : tile.vegetationNodes) {
            if (vegetation.node == nullptr) continue;
            // L1 trees are the continuous middle-distance forest
            // representation. Keep them resident across finer terrain
            // replacement and let their own visibility-range fade cross over
            // to L0 trees. Tying them to the exclusive terrain parent made
            // forests appear and disappear as rectangular LOD blocks.
            const auto vegetation_active = vegetation.kind == VegetationKind::FarTrees
                ? key.level == 1 && key.level >= minimum_lod && wanted
                : active;
            vegetation.node->set_visible(vegetation_active);
            if (!vegetation_active && vegetation.castsShadow) {
                vegetation.castsShadow = false;
                vegetation.node->set_cast_shadows_setting(
                    godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
            }
        }
        if (!active) continue;

        const auto neighbor_ratio_at = [&](const double absolute_x, const double absolute_z) {
            for (const auto& neighbor_key : selected) {
                if (neighbor_key == key) continue;
                const auto neighbor = resident_tiles_.find(neighbor_key);
                if (neighbor == resident_tiles_.end()) continue;
                const auto& candidate = neighbor->second;
                const auto maximum = candidate.absoluteOrigin
                    + glm::dvec2{candidate.spec.tileWorldSize};
                if (absolute_x < candidate.absoluteOrigin.x || absolute_x >= maximum.x
                    || absolute_z < candidate.absoluteOrigin.y || absolute_z >= maximum.y) {
                    continue;
                }
                const auto spacing = candidate.spec.tileWorldSize
                    / static_cast<double>(candidate.spec.segments);
                const auto own_spacing = tile.spec.tileWorldSize
                    / static_cast<double>(tile.spec.segments);
                return static_cast<float>(spacing / own_spacing);
            }
            return 1.0F;
        };
        const auto center_x = tile.absoluteOrigin.x + tile.spec.tileWorldSize * 0.5;
        const auto center_z = tile.absoluteOrigin.y + tile.spec.tileWorldSize * 0.5;
        const auto probe_offset = std::max(
            tile.spec.tileWorldSize / static_cast<double>(tile.spec.segments) * 0.25,
            0.25);
        const godot::Vector4 edge_grid_ratios{
            neighbor_ratio_at(center_x, tile.absoluteOrigin.y - probe_offset),
            neighbor_ratio_at(center_x,
                tile.absoluteOrigin.y + tile.spec.tileWorldSize + probe_offset),
            neighbor_ratio_at(tile.absoluteOrigin.x - probe_offset, center_z),
            neighbor_ratio_at(
                tile.absoluteOrigin.x + tile.spec.tileWorldSize + probe_offset, center_z),
        };
        const auto presentation_range = streamer_
            ? streamer_->presentationRange(key.level)
            : LodPresentationRange{tile.spec.innerRadius, tile.spec.outerRadius};
        tile.node->set_extra_cull_margin(
            terrain_cull_margin(key, tile.spec, presentation_range));
        if (tile.terrainMaterial.is_valid()) {
            tile.terrainMaterial->set_shader_parameter(
                "morph_start_meters",
                static_cast<float>(presentation_range.innerRadius
                    + (presentation_range.outerRadius - presentation_range.innerRadius) * 0.68));
            tile.terrainMaterial->set_shader_parameter(
                "morph_end_meters", static_cast<float>(presentation_range.outerRadius));
            tile.terrainMaterial->set_shader_parameter(
                "edge_grid_ratios", edge_grid_ratios);
        }
    }

    update_vegetation_shadows(camera_position);
    presented_minimum_lod_ = minimum_lod;
    presented_lod_revision_ = streamer_ ? streamer_->presentationRevision() : 0U;
    last_lod_update_position_ = {camera_position.x, camera_position.z};
    lod_readiness_dirty_ = false;
}

void TerrainWorld3D::update_vegetation_shadows(const glm::dvec3& camera_position) {
    // Alpha-tested tree cards are relatively cheap in the colour pass but
    // expensive when every visible stand is rasterized into the directional
    // shadow map. Keep real tree shadows around the player and let the terrain
    // material's forest/form shading carry the middle and far distance.
    const auto enable_distance = std::max(
        250.0, static_cast<double>(visuals_.forest.nearShadowDistanceMeters) * 0.86);
    const auto disable_distance = std::max(
        enable_distance + 80.0,
        static_cast<double>(visuals_.forest.nearShadowDistanceMeters));
    const auto minimum_lod = streamer_ ? streamer_->activeMinimumLevel() : 0;
    for (auto& [key, tile] : resident_tiles_) {
        const auto tile_active = key.level >= minimum_lod
            && (streamer_ == nullptr || streamer_->isWanted(key));
        for (auto& vegetation : tile.vegetationNodes) {
            if (!vegetation.shadowEligible || vegetation.node == nullptr) continue;
            if (!tile_active) {
                if (vegetation.castsShadow) {
                    vegetation.castsShadow = false;
                    vegetation.node->set_cast_shadows_setting(
                        godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
                }
                continue;
            }
            const auto absolute_center = tile.absoluteOrigin
                + glm::dvec2{vegetation.localOffset};
            const auto distance = glm::distance(
                absolute_center, glm::dvec2{camera_position.x, camera_position.z});
            const auto threshold = vegetation.castsShadow
                ? disable_distance : enable_distance;
            const auto should_cast = distance <= threshold;
            if (should_cast == vegetation.castsShadow) continue;
            vegetation.castsShadow = should_cast;
            vegetation.node->set_cast_shadows_setting(should_cast
                ? godot::GeometryInstance3D::SHADOW_CASTING_SETTING_ON
                : godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
            vegetation_debug_dirty_ = true;
        }
    }
}

void TerrainWorld3D::update_vegetation_presentation(const double delta) {
    const auto editor_hint = godot::Engine::get_singleton()->is_editor_hint();
    auto* camera = camera_;
    auto* viewport = get_viewport();
    if ((!editor_hint && (camera == nullptr || viewport == nullptr))
        || tree_material_.is_null()) return;

    const auto camera_position = editor_hint && editor_streaming_mode_
        ? godot::Vector3{
            static_cast<godot::real_t>(editor_observer_position_.x),
            static_cast<godot::real_t>(editor_observer_position_.y),
            static_cast<godot::real_t>(editor_observer_position_.z)}
        : camera->get_global_position();
    tree_material_->set_shader_parameter("render_camera_position", camera_position);

    diagnostics_accumulator_ += delta;
    if (diagnostics_accumulator_ < 0.25) return;
    diagnostics_accumulator_ = std::fmod(diagnostics_accumulator_, 0.25);

    std::size_t visible_trees{};
    std::size_t visible_grass{};
    std::size_t shadow_batches{};
    std::size_t visible_terrain_batches{};
    std::size_t visible_vegetation_batches{};
    for (const auto& [unused, tile] : resident_tiles_) {
        static_cast<void>(unused);
        if (tile.node != nullptr && tile.node->is_visible()) ++visible_terrain_batches;
        for (const auto& vegetation : tile.vegetationNodes) {
            if (vegetation.node == nullptr || !vegetation.node->is_visible()) continue;
            const auto node_position = vegetation.node->get_global_position();
            const auto distance = std::max(
                1.0F,
                camera_position.distance_to(node_position) - vegetation.radius);
            if (vegetation.kind == VegetationKind::Grass) {
                if (distance <= 188.0F) {
                    visible_grass += vegetation.instanceCount;
                    ++visible_vegetation_batches;
                }
                continue;
            }

            const auto in_range = vegetation.kind == VegetationKind::NearTrees
                ? distance <= 740.0F
                : distance >= 460.0F && distance <= 3'040.0F;
            if (!in_range) continue;
            visible_trees += vegetation.instanceCount;
            ++visible_vegetation_batches;
            if (vegetation.castsShadow) ++shadow_batches;
        }
    }
    visible_tree_estimate_ = visible_trees;
    visible_grass_estimate_ = visible_grass;
    visible_terrain_batch_count_ = visible_terrain_batches;
    visible_vegetation_batch_count_ = visible_vegetation_batches;
    tree_shadow_batch_count_ = shadow_batches;
    if (debug_view_ == 13 && vegetation_debug_dirty_) {
        rebuild_vegetation_debug_overlay();
    }
    if (!editor_hint) update_hud();
}

void TerrainWorld3D::update_hud() {
    auto* player = player_node_;
    auto* camera = camera_;
    if (player == nullptr || camera == nullptr) return;

    const auto local_position = player->get_global_position();
    const auto camera_position = camera->get_global_position();
    const auto absolute_x = floating_origin_.x + static_cast<double>(local_position.x);
    const auto absolute_z = floating_origin_.y + static_cast<double>(local_position.z);
    const auto camera_absolute_x = floating_origin_.x + static_cast<double>(camera_position.x);
    const auto camera_absolute_z = floating_origin_.y + static_cast<double>(camera_position.z);
    const auto ground = streamer_
        ? streamer_->sampleHeight(camera_absolute_x, camera_absolute_z)
        : std::nullopt;
    // Terrain elevation and the ocean datum use metres above mean sea level.
    // Keep that physical altitude distinct from local ground clearance.
    const auto altitude_asl = static_cast<double>(camera_position.y);
    const auto height_agl = ground
        ? std::optional<double>{altitude_asl - static_cast<double>(*ground)}
        : std::nullopt;
    const auto installed_km = streamer_ ? streamer_->installedRadius() / 1'000.0 : 0.0;
    const auto requested_km = streamer_ ? streamer_->requestedRadius() / 1'000.0 : 0.0;

    if (streamer_) {
        if (auto* controller = godot::Object::cast_to<PlayerController>(player_node_);
            controller != nullptr) {
            const auto camera_direction = -camera->get_global_basis().get_column(2);
            const auto terrain_distance = streamer_->measureRay(
                glm::dvec3{
                    floating_origin_.x + static_cast<double>(camera_position.x),
                    static_cast<double>(camera_position.y),
                    floating_origin_.y + static_cast<double>(camera_position.z),
                },
                glm::dvec3{
                    static_cast<double>(camera_direction.x),
                    static_cast<double>(camera_direction.y),
                    static_cast<double>(camera_direction.z),
                },
                FlatWorldViewDistanceMetres);
            controller->set_terrain_aim_distance(terrain_distance.value_or(0.0));
        }
    }

    if (auto* seed_label = godot::Object::cast_to<godot::Label>(
            get_node_or_null(godot::NodePath{"../../HUD/Seed"})); seed_label != nullptr) {
        seed_label->set_text(godot::String{"SEED "}
            + (streamer_ ? seed_text(streamer_->seed()) : godot::String{"--"}));
    }

    if (auto* physical = godot::Object::cast_to<godot::Label>(
            get_node_or_null(godot::NodePath{"../../HUD/Physical"})); physical != nullptr) {
        const auto height_agl_text = height_agl
            ? godot::String::num(*height_agl, 0)
            : godot::String{"--"};
        physical->set_text(
            godot::String{"HUMAN SCALE | EYE 1.68 M | 1 UNIT = 1 M"}
            + "\nX " + godot::String::num(absolute_x, 0)
            + " M   Y " + godot::String::num(altitude_asl, 0)
            + " M   Z " + godot::String::num(absolute_z, 0)
            + " M   ALT ASL " + godot::String::num(altitude_asl, 0)
            + " M   AGL " + height_agl_text + " M");
    }

    if (auto* diagnostics = godot::Object::cast_to<godot::Label>(
            get_node_or_null(godot::NodePath{"../../HUD/Diagnostics"})); diagnostics != nullptr) {
        godot::String timing{"NET --  DEC --  DER --  VEG -- MS"};
        godot::String memory{"CPU -- + -- MB | RENDER EST -- MB"};
        if (streamer_) {
            const auto& performance = streamer_->performance();
            timing = godot::String{"NET "} + godot::String::num(performance.averageFetchMilliseconds, 1)
                + "  DEC " + godot::String::num(performance.averageDecodeMilliseconds, 1)
                + "  DER " + godot::String::num(performance.averageDerivedMilliseconds, 1)
                + "  VEG " + godot::String::num(performance.averageVegetationMilliseconds, 1) + " MS";
            memory = godot::String{"CPU "}
                + godot::String::num(
                    static_cast<double>(performance.terrainCpuBytes) / (1'024.0 * 1'024.0), 1)
                + " + " + godot::String::num(
                    static_cast<double>(performance.vegetationCpuBytes) / (1'024.0 * 1'024.0), 1)
                + " MB | RENDER EST " + godot::String::num(
                    static_cast<double>(performance.residentRenderBytes) / (1'024.0 * 1'024.0), 1)
                + " MB";
        }
        const auto lod_prefix = streamer_
            ? godot::String{"LOD L"} + godot::String::num_int64(streamer_->activeMinimumLevel()) + " | "
            : godot::String{};
        const auto far_geometry = pending_tile_count_ > 0
            ? lod_prefix + godot::String{"FAR GEOMETRY "} + godot::String::num(installed_km, 0)
                + " > " + godot::String::num(requested_km, 0) + " KM LOADING"
            : lod_prefix + godot::String{"FAR GEOMETRY "}
                + godot::String::num(installed_km, 0) + " KM";
        const auto* engine_performance = godot::Performance::get_singleton();
        const auto process_milliseconds = engine_performance->get_monitor(
            godot::Performance::TIME_PROCESS) * 1'000.0;
        const auto draw_calls = static_cast<std::int64_t>(engine_performance->get_monitor(
            godot::Performance::RENDER_TOTAL_DRAW_CALLS_IN_FRAME));
        const auto primitives_millions = engine_performance->get_monitor(
            godot::Performance::RENDER_TOTAL_PRIMITIVES_IN_FRAME) / 1'000'000.0;
        diagnostics->set_text(
            timing + "\n" + memory + "\n" + far_geometry
            + "\n" + godot::String::num_int64(
                static_cast<std::int64_t>(godot::Engine::get_singleton()->get_frames_per_second()))
            + " FPS  " + godot::String::num(process_milliseconds, 1) + " MS  "
            + godot::String::num_int64(draw_calls) + " DRAWS  "
            + godot::String::num(primitives_millions, 2) + "M PRIM | "
            + godot::String::num_int64(
                static_cast<std::int64_t>(visible_terrain_batch_count_)) + "T "
            + godot::String::num_int64(
                static_cast<std::int64_t>(visible_vegetation_batch_count_)) + "V | "
            + godot::String::num_int64(resident_tile_count_) + " TILES +"
            + godot::String::num_int64(pending_tile_count_)
            + "   ~" + godot::String::num_int64(
                static_cast<std::int64_t>(visible_tree_estimate_)) + " TREES"
            + "   ~" + godot::String::num_int64(
                static_cast<std::int64_t>(visible_grass_estimate_)) + " GRASS");
    }

    if (auto* progress = godot::Object::cast_to<godot::ProgressBar>(
            get_node_or_null(godot::NodePath{"../../HUD/StreamingProgress"})); progress != nullptr) {
        const auto total = std::max<std::int64_t>(resident_tile_count_ + pending_tile_count_, 1);
        progress->set_value(
            static_cast<double>(resident_tile_count_) / static_cast<double>(total) * 100.0);
        progress->set_visible(pending_tile_count_ > 0 || spawn_pending_ || load_started_);
    }

    if (auto* button = godot::Object::cast_to<godot::BaseButton>(
            get_node_or_null(godot::NodePath{"../../HUD/RandomTerrain"})); button != nullptr) {
        button->set_disabled(spawn_pending_ || load_started_ || !streamer_);
    }
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
    update_authored_world_origin();
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
        if (tile.occluderNode != nullptr) {
            tile.occluderNode->set_position(tile.node->get_position());
        }
        tile.node->set_instance_shader_parameter("floating_origin_xz", floating_origin);
        const auto position = godot::Vector3{
            static_cast<godot::real_t>(tile.absoluteOrigin.x - floating_origin_.x),
            0.0F,
            static_cast<godot::real_t>(tile.absoluteOrigin.y - floating_origin_.y),
        };
        for (const auto& vegetation : tile.vegetationNodes) {
            if (vegetation.node == nullptr) continue;
            vegetation.node->set_position(position + godot::Vector3{
                vegetation.localOffset.x, 0.0F, vegetation.localOffset.y});
        }
    }
    if (auto* ocean = ocean_; ocean != nullptr) {
        ocean->set_instance_shader_parameter("floating_origin_xz", floating_origin);
    }
    vegetation_debug_dirty_ = true;
    godot::UtilityFunctions::print(
        "[TerrainWorld3D] Floating origin rebased to (", floating_origin.x, ", ", floating_origin.y,
        "); resident=", static_cast<std::int64_t>(resident_tiles_.size()));
}

void TerrainWorld3D::apply_visual_configuration() {
    apply_material_configuration();
    apply_water_configuration();

    // Editor streaming reuses the authored physical atmosphere as-is. Mutating
    // scene-owned environment resources from a transient preview would make
    // camera-dependent values eligible for accidental serialization.
    if (godot::Engine::get_singleton()->is_editor_hint()) return;

    // PhysicalAtmosphere owns the sun transform, spectrum and energy. Shadow
    // cost remains a terrain renderer policy, so keep its nearby coverage
    // bounded independently from the astronomical day/night controller.
    const auto editor_hint = godot::Engine::get_singleton()->is_editor_hint();
    auto* atmosphere_environment = godot::Object::cast_to<godot::WorldEnvironment>(
        get_node_or_null(editor_hint
            ? godot::NodePath{"../../../PhysicalAtmosphere"}
            : godot::NodePath{"../../PhysicalAtmosphere"}));
    auto* atmosphere_sun = godot::Object::cast_to<godot::DirectionalLight3D>(
        get_node_or_null(editor_hint
            ? godot::NodePath{"../../../PhysicalAtmosphere/Sun"}
            : godot::NodePath{"../../PhysicalAtmosphere/Sun"}));
    const bool physical_atmosphere_active =
        atmosphere_environment != nullptr && atmosphere_sun != nullptr;

    if (atmosphere_environment != nullptr) {
        const auto environment = atmosphere_environment->get_environment();
        if (environment.is_valid()) {
            // A cool, bounded ambient term preserves shadow contrast while
            // low-cost SSAO restores contact at rocks, folds and vegetation.
            environment->set_ambient_source(godot::Environment::AMBIENT_SOURCE_COLOR);
            environment->set_ambient_light_color(
                godot::Color{0.53F, 0.64F, 0.82F, 1.0F});
            environment->set_ambient_light_energy(0.55F);
            environment->set_reflection_source(
                godot::Environment::REFLECTION_SOURCE_DISABLED);
            environment->set_ssao_enabled(true);
            environment->set_ssao_radius(2.2F);
            environment->set_ssao_intensity(1.25F);
            environment->set_ssao_power(1.35F);
            environment->set_ssao_detail(0.55F);
            environment->set_ssao_horizon(0.08F);
            environment->set_ssao_sharpness(0.90F);
            environment->set_ssao_direct_light_affect(0.08F);
            environment->set_ssao_ao_channel_affect(0.85F);
        }
    }

    if (atmosphere_sun != nullptr) {
        atmosphere_sun->set_param(godot::Light3D::PARAM_SHADOW_MAX_DISTANCE, 3'000.0F);
        atmosphere_sun->set_param(godot::Light3D::PARAM_SHADOW_SPLIT_1_OFFSET, 0.30F);
        atmosphere_sun->set_param(godot::Light3D::PARAM_SHADOW_FADE_START, 0.90F);
        atmosphere_sun->set_param(godot::Light3D::PARAM_SHADOW_NORMAL_BIAS, 1.25F);
        atmosphere_sun->set_param(godot::Light3D::PARAM_SHADOW_BIAS, 0.10F);
        atmosphere_sun->set_param(godot::Light3D::PARAM_SHADOW_PANCAKE_SIZE, 40.0F);
        atmosphere_sun->set_shadow_mode(
            godot::DirectionalLight3D::SHADOW_PARALLEL_2_SPLITS);
        atmosphere_sun->set_blend_splits(true);
    }

    if (auto* sun = physical_atmosphere_active
            ? nullptr
            : godot::Object::cast_to<godot::DirectionalLight3D>(
            get_node_or_null(godot::NodePath{"../Sun"})); sun != nullptr) {
        sun->set_rotation_degrees({
            -visuals_.lighting.sunElevationDegrees,
            -visuals_.lighting.sunAzimuthDegrees,
            0.0F,
        });
        sun->set_color(godot::Color{1.0F, 0.945F, 0.855F, 1.0F});
        sun->set_param(godot::Light3D::PARAM_ENERGY, visuals_.lighting.sunIntensity);
        sun->set_param(godot::Light3D::PARAM_INDIRECT_ENERGY, 0.40F);
        sun->set_param(godot::Light3D::PARAM_VOLUMETRIC_FOG_ENERGY, 0.72F);
        sun->set_param(godot::Light3D::PARAM_SPECULAR, 1.0F);
        sun->set_param(godot::Light3D::PARAM_SIZE, 0.53F);
        // Realtime cascades ground the player and nearby vegetation. Macro
        // terrain form replaces long-range realtime shadows.
        sun->set_param(godot::Light3D::PARAM_SHADOW_MAX_DISTANCE, 3'000.0F);
        sun->set_param(godot::Light3D::PARAM_SHADOW_SPLIT_1_OFFSET, 0.30F);
        sun->set_param(godot::Light3D::PARAM_SHADOW_FADE_START, 0.90F);
        sun->set_param(godot::Light3D::PARAM_SHADOW_NORMAL_BIAS, 1.25F);
        sun->set_param(godot::Light3D::PARAM_SHADOW_BIAS, 0.10F);
        sun->set_param(godot::Light3D::PARAM_SHADOW_PANCAKE_SIZE, 40.0F);
        sun->set_param(godot::Light3D::PARAM_SHADOW_OPACITY, 0.92F);
        sun->set_param(godot::Light3D::PARAM_SHADOW_BLUR, 1.0F);
        sun->set_shadow(true);
        sun->set_shadow_mode(godot::DirectionalLight3D::SHADOW_PARALLEL_2_SPLITS);
        sun->set_blend_splits(true);
        sun->set_sky_mode(godot::DirectionalLight3D::SKY_MODE_LIGHT_AND_SKY);
    }

    auto* world_environment = physical_atmosphere_active
        ? nullptr
        : godot::Object::cast_to<godot::WorldEnvironment>(
            get_node_or_null(godot::NodePath{"../WorldEnvironment"}));
    if (world_environment != nullptr) {
        auto environment = world_environment->get_environment();
        if (environment.is_null()) environment.instantiate();

        godot::Ref<godot::PhysicalSkyMaterial> sky_material;
        sky_material.instantiate();
        sky_material->set_rayleigh_coefficient(2.0F);
        sky_material->set_rayleigh_color(godot::Color{0.30F, 0.43F, 0.72F, 1.0F});
        sky_material->set_mie_coefficient(0.005F);
        sky_material->set_mie_eccentricity(0.80F);
        sky_material->set_mie_color(godot::Color{0.72F, 0.68F, 0.60F, 1.0F});
        sky_material->set_turbidity(visuals_.atmosphere.turbidity);
        sky_material->set_sun_disk_scale(1.0F);
        sky_material->set_ground_color(godot::Color{0.115F, 0.135F, 0.10F, 1.0F});
        sky_material->set_energy_multiplier(1.0F);
        sky_material->set_use_debanding(true);

        godot::Ref<godot::Sky> sky;
        sky.instantiate();
        sky->set_material(sky_material);
        sky->set_process_mode(godot::Sky::PROCESS_MODE_INCREMENTAL);
        sky->set_radiance_size(godot::Sky::RADIANCE_SIZE_256);

        environment->set_background(godot::Environment::BG_SKY);
        environment->set_sky(sky);
        environment->set_bg_energy_multiplier(1.0F);
        environment->set_ambient_source(godot::Environment::AMBIENT_SOURCE_COLOR);
        environment->set_ambient_light_energy(visuals_.lighting.skyIntensity);
        environment->set_ambient_light_sky_contribution(1.0F);
        environment->set_reflection_source(godot::Environment::REFLECTION_SOURCE_DISABLED);
        environment->set_tonemapper(godot::Environment::TONE_MAPPER_ACES);
        environment->set_tonemap_exposure(visuals_.lighting.exposure);
        environment->set_tonemap_white(6.0F);
        environment->set_glow_enabled(false);
        environment->set_ssao_enabled(true);
        environment->set_ssil_enabled(false);

        environment->set_fog_enabled(true);
        environment->set_fog_mode(godot::Environment::FOG_MODE_EXPONENTIAL);
        environment->set_fog_light_color(godot::Color{0.67F, 0.76F, 0.86F, 1.0F});
        environment->set_fog_light_energy(0.82F);
        environment->set_fog_sun_scatter(0.22F);
        environment->set_fog_density(visuals_.atmosphere.distanceDensity);
        environment->set_fog_height(visuals_.atmosphere.baseHeightMeters);
        environment->set_fog_height_density(visuals_.atmosphere.heightFalloff);
        environment->set_fog_aerial_perspective(visuals_.atmosphere.aerialPerspectiveStrength);
        environment->set_fog_sky_affect(0.66F);
        environment->set_volumetric_fog_enabled(false);
        world_environment->set_environment(environment);
    }

    update_debug_presentation();
    if (physical_atmosphere_active) {
        godot::UtilityFunctions::print(
            "[TerrainWorld3D] Project physical atmosphere active; terrain and vegetation "
            "use its solar direction, spectral color, aerial perspective, and shadows");
    } else {
        godot::UtilityFunctions::print(
            "[TerrainWorld3D] Built-in visual fallback configured; ACES exposure=",
            visuals_.lighting.exposure,
            ", directional_shadow=2 x 4 km @ 2048, fog_density=",
            visuals_.atmosphere.distanceDensity,
            ", fog_height_falloff=",
            visuals_.atmosphere.heightFalloff);
    }
}

void TerrainWorld3D::apply_material_configuration() {
    const godot::Ref<godot::ShaderMaterial> material = terrain_material_;
    if (material.is_null()) return;
    material->set_shader_parameter("albedo_gain", visuals_.terrain.albedoGain);
    material->set_shader_parameter(
        "macro_texture_scale_meters", visuals_.terrain.macroTextureScaleMeters);
    material->set_shader_parameter(
        "detail_fade_start_meters", visuals_.terrain.detailFadeStartMeters);
    material->set_shader_parameter(
        "detail_fade_end_meters", visuals_.terrain.detailFadeEndMeters);
    material->set_shader_parameter(
        "atmosphere_distance_density", visuals_.atmosphere.distanceDensity);
    material->set_shader_parameter(
        "atmosphere_height_falloff", visuals_.atmosphere.heightFalloff);
    material->set_shader_parameter(
        "atmosphere_base_height_meters", visuals_.atmosphere.baseHeightMeters);
}

void TerrainWorld3D::apply_water_configuration() {
    if (visual_settings_.is_null()) return;
    auto* ocean = ocean_;
    if (ocean == nullptr) return;
    const godot::Ref<godot::ShaderMaterial> material = ocean->get_active_material(0);
    if (material.is_null()) return;

    const auto water = visual_settings_->water();
    material->set_shader_parameter("detail_wavelength_meters", water.detailWavelengthMeters);
    material->set_shader_parameter("wave_time_scale", water.timeScale);
    material->set_shader_parameter("detail_normal_strength", water.detailNormalStrength);
    material->set_shader_parameter(
        "macro_wavelength_meters", water.macroWavelengthMeters);
    material->set_shader_parameter(
        "macro_height_meters", water.macroHeightMeters);
    material->set_shader_parameter("wave_fade_start_meters", water.waveFadeStartMeters);
    material->set_shader_parameter("wave_fade_end_meters", water.waveFadeEndMeters);
}

void TerrainWorld3D::create_vegetation_resources() {
    near_tree_billboard_mesh_ = make_tree_billboard_mesh(2);
    far_tree_billboard_mesh_ = make_tree_billboard_mesh(1);
    grass_clump_mesh_ = make_grass_clump_mesh();

    godot::Ref<godot::Shader> tree_shader = godot::ResourceLoader::get_singleton()->load(
        "res://materials/tree_billboard.gdshader", "Shader");
    godot::Ref<godot::Shader> grass_shader = godot::ResourceLoader::get_singleton()->load(
        "res://materials/grass.gdshader", "Shader");
    if (tree_shader.is_null() || grass_shader.is_null()) {
        godot::UtilityFunctions::push_warning(
            "[TerrainWorld3D] Vegetation shaders unavailable; terrain will continue without geometry");
        return;
    }

    const godot::Ref<godot::Texture2D> texture =
        godot::ResourceLoader::get_singleton()->load(
            "res://assets/vegetation/tree_atlas.webp", "Texture2D");
    if (texture.is_null()) {
        godot::UtilityFunctions::push_warning(
            "[TerrainWorld3D] Imported tree atlas is unavailable; vegetation disabled");
        near_tree_billboard_mesh_.unref();
        far_tree_billboard_mesh_.unref();
        grass_clump_mesh_.unref();
        return;
    }

    godot::Ref<godot::ShaderMaterial> tree_material;
    tree_material.instantiate();
    tree_material->set_shader(tree_shader);
    tree_material->set_shader_parameter("tree_atlas", texture);
    tree_material->set_shader_parameter(
        "size_multiplier", visuals_.forest.billboardSizeMultiplier);
    tree_material->set_shader_parameter(
        "near_pixel_threshold", visuals_.forest.nearPixelThreshold);
    tree_material->set_shader_parameter(
        "mid_pixel_threshold", visuals_.forest.midPixelThreshold);
    tree_material->set_shader_parameter(
        "geometry_cull_pixel_threshold", visuals_.forest.geometryCullPixelThreshold);
    tree_material_ = tree_material;

    godot::Ref<godot::ShaderMaterial> grass_material;
    grass_material.instantiate();
    grass_material->set_shader(grass_shader);
    grass_material_ = grass_material;
    godot::UtilityFunctions::print(
        "[TerrainWorld3D] Vegetation resources ready; atlas=2560x1024, tree_cards=2, grass_blades=6");
}

void TerrainWorld3D::update_debug_presentation() {
    const godot::Ref<godot::ShaderMaterial> material = terrain_material_;
    if (material.is_valid()) {
        material->set_shader_parameter("debug_view", debug_view_ <= 12 ? debug_view_ : 0);
    }
    for (auto& [unused, tile] : resident_tiles_) {
        static_cast<void>(unused);
        if (tile.terrainMaterial.is_valid()) {
            tile.terrainMaterial->set_shader_parameter(
                "debug_view", debug_view_ <= 12 ? debug_view_ : 0);
        }
    }
    if (auto* viewport = get_viewport(); viewport != nullptr) {
        // The project uses one orthogonal near shadow range, not PSSM.
        // Godot's PSSM diagnostic therefore produced an unstable black/white
        // edge image that changed with the camera and did not describe the
        // renderer. Keep engine debug draw disabled and visualize the actual
        // 1.5 km coverage in the terrain material instead.
        viewport->set_debug_draw(wireframe_enabled_
            ? godot::Viewport::DEBUG_DRAW_WIREFRAME
            : godot::Viewport::DEBUG_DRAW_DISABLED);
    }
    if (tree_material_.is_valid()) {
        tree_material_->set_shader_parameter("debug_lod", debug_view_ == 13);
    }
    if (debug_view_ == 13) {
        vegetation_debug_dirty_ = true;
        rebuild_vegetation_debug_overlay();
    } else if (vegetation_debug_node_ != nullptr) {
        vegetation_debug_node_->set_visible(false);
    }
    if (auto* label = godot::Object::cast_to<godot::Label>(
            get_node_or_null(godot::NodePath{"../../HUD/DebugView"})); label != nullptr) {
        label->set_visible(debug_view_ != 0);
        label->set_text(godot::String{"DEBUG | "}
            + debug_view_names[static_cast<std::size_t>(debug_view_)]
            + (debug_view_ == 12
                ? godot::String{"\nGREEN REALTIME SHADOW | BLUE MACRO LIGHTING"}
                : debug_view_ == 13
                ? godot::String{"\nGREEN NEAR | BLUE FAR | YELLOW GRASS | ORANGE SHADOW"}
                : godot::String{}));
    }
}

void TerrainWorld3D::rebuild_vegetation_debug_overlay() {
    if (!is_inside_tree() || godot::Engine::get_singleton()->is_editor_hint()) return;
    if (vegetation_debug_node_ == nullptr) {
        vegetation_debug_node_ = memnew(godot::MeshInstance3D);
        vegetation_debug_node_->set_name("VegetationDebugBounds");
        vegetation_debug_node_->set_cast_shadows_setting(
            godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
        add_child(vegetation_debug_node_);
    }
    vegetation_debug_node_->set_visible(debug_view_ == 13);
    if (debug_view_ != 13) return;

    godot::Ref<godot::StandardMaterial3D> material;
    material.instantiate();
    material->set_shading_mode(godot::BaseMaterial3D::SHADING_MODE_UNSHADED);
    material->set_flag(godot::BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
    material->set_flag(godot::BaseMaterial3D::FLAG_DISABLE_FOG, true);

    godot::Ref<godot::ImmediateMesh> mesh;
    mesh.instantiate();
    mesh->surface_begin(godot::Mesh::PRIMITIVE_LINES, material);
    const auto add_line = [&mesh](
        const godot::Vector3& start,
        const godot::Vector3& end,
        const godot::Color& color) {
        mesh->surface_set_color(color);
        mesh->surface_add_vertex(start);
        mesh->surface_set_color(color);
        mesh->surface_add_vertex(end);
    };

    for (const auto& [unused, tile] : resident_tiles_) {
        static_cast<void>(unused);
        for (const auto& vegetation : tile.vegetationNodes) {
            if (vegetation.node == nullptr) continue;
            const auto bounds = vegetation.node->get_aabb();
            if (!bounds.has_surface()) continue;
            const auto origin = vegetation.node->get_position() + bounds.position;
            const auto y = origin.y + 0.35F;
            const auto min_x = origin.x;
            const auto max_x = origin.x + bounds.size.x;
            const auto min_z = origin.z;
            const auto max_z = origin.z + bounds.size.z;
            const godot::Color color = vegetation.castsShadow
                ? godot::Color{1.0F, 0.28F, 0.05F, 1.0F}
                : vegetation.kind == VegetationKind::NearTrees
                    ? godot::Color{0.10F, 1.0F, 0.26F, 1.0F}
                    : vegetation.kind == VegetationKind::FarTrees
                        ? godot::Color{0.12F, 0.52F, 1.0F, 1.0F}
                        : godot::Color{1.0F, 0.82F, 0.08F, 1.0F};
            const godot::Vector3 a{min_x, y, min_z};
            const godot::Vector3 b{max_x, y, min_z};
            const godot::Vector3 c{max_x, y, max_z};
            const godot::Vector3 d{min_x, y, max_z};
            add_line(a, b, color);
            add_line(b, c, color);
            add_line(c, d, color);
            add_line(d, a, color);
            const auto center = (a + c) * 0.5F;
            add_line(center, center + godot::Vector3{0.0F, 4.0F, 0.0F}, color);
        }
    }
    mesh->surface_end();
    vegetation_debug_node_->set_mesh(mesh);
    vegetation_debug_dirty_ = false;
}

void TerrainWorld3D::set_status(const godot::String& value) {
    if (status_ == value) return;
    status_ = value;
    update_status_label();
    emit_signal("streaming_state_changed", status_);
}

void TerrainWorld3D::update_status_label() const {
    const auto ready = status_ == "terrain_ready";
    if (auto* panel = godot::Object::cast_to<godot::Control>(
            get_node_or_null(godot::NodePath{"../../HUD/StatusPanel"})); panel != nullptr) {
        panel->set_visible(!ready);
    }
    if (auto* heading = godot::Object::cast_to<godot::Label>(
            get_node_or_null(godot::NodePath{"../../HUD/StatusHeading"})); heading != nullptr) {
        heading->set_visible(!ready);
        heading->set_text(status_ == "terrain_error" ? "TERRAIN ERROR" : "GENERATING TERRAIN");
    }
    if (auto* label = godot::Object::cast_to<godot::Label>(
            get_node_or_null(godot::NodePath{"../../HUD/Status"})); label != nullptr) {
        label->set_visible(!ready);
        label->set_text(status_.replace("_", " ").to_upper());
    }
}

} // namespace terrain::godot_adapter
