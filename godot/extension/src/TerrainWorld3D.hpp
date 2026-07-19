#pragma once

#include <terrain/TerrainClient.hpp>
#include <terrain/TerrainTypes.hpp>
#include <terrain/VisualConfig.hpp>

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace terrain::godot_adapter {

class TerrainWorld3D final : public godot::Node3D {
    GDCLASS(TerrainWorld3D, godot::Node3D)

public:
    TerrainWorld3D() = default;
    ~TerrainWorld3D() override;

    void _ready() override;
    void _process(double delta) override;

    void set_server_url(const godot::String& value);
    [[nodiscard]] godot::String get_server_url() const;

    void set_streaming_enabled(bool value);
    [[nodiscard]] bool is_streaming_enabled() const noexcept;

    void set_terrain_material(const godot::Ref<godot::Material>& value);
    [[nodiscard]] godot::Ref<godot::Material> get_terrain_material() const;

    [[nodiscard]] godot::String get_status() const;
    [[nodiscard]] std::int64_t get_resident_tile_count() const noexcept;
    [[nodiscard]] std::int64_t get_pending_tile_count() const noexcept;

protected:
    static void _bind_methods();

private:
    struct LoadResult final {
        WorldInfo world;
        TileFetchTiming timing;
        TileMeshPtr tile;
        std::int32_t spawnSearchAttempts{};
        std::string error;
    };

    void start_initial_load();
    void install_initial_tile(LoadResult result);
    void set_status(const godot::String& value);
    void update_status_label() const;

    godot::String server_url_{"http://127.0.0.1:8000"};
    godot::String status_{"scaffold_ready"};
    godot::Ref<godot::Material> terrain_material_;
    WorldVisualConfig visuals_;
    TileMeshPtr loaded_tile_;
    std::jthread load_thread_;
    mutable std::mutex result_mutex_;
    std::optional<LoadResult> completed_load_;
    std::int64_t resident_tile_count_{};
    std::int64_t pending_tile_count_{};
    bool streaming_enabled_{true};
    bool load_started_{};
};

} // namespace terrain::godot_adapter
