#include "TerrainGodotAdapter.hpp"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

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

godot::Ref<godot::ArrayMesh> make_heightmap_grid_mesh(const std::int32_t segments) {
    if (segments < 2 || segments > 512) return {};
    const auto width = segments + 1;
    const auto vertex_count = width * width;
    const auto index_count = segments * segments * 6;

    godot::PackedVector3Array positions;
    godot::PackedVector3Array normals;
    godot::PackedVector2Array uvs;
    godot::PackedInt32Array indices;
    positions.resize(vertex_count);
    normals.resize(vertex_count);
    uvs.resize(vertex_count);
    indices.resize(index_count);
    auto* position_data = positions.ptrw();
    auto* normal_data = normals.ptrw();
    auto* uv_data = uvs.ptrw();
    auto* index_data = indices.ptrw();

    const auto inverse_segments = 1.0F / static_cast<float>(segments);
    for (std::int32_t z = 0; z < width; ++z) {
        for (std::int32_t x = 0; x < width; ++x) {
            const auto index = z * width + x;
            const godot::Vector2 uv{
                static_cast<float>(x) * inverse_segments,
                static_cast<float>(z) * inverse_segments,
            };
            position_data[index] = {uv.x, 0.0F, uv.y};
            normal_data[index] = {0.0F, 1.0F, 0.0F};
            uv_data[index] = uv;
        }
    }
    std::int32_t output{};
    for (std::int32_t z = 0; z < segments; ++z) {
        for (std::int32_t x = 0; x < segments; ++x) {
            const auto a = z * width + x;
            const auto b = a + 1;
            const auto c = a + width;
            const auto d = c + 1;
            index_data[output++] = a;
            index_data[output++] = b;
            index_data[output++] = c;
            index_data[output++] = b;
            index_data[output++] = d;
            index_data[output++] = c;
        }
    }
    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = positions;
    arrays[godot::Mesh::ARRAY_NORMAL] = normals;
    arrays[godot::Mesh::ARRAY_TEX_UV] = uvs;
    arrays[godot::Mesh::ARRAY_INDEX] = indices;
    godot::Ref<godot::ArrayMesh> mesh;
    mesh.instantiate();
    mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays);
    return mesh;
}

godot::Ref<godot::ImageTexture> make_heightmap_texture(const RawTile& tile) {
    const auto count = static_cast<std::size_t>(tile.width) * static_cast<std::size_t>(tile.height);
    if (tile.width < 2 || tile.height < 2 || tile.elevations.size() != count) return {};
    godot::PackedFloat32Array samples;
    samples.resize(static_cast<std::int64_t>(count));
    auto* output = samples.ptrw();
    const auto has_render_elevations = tile.renderElevations.size() == count;
    const auto source_at = [&tile, has_render_elevations](std::int32_t x, std::int32_t z) {
        x = std::clamp(x, 0, tile.width - 1);
        z = std::clamp(z, 0, tile.height - 1);
        const auto index = static_cast<std::size_t>(z * tile.width + x);
        return has_render_elevations
            ? tile.renderElevations[index]
            : static_cast<float>(tile.elevations[index]);
    };
    for (std::int32_t z = 0; z < tile.height; ++z) {
        for (std::int32_t x = 0; x < tile.width; ++x) {
            const auto index = static_cast<std::size_t>(z * tile.width + x);
            // TerrainClient already supplies the canonical cubic-reconstructed
            // render field. A second filter here used a different physical
            // footprint at every LOD and made shared vertices disagree by
            // metres, exposing the cross-LOD seams as black lines.
            output[index] = source_at(x, z);
        }
    }
    const auto image = godot::Image::create_from_data(
        tile.width, tile.height, false, godot::Image::FORMAT_RF, samples.to_byte_array());
    return image.is_valid() ? godot::ImageTexture::create_from_image(image)
                            : godot::Ref<godot::ImageTexture>{};
}

godot::Ref<godot::ImageTexture> make_terrain_field_texture(
    const TileMesh& tile,
    const bool biome_fields) {
    const auto width = tile.raw.width - tile.raw.borderSamples * 2;
    const auto height = tile.raw.height - tile.raw.borderSamples * 2;
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width < 2 || height < 2 || tile.vertices.size() < count) return {};
    godot::PackedByteArray bytes;
    bytes.resize(static_cast<std::int64_t>(count * 4U));
    auto* output = bytes.ptrw();
    for (std::size_t index = 0; index < count; ++index) {
        unpack_rgba8(biome_fields ? tile.vertices[index].biome : tile.vertices[index].derived,
            output + index * 4U);
    }
    const auto image = godot::Image::create_from_data(
        width, height, false, godot::Image::FORMAT_RGBA8, bytes);
    return image.is_valid() ? godot::ImageTexture::create_from_image(image)
                            : godot::Ref<godot::ImageTexture>{};
}

godot::Ref<godot::ArrayMesh> make_array_mesh(const TileMesh& tile) {
    if (tile.vertices.empty() || tile.indices.empty()
        || tile.indices.size() % 3U != 0U
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
    for (std::size_t index = 0; index < tile.indices.size(); index += 3U) {
        // The shared native mesh is counter-clockwise when viewed from above.
        // Godot defines clockwise triangle winding as front-facing, so reverse
        // each triangle at the adapter boundary instead of mutating core data.
        index_data[index] = static_cast<std::int32_t>(tile.indices[index]);
        index_data[index + 1U] = static_cast<std::int32_t>(tile.indices[index + 2U]);
        index_data[index + 2U] = static_cast<std::int32_t>(tile.indices[index + 1U]);
    }

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

godot::Ref<godot::ArrayOccluder3D> make_terrain_occluder(const TileMesh& tile) {
    // Occlusion is deliberately much coarser than either the visible mesh or
    // its shadow mesh. Large terrain forms, not individual ridges, are what
    // should reject whole terrain/vegetation batches behind them.
    constexpr std::int32_t sample_stride = 8;
    const auto width = tile.raw.width - tile.raw.borderSamples * 2;
    const auto height = tile.raw.height - tile.raw.borderSamples * 2;
    if (width < 3 || height < 3) return {};
    const auto surface_vertex_count = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height);
    if (surface_vertex_count > tile.vertices.size()) return {};

    std::vector<std::int32_t> source_x;
    std::vector<std::int32_t> source_z;
    for (std::int32_t x = 0; x < width; x += sample_stride) source_x.push_back(x);
    for (std::int32_t z = 0; z < height; z += sample_stride) source_z.push_back(z);
    if (source_x.back() != width - 1) source_x.push_back(width - 1);
    if (source_z.back() != height - 1) source_z.push_back(height - 1);

    const auto occluder_width = static_cast<std::int32_t>(source_x.size());
    const auto occluder_height = static_cast<std::int32_t>(source_z.size());
    godot::PackedVector3Array vertices;
    godot::PackedInt32Array indices;
    vertices.resize(occluder_width * occluder_height);
    indices.resize((occluder_width - 1) * (occluder_height - 1) * 6);
    auto* vertex_data = vertices.ptrw();
    for (std::int32_t z = 0; z < occluder_height; ++z) {
        for (std::int32_t x = 0; x < occluder_width; ++x) {
            const auto source_index = static_cast<std::size_t>(
                source_z[static_cast<std::size_t>(z)] * width
                + source_x[static_cast<std::size_t>(x)]);
            vertex_data[z * occluder_width + x] = to_godot(
                tile.vertices[source_index].position);
        }
    }

    auto* index_data = indices.ptrw();
    std::int32_t output_index{};
    for (std::int32_t z = 0; z < occluder_height - 1; ++z) {
        for (std::int32_t x = 0; x < occluder_width - 1; ++x) {
            const auto a = z * occluder_width + x;
            const auto b = a + 1;
            const auto c = a + occluder_width;
            const auto d = c + 1;
            index_data[output_index++] = a;
            index_data[output_index++] = b;
            index_data[output_index++] = c;
            index_data[output_index++] = b;
            index_data[output_index++] = d;
            index_data[output_index++] = c;
        }
    }

    godot::Ref<godot::ArrayOccluder3D> occluder;
    occluder.instantiate();
    occluder->set_arrays(vertices, indices);
    return occluder;
}

godot::Ref<godot::ConcavePolygonShape3D> make_terrain_collision_shape(
    const TileMesh& tile) {
    // Physics does not need the render mesh's 128x128 surface. A 32x32 local
    // height surface preserves walking-scale terrain while reducing shape
    // construction from 32,768 to 2,048 triangles.
    constexpr std::int32_t sample_stride = 4;
    const auto width = tile.raw.width - tile.raw.borderSamples * 2;
    const auto height = tile.raw.height - tile.raw.borderSamples * 2;
    if (width < 3 || height < 3) return {};
    const auto surface_vertex_count = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height);
    if (surface_vertex_count > tile.vertices.size()) return {};

    std::vector<std::int32_t> source_x;
    std::vector<std::int32_t> source_z;
    for (std::int32_t x = 0; x < width; x += sample_stride) source_x.push_back(x);
    for (std::int32_t z = 0; z < height; z += sample_stride) source_z.push_back(z);
    if (source_x.back() != width - 1) source_x.push_back(width - 1);
    if (source_z.back() != height - 1) source_z.push_back(height - 1);

    const auto collision_width = static_cast<std::int32_t>(source_x.size());
    const auto collision_height = static_cast<std::int32_t>(source_z.size());
    godot::PackedVector3Array faces;
    faces.resize(
        static_cast<std::int64_t>(collision_width - 1)
        * static_cast<std::int64_t>(collision_height - 1) * 6);
    auto* face_data = faces.ptrw();
    std::int64_t output{};
    const auto vertex_at = [&](const std::int32_t x, const std::int32_t z) {
        const auto source_index = static_cast<std::size_t>(
            source_z[static_cast<std::size_t>(z)] * width
            + source_x[static_cast<std::size_t>(x)]);
        return to_godot(tile.vertices[source_index].position);
    };
    for (std::int32_t z = 0; z < collision_height - 1; ++z) {
        for (std::int32_t x = 0; x < collision_width - 1; ++x) {
            const auto a = vertex_at(x, z);
            const auto b = vertex_at(x + 1, z);
            const auto c = vertex_at(x, z + 1);
            const auto d = vertex_at(x + 1, z + 1);
            face_data[output++] = a;
            face_data[output++] = b;
            face_data[output++] = c;
            face_data[output++] = b;
            face_data[output++] = d;
            face_data[output++] = c;
        }
    }

    godot::Ref<godot::ConcavePolygonShape3D> shape;
    shape.instantiate();
    shape->set_faces(faces);
    return shape;
}

godot::Ref<godot::ArrayMesh> make_tree_billboard_mesh(const std::int32_t requested_card_count) {
    const auto card_count = std::clamp(requested_card_count, 1, 2);
    constexpr std::int32_t vertices_per_card = 4;
    constexpr std::int32_t indices_per_card = 6;
    godot::PackedVector3Array positions;
    godot::PackedVector3Array normals;
    godot::PackedColorArray colors;
    godot::PackedVector2Array uvs;
    godot::PackedInt32Array indices;
    positions.resize(card_count * vertices_per_card);
    normals.resize(card_count * vertices_per_card);
    colors.resize(card_count * vertices_per_card);
    uvs.resize(card_count * vertices_per_card);
    indices.resize(card_count * indices_per_card);

    auto* position_data = positions.ptrw();
    auto* normal_data = normals.ptrw();
    auto* color_data = colors.ptrw();
    auto* uv_data = uvs.ptrw();
    auto* index_data = indices.ptrw();
    for (std::int32_t card = 0; card < card_count; ++card) {
        const auto angle = static_cast<float>(card) * std::numbers::pi_v<float> * 0.5F;
        const godot::Vector3 right{std::cos(angle), 0.0F, std::sin(angle)};
        const godot::Vector3 normal{-right.z, 0.24F, right.x};
        const auto vertex = card * vertices_per_card;
        position_data[vertex] = -right * 0.5F;
        position_data[vertex + 1] = right * 0.5F;
        position_data[vertex + 2] = right * 0.5F + godot::Vector3{0.0F, 1.0F, 0.0F};
        position_data[vertex + 3] = -right * 0.5F + godot::Vector3{0.0F, 1.0F, 0.0F};
        uv_data[vertex] = {0.0F, 1.0F};
        uv_data[vertex + 1] = {1.0F, 1.0F};
        uv_data[vertex + 2] = {1.0F, 0.0F};
        uv_data[vertex + 3] = {0.0F, 0.0F};
        for (std::int32_t index = 0; index < vertices_per_card; ++index) {
            normal_data[vertex + index] = normal.normalized();
            color_data[vertex + index] = godot::Color{1.0F, 1.0F, 1.0F, 1.0F};
        }
        const auto triangle = card * indices_per_card;
        index_data[triangle] = vertex;
        index_data[triangle + 1] = vertex + 2;
        index_data[triangle + 2] = vertex + 1;
        index_data[triangle + 3] = vertex;
        index_data[triangle + 4] = vertex + 3;
        index_data[triangle + 5] = vertex + 2;
    }

    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = positions;
    arrays[godot::Mesh::ARRAY_NORMAL] = normals;
    arrays[godot::Mesh::ARRAY_COLOR] = colors;
    arrays[godot::Mesh::ARRAY_TEX_UV] = uvs;
    arrays[godot::Mesh::ARRAY_INDEX] = indices;
    godot::Ref<godot::ArrayMesh> mesh;
    mesh.instantiate();
    mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays);
    return mesh;
}

godot::Ref<godot::ArrayMesh> make_grass_clump_mesh() {
    // Instance density provides field coverage. Six broad blades retain the
    // clump footprint while halving triangle and fragment pressure.
    constexpr std::int32_t blade_count = 6;
    constexpr std::int32_t vertices_per_blade = 3;
    godot::PackedVector3Array positions;
    godot::PackedVector3Array normals;
    godot::PackedColorArray colors;
    godot::PackedInt32Array indices;
    positions.resize(blade_count * vertices_per_blade);
    normals.resize(blade_count * vertices_per_blade);
    colors.resize(blade_count * vertices_per_blade);
    indices.resize(blade_count * vertices_per_blade);

    auto* position_data = positions.ptrw();
    auto* normal_data = normals.ptrw();
    auto* color_data = colors.ptrw();
    auto* index_data = indices.ptrw();
    for (std::int32_t blade = 0; blade < blade_count; ++blade) {
        const auto unit = static_cast<float>(blade) / static_cast<float>(blade_count);
        const auto angle = static_cast<float>(blade) * 2.399963F;
        const auto radius = std::sqrt((static_cast<float>(blade) + 0.35F)
            / static_cast<float>(blade_count)) * 1.48F;
        const godot::Vector3 center{std::cos(angle) * radius, 0.0F, std::sin(angle) * radius};
        const auto facing = angle * 1.71F + 0.43F;
        const auto half_width = 0.026F + unit * 0.018F;
        const auto height = 0.38F + (1.0F - unit) * 0.36F;
        const godot::Vector3 side{std::cos(facing) * half_width, 0.0F, std::sin(facing) * half_width};
        const godot::Vector3 tip = center + godot::Vector3{
            std::cos(facing + std::numbers::pi_v<float> * 0.5F) * 0.08F,
            height,
            std::sin(facing + std::numbers::pi_v<float> * 0.5F) * 0.08F,
        };
        const auto normal = ((center + side) - (center - side)).cross(tip - (center - side)).normalized();
        const auto vertex = blade * vertices_per_blade;
        position_data[vertex] = center - side;
        position_data[vertex + 1] = center + side;
        position_data[vertex + 2] = tip;
        for (std::int32_t index = 0; index < vertices_per_blade; ++index) {
            normal_data[vertex + index] = normal;
            color_data[vertex + index] = godot::Color{0.76F, 0.90F, 0.62F, 1.0F};
            index_data[vertex + index] = vertex + index;
        }
    }

    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = positions;
    arrays[godot::Mesh::ARRAY_NORMAL] = normals;
    arrays[godot::Mesh::ARRAY_COLOR] = colors;
    arrays[godot::Mesh::ARRAY_INDEX] = indices;
    godot::Ref<godot::ArrayMesh> mesh;
    mesh.instantiate();
    mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays);
    return mesh;
}

godot::Ref<godot::MultiMesh> make_vegetation_multimesh(
    const std::span<const VegetationInstance> instances,
    const godot::Ref<godot::Mesh>& billboard_mesh,
    const glm::vec2 local_origin) {
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
        const auto angle = instance.parameters.y;
        const auto cosine = std::cos(angle);
        const auto sine = std::sin(angle);
        item[0] = cosine; item[1] = 0.0F; item[2] = sine;
        item[3] = instance.position.x - local_origin.x;
        item[4] = 0.0F; item[5] = 1.0F; item[6] = 0.0F; item[7] = instance.position.y;
        item[8] = -sine; item[9] = 0.0F; item[10] = cosine;
        item[11] = instance.position.z - local_origin.y;

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
