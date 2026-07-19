#pragma once

#include <terrain/TerrainTypes.hpp>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>

#include <span>

namespace terrain::godot_adapter {

// These conversion functions run on Godot's main thread after a terrain-core
// ready event is drained. They populate packed engine arrays directly and do
// not create a Godot Object or Variant for every vertex/instance.
[[nodiscard]] godot::Ref<godot::ArrayMesh> make_array_mesh(const TileMesh& tile);

[[nodiscard]] godot::Ref<godot::MultiMesh> make_vegetation_multimesh(
    std::span<const VegetationInstance> instances,
    const godot::Ref<godot::Mesh>& billboard_mesh);

} // namespace terrain::godot_adapter
