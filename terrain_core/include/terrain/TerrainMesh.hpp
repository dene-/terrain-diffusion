#pragma once

#include <terrain/TerrainTypes.hpp>
#include <terrain/VisualConfig.hpp>

namespace terrain {

[[nodiscard]] TileMeshPtr buildTerrainMesh(
    RawTile raw,
    std::uint64_t worldSeed,
    const WorldVisualConfig& visuals);

} // namespace terrain
