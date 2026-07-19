#pragma once

#include "TerrainTypes.hpp"
#include "VisualConfig.hpp"

namespace terrain {

[[nodiscard]] TileMeshPtr buildTerrainMesh(
    RawTile raw,
    std::uint64_t worldSeed,
    const WorldVisualConfig& visuals);

} // namespace terrain
