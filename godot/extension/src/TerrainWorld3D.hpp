#pragma once

#include "WorldVisualSettings.hpp"

#include <terrain/TerrainClient.hpp>
#include <terrain/TerrainStreamer.hpp>
#include <terrain/TerrainTypes.hpp>
#include <terrain/VisualConfig.hpp>

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace godot {
class Camera3D;
class ArrayMesh;
class InputEvent;
class MeshInstance3D;
class MultiMeshInstance3D;
class Node;
class Node3D;
class OccluderInstance3D;
class ShaderMaterial;
class Shader;
class StaticBody3D;
}

namespace terrain::godot_adapter {

class TerrainWorld3D final : public godot::Node3D {
    GDCLASS(TerrainWorld3D, godot::Node3D)

public:
    TerrainWorld3D() = default;
    ~TerrainWorld3D() override;

    void _ready() override;
    void _process(double delta) override;
    void _unhandled_input(const godot::Ref<godot::InputEvent>& event) override;

    void set_server_url(const godot::String& value);
    [[nodiscard]] godot::String get_server_url() const;
    void set_map_path(const godot::String& value);
    [[nodiscard]] godot::String get_map_path() const;

    void set_streaming_enabled(bool value);
    [[nodiscard]] bool is_streaming_enabled() const noexcept;

    void set_editor_streaming_mode(bool value);
    [[nodiscard]] bool is_editor_streaming_mode() const noexcept;
    void set_editor_observer(
        const godot::Vector3& position,
        const godot::Vector3& view_direction,
        double vertical_fov_degrees,
        double viewport_height);

    void set_terrain_material(const godot::Ref<godot::Material>& value);
    [[nodiscard]] godot::Ref<godot::Material> get_terrain_material() const;

    void set_visual_settings(const godot::Ref<WorldVisualSettings>& value);
    [[nodiscard]] godot::Ref<WorldVisualSettings> get_visual_settings() const;
    void apply_visual_settings(bool rebuild_derived = false);

    void set_debug_view(std::int32_t value);
    [[nodiscard]] std::int32_t get_debug_view() const noexcept;
    void apply_camera_bookmark(std::int32_t bookmark);
    void request_random_world();

    [[nodiscard]] godot::String get_status() const;
    [[nodiscard]] std::int64_t get_resident_tile_count() const noexcept;
    [[nodiscard]] std::int64_t get_pending_tile_count() const noexcept;

protected:
    static void _bind_methods();

private:
    enum class VegetationKind : std::uint8_t {
        NearTrees,
        FarTrees,
        Grass,
    };

    struct VegetationNode final {
        godot::MultiMeshInstance3D* node{};
        glm::vec2 localOffset{};
        float radius{};
        std::size_t instanceCount{};
        VegetationKind kind{VegetationKind::Grass};
        bool shadowEligible{};
        bool castsShadow{};
    };

    struct LoadResult final {
        WorldInfo world;
        TileFetchTiming timing;
        TileMeshPtr tile;
        std::unique_ptr<TerrainStreamer> streamer;
        std::int32_t spawnSearchAttempts{};
        std::string error;
    };

    struct ResidentTile final {
        godot::MeshInstance3D* node{};
        godot::OccluderInstance3D* occluderNode{};
        godot::StaticBody3D* collisionBody{};
        godot::Ref<godot::ConcavePolygonShape3D> collisionShape;
        godot::Ref<godot::ShaderMaterial> terrainMaterial;
        std::vector<VegetationNode> vegetationNodes;
        LodSpec spec;
        glm::dvec2 absoluteOrigin{};
        std::size_t treeCount{};
        std::size_t grassCount{};
        std::size_t estimatedRenderBytes{};
    };

    struct PendingVegetationInstall final {
        TileMeshPtr tile;
        std::uint8_t auxiliaryPhase{};
        std::size_t treeRangeIndex{};
        std::size_t grassRowBegin{};
    };

    void start_initial_load();
    void install_initial_tile(LoadResult result);
    [[nodiscard]] godot::MeshInstance3D* add_tile(
        const TileMeshPtr& tile,
        bool install_vegetation = true,
        bool install_auxiliary = true);
    void add_vegetation(const TileMeshPtr& tile, ResidentTile& resident);
    [[nodiscard]] bool advance_vegetation_install(PendingVegetationInstall& install);
    void update_pending_vegetation_installs();
    void remove_tile(const TileKey& key);
    void update_streaming();
    void update_collision_neighborhood(const glm::dvec3& camera_position);
    void update_lod_readiness(const glm::dvec3& camera_position);
    void update_vegetation_shadows(const glm::dvec3& camera_position);
    void update_flat_world_anchor(const glm::dvec2& camera_absolute_xz);
    void update_authored_world_origin();
    void update_flat_world_presentation(
        double viewer_altitude_meters,
        double camera_far_meters,
        double viewer_height_agl_meters,
        double viewer_height_signed_meters);
    void update_vegetation_presentation(double delta);
    void update_spawn_search(godot::Node3D& player, const glm::dvec3& absolute_position);
    void relocate_spawn_search(godot::Node3D& player, const glm::dvec3& absolute_position);
    void set_player_absolute_position(
        godot::Node3D& player,
        const glm::dvec3& absolute_position);
    void clear_resident_tiles();
    void rebase_if_needed(godot::Node3D& player);
    void apply_visual_configuration();
    void apply_material_configuration();
    void apply_water_configuration();
    void on_visual_settings_changed();
    void create_vegetation_resources();
    void update_debug_presentation();
    void rebuild_vegetation_debug_overlay();
    void update_hud();
    void set_status(const godot::String& value);
    void update_status_label() const;

    godot::String server_url_{"http://127.0.0.1:8000"};
    godot::String map_path_;
    godot::String status_{"scaffold_ready"};
    godot::Ref<godot::Material> terrain_material_;
    godot::Ref<godot::Shader> heightmap_terrain_shader_;
    godot::Ref<WorldVisualSettings> visual_settings_;
    godot::Ref<godot::ShaderMaterial> tree_material_;
    godot::Ref<godot::ShaderMaterial> grass_material_;
    godot::Ref<godot::Mesh> near_tree_billboard_mesh_;
    godot::Ref<godot::Mesh> far_tree_billboard_mesh_;
    godot::Ref<godot::Mesh> grass_clump_mesh_;
    godot::Node3D* player_node_{};
    godot::Camera3D* camera_{};
    godot::Node3D* terrain_tiles_root_{};
    godot::Node3D* vegetation_root_{};
    godot::MeshInstance3D* ocean_{};
    godot::Node3D* authored_world_root_{};
    godot::Node* atmosphere_{};
    godot::MeshInstance3D* vegetation_debug_node_{};
    WorldVisualConfig visuals_;
    TileMeshPtr loaded_tile_;
    std::unique_ptr<TerrainStreamer> streamer_;
    std::unordered_map<TileKey, ResidentTile, TileKeyHash> resident_tiles_;
    std::unordered_map<std::int32_t, godot::Ref<godot::ArrayMesh>> heightmap_grid_meshes_;
    std::deque<PendingVegetationInstall> pending_vegetation_installs_;
    std::jthread load_thread_;
    mutable std::mutex result_mutex_;
    std::optional<LoadResult> completed_load_;
    std::int64_t resident_tile_count_{};
    std::int64_t pending_tile_count_{};
    std::int32_t debug_view_{};
    std::int32_t pending_camera_bookmark_{};
    std::int32_t presented_minimum_lod_{-1};
    std::uint64_t presented_lod_revision_{};
    glm::dvec2 floating_origin_{};
    glm::dvec2 last_lod_update_position_{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
    };
    bool streaming_enabled_{true};
    bool editor_streaming_mode_{};
    glm::dvec3 editor_observer_position_{0.0, 2'000.0, 0.0};
    glm::dvec3 editor_observer_direction_{0.0, -0.25, -1.0};
    double editor_observer_vertical_fov_radians_{1.0471975511965976};
    double editor_observer_viewport_height_{900.0};
    double editor_streaming_accumulator_seconds_{};
    bool wireframe_enabled_{};
    bool load_started_{};
    bool lod_readiness_dirty_{true};
    bool vegetation_debug_dirty_{true};
    bool spawn_pending_{};
    std::uint32_t spawn_search_attempts_{};
    double diagnostics_accumulator_{};
    double last_ocean_extent_scale_{-1.0};
    std::size_t visible_tree_estimate_{};
    std::size_t visible_grass_estimate_{};
    std::size_t visible_terrain_batch_count_{};
    std::size_t visible_vegetation_batch_count_{};
    std::size_t tree_shadow_batch_count_{};
};

} // namespace terrain::godot_adapter
