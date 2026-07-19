#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

namespace terrain::godot_adapter {

class TerrainWorld3D final : public godot::Node3D {
    GDCLASS(TerrainWorld3D, godot::Node3D)

public:
    TerrainWorld3D() = default;
    ~TerrainWorld3D() override = default;

    void _ready() override;

    void set_server_url(const godot::String& value);
    [[nodiscard]] godot::String get_server_url() const;

    void set_streaming_enabled(bool value);
    [[nodiscard]] bool is_streaming_enabled() const noexcept;

    [[nodiscard]] godot::String get_status() const;
    [[nodiscard]] std::int64_t get_resident_tile_count() const noexcept;
    [[nodiscard]] std::int64_t get_pending_tile_count() const noexcept;

protected:
    static void _bind_methods();

private:
    godot::String server_url_{"http://127.0.0.1:8000"};
    godot::String status_{"scaffold_ready"};
    std::int64_t resident_tile_count_{};
    std::int64_t pending_tile_count_{};
    bool streaming_enabled_{true};
};

} // namespace terrain::godot_adapter

