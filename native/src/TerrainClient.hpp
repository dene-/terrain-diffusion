#pragma once

#include "TerrainTypes.hpp"

#include <httplib.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace terrain {

struct DetailedPage;
struct DetailedPageCache;

struct TileFetchTiming final {
    double requestMilliseconds{};
    double decodeMilliseconds{};
    std::size_t responseBytes{};
};

class TerrainClient final {
public:
    explicit TerrainClient(std::string serverUrl);

    [[nodiscard]] WorldInfo fetchWorld() const;
    [[nodiscard]] RawTile fetchTile(
        const TileKey& key,
        const LodSpec& spec,
        const WorldInfo& world,
        std::optional<std::uint64_t> seed = std::nullopt,
        TileFetchTiming* timing = nullptr) const;

private:
    [[nodiscard]] std::unique_ptr<httplib::Client> makeClient() const;
    [[nodiscard]] std::shared_ptr<const DetailedPage> fetchDetailedPage(
        std::int64_t pageX,
        std::int64_t pageZ,
        std::uint64_t seed,
        TileFetchTiming* timing) const;
    [[nodiscard]] RawTile fetchDetailedTile(
        const TileKey& key,
        const LodSpec& spec,
        std::uint64_t seed,
        TileFetchTiming* timing) const;
    [[nodiscard]] static std::int32_t headerDimension(const httplib::Response& response, const char* name);
    [[nodiscard]] static std::string encodeQuery(const httplib::Params& params);

    std::string serverUrl_;
    std::shared_ptr<DetailedPageCache> detailedPageCache_;
};

} // namespace terrain
