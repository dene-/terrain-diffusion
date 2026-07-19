#include "TerrainGodotAdapter.hpp"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace terrain::godot_adapter {
namespace {

constexpr float ByteToUnit = 1.0F / 255.0F;
constexpr std::size_t Transform3DFloats = 12U;
constexpr std::size_t ColorFloats = 4U;
constexpr std::size_t CustomDataFloats = 4U;
constexpr std::size_t VegetationStrideFloats =
    Transform3DFloats + ColorFloats + CustomDataFloats;

[[nodiscard]] godot::Vector3 to_godot(const glm::vec3& value) noexcept {
    return {value.x, value.y, value.z};
}

[[nodiscard]] godot::Color unpack_color(const std::uint32_t packed) noexcept {
    return {
        static_cast<float>(packed & 0xffU) * ByteToUnit,
        static_cast<float>((packed >> 8U) & 0xffU) * ByteToUnit,
        static_cast<float>((packed >> 16U) & 0xffU) * ByteToUnit,
        static_cast<float>((packed >> 24U) & 0xffU) * ByteToUnit,
    };
}

void unpack_rgba8(const std::uint32_t packed, std::uint8_t* destination) noexcept {
    destination[0] = static_cast<std::uint8_t>(packed & 0xffU);
    destination[1] = static_cast<std::uint8_t>((packed >> 8U) & 0xffU);
    destination[2] = static_cast<std::uint8_t>((packed >> 16U) & 0xffU);
    destination[3] = static_cast<std::uint8_t>((packed >> 24U) & 0xffU);
}

[[nodiscard]] bool fits_godot_array(const std::size_t size) noexcept {
    return size <= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
}

} // namespace

godot::Ref<godot::ArrayMesh> make_array_mesh(const TileMesh& tile) {
    if (tile.vertices.empty() || tile.indices.empty()
        || !fits_godot_array(tile.vertices.size()) || !fits_godot_array(tile.indices.size())) {
        return {};
    }

    const auto vertex_count = static_cast<std::int32_t>(tile.vertices.size());
    const auto index_count = static_cast<std::int32_t>(tile.indices.size());

    godot::PackedVector3Array positions;
    godot::PackedVector3Array normals;
    godot::PackedColorArray colors;
    godot::PackedByteArray derived;
    godot::PackedByteArray biome;
    godot::PackedInt32Array indices;
    positions.resize(vertex_count);
    normals.resize(vertex_count);
    colors.resize(vertex_count);
    derived.resize(static_cast<std::int64_t>(tile.vertices.size() * 4U));
    biome.resize(static_cast<std::int64_t>(tile.vertices.size() * 4U));
    indices.resize(index_count);

    auto* position_data = positions.ptrw();
    auto* normal_data = normals.ptrw();
    auto* color_data = colors.ptrw();
    auto* derived_data = derived.ptrw();
    auto* biome_data = biome.ptrw();
    for (std::size_t index = 0; index < tile.vertices.size(); ++index) {
        const auto& vertex = tile.vertices[index];
        position_data[index] = to_godot(vertex.position);
        normal_data[index] = to_godot(vertex.normal);
        color_data[index] = unpack_color(vertex.color);
        unpack_rgba8(vertex.derived, derived_data + index * 4U);
        unpack_rgba8(vertex.biome, biome_data + index * 4U);
    }

    auto* index_data = indices.ptrw();
    std::transform(tile.indices.begin(), tile.indices.end(), index_data,
        [](const std::uint32_t index) { return static_cast<std::int32_t>(index); });

    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = positions;
    arrays[godot::Mesh::ARRAY_NORMAL] = normals;
    arrays[godot::Mesh::ARRAY_COLOR] = colors;
    arrays[godot::Mesh::ARRAY_CUSTOM0] = derived;
    arrays[godot::Mesh::ARRAY_CUSTOM1] = biome;
    arrays[godot::Mesh::ARRAY_INDEX] = indices;

    godot::Ref<godot::ArrayMesh> mesh;
    mesh.instantiate();
    mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays);
    return mesh;
}

godot::Ref<godot::MultiMesh> make_vegetation_multimesh(
    const std::span<const VegetationInstance> instances,
    const godot::Ref<godot::Mesh>& billboard_mesh) {
    if (instances.empty() || billboard_mesh.is_null() || !fits_godot_array(instances.size())) {
        return {};
    }

    godot::Ref<godot::MultiMesh> multimesh;
    multimesh.instantiate();
    multimesh->set_transform_format(godot::MultiMesh::TRANSFORM_3D);
    multimesh->set_use_colors(true);
    multimesh->set_use_custom_data(true);
    multimesh->set_mesh(billboard_mesh);
    multimesh->set_instance_count(static_cast<std::int32_t>(instances.size()));
    multimesh->set_visible_instance_count(static_cast<std::int32_t>(instances.size()));

    godot::PackedFloat32Array buffer;
    buffer.resize(static_cast<std::int64_t>(instances.size() * VegetationStrideFloats));
    auto* data = buffer.ptrw();
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const auto& instance = instances[index];
        auto* item = data + index * VegetationStrideFloats;

        // Transform3D uses the row-major order required by MultiMesh::buffer.
        item[0] = 1.0F; item[1] = 0.0F; item[2] = 0.0F; item[3] = instance.position.x;
        item[4] = 0.0F; item[5] = 1.0F; item[6] = 0.0F; item[7] = instance.position.y;
        item[8] = 0.0F; item[9] = 0.0F; item[10] = 1.0F; item[11] = instance.position.z;

        const auto color = unpack_color(instance.color);
        item[12] = color.r;
        item[13] = color.g;
        item[14] = color.b;
        item[15] = color.a;

        // INSTANCE_CUSTOM: species/type, wind phase, physical scale, reserved.
        item[16] = instance.parameters.x;
        item[17] = instance.parameters.y;
        item[18] = instance.scale;
        item[19] = 0.0F;
    }
    multimesh->set_buffer(buffer);
    return multimesh;
}

} // namespace terrain::godot_adapter
