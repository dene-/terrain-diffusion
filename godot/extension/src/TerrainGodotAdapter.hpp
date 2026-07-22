#pragma once

#include <terrain/TerrainTypes.hpp>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/array_occluder3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>

#include <cstdint>
#include <span>

namespace terrain::godot_adapter {

// These conversion functions run on Godot's main thread after a terrain-core
// ready event is drained. They populate packed engine arrays directly and do
// not create a Godot Object or Variant for every vertex/instance.
[[nodiscard]] godot::Ref<godot::ArrayMesh> make_array_mesh(const TileMesh& tile);

// GPU heightmap terrain uses one immutable unit grid per segment count. Tile
// elevation and classification data live in compact textures and are sampled
// by the vertex/fragment shader instead of duplicating full vertex buffers.
[[nodiscard]] godot::Ref<godot::ArrayMesh> make_heightmap_grid_mesh(std::int32_t segments);

[[nodiscard]] godot::Ref<godot::ImageTexture> make_heightmap_texture(const RawTile& tile);

[[nodiscard]] godot::Ref<godot::ImageTexture> make_terrain_field_texture(
    const TileMesh& tile,
    bool biome_fields);

[[nodiscard]] godot::Ref<godot::ArrayOccluder3D> make_terrain_occluder(
    const TileMesh& tile);

[[nodiscard]] godot::Ref<godot::ConcavePolygonShape3D> make_terrain_collision_shape(
    const TileMesh& tile);

[[nodiscard]] godot::Ref<godot::ArrayMesh> make_tree_billboard_mesh(std::int32_t card_count);

[[nodiscard]] godot::Ref<godot::ArrayMesh> make_grass_clump_mesh();

[[nodiscard]] godot::Ref<godot::MultiMesh> make_vegetation_multimesh(
    std::span<const VegetationInstance> instances,
    const godot::Ref<godot::Mesh>& billboard_mesh,
    glm::vec2 local_origin = {});

} // namespace terrain::godot_adapter
