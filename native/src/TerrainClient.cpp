#include "TerrainClient.hpp"

#include <nlohmann/json.hpp>

#include <bit>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace terrain {
namespace {

constexpr std::int32_t DetailedScale = 8;
constexpr std::int64_t DetailedPageSamples = 4096;
constexpr std::int64_t DetailedPageHalfSamples = DetailedPageSamples / 2;
constexpr std::int64_t DetailedPageBorderSamples = 64;
constexpr std::int32_t DetailedClimateBorder = 1;
constexpr std::size_t MaximumDetailedPages = 8;

struct DetailedPageKey final {
    std::uint64_t seed{};
    std::int64_t x{};
    std::int64_t z{};
    bool operator==(const DetailedPageKey&) const = default;
};

struct DetailedPageKeyHash final {
    [[nodiscard]] std::size_t operator()(const DetailedPageKey& key) const noexcept {
        auto hash = key.seed ^ 0x9e3779b97f4a7c15ULL;
        hash ^= static_cast<std::uint64_t>(key.x) + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<std::uint64_t>(key.z) + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
        return static_cast<std::size_t>(hash);
    }
};

[[nodiscard]] std::int64_t floorDiv(std::int64_t numerator, std::int64_t denominator) noexcept {
    auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    if (remainder != 0 && ((remainder < 0) != (denominator < 0))) --quotient;
    return quotient;
}

[[nodiscard]] std::int64_t detailedPageIndex(std::int64_t coordinate) noexcept {
    return floorDiv(coordinate + DetailedPageHalfSamples, DetailedPageSamples);
}

[[nodiscard]] std::int64_t detailedPageOrigin(std::int64_t page) noexcept {
    return page * DetailedPageSamples - DetailedPageHalfSamples;
}

} // namespace

struct DetailedPage final {
    std::int64_t originI{};
    std::int64_t originJ{};
    std::int64_t storageOriginI{};
    std::int64_t storageOriginJ{};
    std::int32_t elevationWidth{};
    std::vector<std::int16_t> elevation;
    std::int64_t climateOriginI{};
    std::int64_t climateOriginJ{};
    std::int32_t climateWidth{};
    std::vector<float> climate;
};

struct DetailedPageCache final {
    struct Entry final {
        std::shared_ptr<const DetailedPage> page;
        std::uint64_t lastUse{};
    };

    std::mutex mutex;
    std::unordered_map<DetailedPageKey, Entry, DetailedPageKeyHash> pages;
    std::uint64_t useCounter{};
};

namespace {

[[nodiscard]] std::int16_t readLittleInt16(const char* bytes) noexcept {
    const auto low = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0]));
    const auto high = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[1]));
    return static_cast<std::int16_t>(low | static_cast<std::uint16_t>(high << 8U));
}

[[nodiscard]] float readLittleFloat(const char* bytes) noexcept {
    std::uint32_t bits = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0]))
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 8U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 16U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3])) << 24U);
    return std::bit_cast<float>(bits);
}

} // namespace

TerrainClient::TerrainClient(std::string serverUrl)
    : serverUrl_(std::move(serverUrl)), detailedPageCache_(std::make_shared<DetailedPageCache>()) {
    while (!serverUrl_.empty() && serverUrl_.back() == '/') serverUrl_.pop_back();
    if (!serverUrl_.starts_with("http://")) {
        throw std::invalid_argument("The native terrain client currently expects an http:// local server URL");
    }
}

std::unique_ptr<httplib::Client> TerrainClient::makeClient() const {
    auto client = std::make_unique<httplib::Client>(serverUrl_);
    client->set_connection_timeout(3, 0);
    client->set_read_timeout(300, 0);
    client->set_write_timeout(10, 0);
    client->set_keep_alive(true);
    return client;
}

WorldInfo TerrainClient::fetchWorld() const {
    auto client = makeClient();
    const auto response = client->Get("/world");
    if (!response) {
        throw std::runtime_error("Could not connect to Terrain Diffusion at " + serverUrl_);
    }
    if (response->status != 200) {
        throw std::runtime_error("GET /world returned HTTP " + std::to_string(response->status) + ": " + response->body);
    }
    const auto json = nlohmann::json::parse(response->body);
    WorldInfo world;
    world.seed = json.at("seed").get<std::string>();
    world.nativeResolution = json.at("native_resolution").get<std::int32_t>();
    world.latentResolution = json.at("latent_resolution").get<std::int32_t>();
    world.coarseResolution = json.value("coarse_resolution", world.latentResolution * 48);
    if (world.nativeResolution <= 0 || world.latentResolution <= 0 || world.coarseResolution <= 0) {
        throw std::runtime_error("GET /world returned invalid terrain resolutions");
    }
    return world;
}

RawTile TerrainClient::fetchTile(
    const TileKey& key,
    const LodSpec& spec,
    const WorldInfo& world,
    std::optional<std::uint64_t> seed,
    TileFetchTiming* timing) const {
    static_cast<void>(world);
    if (timing != nullptr) *timing = {};
    if (spec.source == TileSource::Detailed && spec.scale == DetailedScale) {
        if (!seed) throw std::invalid_argument("Scale-8 detailed terrain requires an explicit world seed");
        return fetchDetailedTile(key, spec, *seed, timing);
    }
    httplib::Params params;
    std::string path;
    constexpr std::int64_t derivativeBorder = 2;
    if (spec.source == TileSource::Detailed) {
        const auto startI = key.z * spec.segments - derivativeBorder;
        const auto startJ = key.x * spec.segments - derivativeBorder;
        params.emplace("i1", std::to_string(startI));
        params.emplace("j1", std::to_string(startJ));
        params.emplace("i2", std::to_string(startI + spec.segments + 1 + derivativeBorder * 2));
        params.emplace("j2", std::to_string(startJ + spec.segments + 1 + derivativeBorder * 2));
        params.emplace("scale", std::to_string(spec.scale));
        params.emplace("climate", spec.vegetation ? "1" : "0");
        if (seed) params.emplace("seed", std::to_string(*seed));
        path = "/terrain";
    } else if (spec.source == TileSource::Latent) {
        const auto startI = (key.z * spec.segments - derivativeBorder) * spec.stride;
        const auto startJ = (key.x * spec.segments - derivativeBorder) * spec.stride;
        params.emplace("i1", std::to_string(startI));
        params.emplace("j1", std::to_string(startJ));
        params.emplace("i2", std::to_string(startI + (spec.segments + derivativeBorder * 2) * spec.stride + 1));
        params.emplace("j2", std::to_string(startJ + (spec.segments + derivativeBorder * 2) * spec.stride + 1));
        params.emplace("stride", std::to_string(spec.stride));
        path = "/terrain/lod";
    } else {
        const auto startI = (key.z * spec.segments - derivativeBorder) * spec.stride;
        const auto startJ = (key.x * spec.segments - derivativeBorder) * spec.stride;
        params.emplace("i1", std::to_string(startI));
        params.emplace("j1", std::to_string(startJ));
        params.emplace("i2", std::to_string(startI + (spec.segments + derivativeBorder * 2) * spec.stride + 1));
        params.emplace("j2", std::to_string(startJ + (spec.segments + derivativeBorder * 2) * spec.stride + 1));
        params.emplace("stride", std::to_string(spec.stride));
        path = "/terrain/orbital";
    }

    auto client = makeClient();
    const auto requestStart = std::chrono::steady_clock::now();
    const auto response = client->Get(path + "?" + encodeQuery(params));
    const auto decodeStart = std::chrono::steady_clock::now();
    if (!response) throw std::runtime_error("Terrain request lost connection to " + serverUrl_);
    if (response->status != 200) {
        throw std::runtime_error(path + " returned HTTP " + std::to_string(response->status) + ": " + response->body);
    }

    RawTile tile;
    tile.key = key;
    tile.spec = spec;
    tile.width = headerDimension(*response, "X-Width");
    tile.height = headerDimension(*response, "X-Height");
    tile.borderSamples = static_cast<std::int32_t>(derivativeBorder);
    const auto expectedDimension = spec.segments + 1 + tile.borderSamples * 2;
    if (tile.width != expectedDimension || tile.height != expectedDimension) {
        throw std::runtime_error(path + " returned " + std::to_string(tile.width) + "x"
            + std::to_string(tile.height) + ", expected " + std::to_string(expectedDimension) + "x"
            + std::to_string(expectedDimension));
    }
    tile.origin = glm::dvec2{
        static_cast<double>(key.x) * spec.tileWorldSize,
        static_cast<double>(key.z) * spec.tileWorldSize,
    };

    const auto sampleCount = static_cast<std::size_t>(tile.width) * static_cast<std::size_t>(tile.height);
    const auto elevationBytes = sampleCount * sizeof(std::int16_t);
    const auto expectsClimate = (spec.source == TileSource::Detailed && spec.vegetation)
        || spec.source == TileSource::Orbital;
    const auto climateBytes = sampleCount * 4U * sizeof(float);
    const auto expectedBytes = elevationBytes + (expectsClimate ? climateBytes : 0U);
    if (response->body.size() != expectedBytes) {
        throw std::runtime_error(path + " returned " + std::to_string(response->body.size())
            + " payload bytes, expected exactly " + std::to_string(expectedBytes));
    }
    tile.elevations.resize(sampleCount);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        tile.elevations[index] = readLittleInt16(response->body.data() + index * 2U);
    }

    if (expectsClimate) {
        tile.climate.resize(sampleCount * 4U);
        const char* climate = response->body.data() + elevationBytes;
        for (std::size_t index = 0; index < tile.climate.size(); ++index) {
            tile.climate[index] = readLittleFloat(climate + index * 4U);
        }
    }
    if (timing != nullptr) {
        timing->requestMilliseconds = std::chrono::duration<double, std::milli>(
            decodeStart - requestStart).count();
        timing->decodeMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - decodeStart).count();
        timing->responseBytes = response->body.size();
    }
    return tile;
}

std::shared_ptr<const DetailedPage> TerrainClient::fetchDetailedPage(
    std::int64_t pageX,
    std::int64_t pageZ,
    std::uint64_t seed,
    TileFetchTiming* timing) const {
    const DetailedPageKey cacheKey{seed, pageX, pageZ};
    // The Terrain Diffusion pipeline is shared by all HTTP requests and the
    // first page is intentionally large. Serialising cache misses prevents
    // duplicate 32 MiB pages and avoids concurrent MPS decoder pressure;
    // resident page hits remain a small mutex-protected map lookup.
    std::unique_lock cacheLock{detailedPageCache_->mutex};
    if (auto found = detailedPageCache_->pages.find(cacheKey);
        found != detailedPageCache_->pages.end()) {
        found->second.lastUse = ++detailedPageCache_->useCounter;
        return found->second.page;
    }

    auto page = std::make_shared<DetailedPage>();
    page->originI = detailedPageOrigin(pageZ);
    page->originJ = detailedPageOrigin(pageX);
    page->storageOriginI = page->originI - DetailedPageBorderSamples;
    page->storageOriginJ = page->originJ - DetailedPageBorderSamples;
    page->elevationWidth = static_cast<std::int32_t>(
        DetailedPageSamples + DetailedPageBorderSamples * 2);
    const auto pageLoadStart = std::chrono::steady_clock::now();
    std::fprintf(stderr,
        "Detailed scale-8 page load: page=(%lld,%lld) target-origin=(%lld,%lld) samples=4096x4096\n",
        static_cast<long long>(pageX), static_cast<long long>(pageZ),
        static_cast<long long>(page->originJ), static_cast<long long>(page->originI));

    const auto fetchResponse = [&](const httplib::Params& params) {
        auto client = makeClient();
        const auto requestStart = std::chrono::steady_clock::now();
        auto response = client->Get("/terrain?" + encodeQuery(params));
        const auto requestEnd = std::chrono::steady_clock::now();
        if (!response) throw std::runtime_error("Terrain source-page request lost connection to " + serverUrl_);
        if (response->status != 200) {
            throw std::runtime_error("GET /terrain source page returned HTTP "
                + std::to_string(response->status) + ": " + response->body);
        }
        if (timing != nullptr) {
            timing->requestMilliseconds += std::chrono::duration<double, std::milli>(
                requestEnd - requestStart).count();
            timing->responseBytes += response->body.size();
        }
        return response;
    };

    httplib::Params elevationParams;
    elevationParams.emplace("i1", std::to_string(page->storageOriginI));
    elevationParams.emplace("j1", std::to_string(page->storageOriginJ));
    elevationParams.emplace("i2", std::to_string(page->storageOriginI + page->elevationWidth));
    elevationParams.emplace("j2", std::to_string(page->storageOriginJ + page->elevationWidth));
    elevationParams.emplace("scale", std::to_string(DetailedScale));
    elevationParams.emplace("climate", "0");
    elevationParams.emplace("seed", std::to_string(seed));
    const auto decodeStart = std::chrono::steady_clock::now();
    const auto elevationResponse = fetchResponse(elevationParams);
    const auto elevationWidth = headerDimension(*elevationResponse, "X-Width");
    const auto elevationHeight = headerDimension(*elevationResponse, "X-Height");
    if (elevationWidth != page->elevationWidth || elevationHeight != page->elevationWidth) {
        throw std::runtime_error("Scale-8 source page returned an unexpected dimension");
    }
    const auto elevationCount = static_cast<std::size_t>(elevationWidth)
        * static_cast<std::size_t>(elevationHeight);
    if (elevationResponse->body.size() != elevationCount * sizeof(std::int16_t)) {
        throw std::runtime_error("Scale-8 source page returned a malformed elevation payload");
    }
    page->elevation.resize(elevationCount);
    for (std::size_t index = 0; index < elevationCount; ++index) {
        page->elevation[index] = readLittleInt16(elevationResponse->body.data() + index * 2U);
    }

    // Climate is smooth at kilometre scales; retaining a 4096-square float4
    // page would waste 256 MiB. Fetch the matching 512-square native climate
    // field plus one-sample borders and interpolate it while constructing
    // render tiles.
    const auto nativeStorageOriginI = page->storageOriginI / DetailedScale;
    const auto nativeStorageOriginJ = page->storageOriginJ / DetailedScale;
    const auto climateSamples = page->elevationWidth / DetailedScale
        + DetailedClimateBorder * 2;
    page->climateOriginI = nativeStorageOriginI - DetailedClimateBorder;
    page->climateOriginJ = nativeStorageOriginJ - DetailedClimateBorder;
    page->climateWidth = climateSamples;
    httplib::Params climateParams;
    climateParams.emplace("i1", std::to_string(page->climateOriginI));
    climateParams.emplace("j1", std::to_string(page->climateOriginJ));
    climateParams.emplace("i2", std::to_string(page->climateOriginI + climateSamples));
    climateParams.emplace("j2", std::to_string(page->climateOriginJ + climateSamples));
    climateParams.emplace("scale", "1");
    climateParams.emplace("climate", "1");
    climateParams.emplace("seed", std::to_string(seed));
    const auto climateResponse = fetchResponse(climateParams);
    const auto climateWidth = headerDimension(*climateResponse, "X-Width");
    const auto climateHeight = headerDimension(*climateResponse, "X-Height");
    if (climateWidth != climateSamples || climateHeight != climateSamples) {
        throw std::runtime_error("Detailed source climate page returned an unexpected dimension");
    }
    const auto climateCount = static_cast<std::size_t>(climateWidth)
        * static_cast<std::size_t>(climateHeight);
    const auto climateOffset = climateCount * sizeof(std::int16_t);
    const auto climateBytes = climateCount * 4U * sizeof(float);
    if (climateResponse->body.size() != climateOffset + climateBytes) {
        throw std::runtime_error("Detailed source climate page returned a malformed payload");
    }
    page->climate.resize(climateCount * 4U);
    for (std::size_t index = 0; index < page->climate.size(); ++index) {
        page->climate[index] = readLittleFloat(climateResponse->body.data() + climateOffset + index * 4U);
    }
    if (timing != nullptr) {
        timing->decodeMilliseconds += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - decodeStart).count();
    }

    const auto use = ++detailedPageCache_->useCounter;
    detailedPageCache_->pages.emplace(cacheKey, DetailedPageCache::Entry{page, use});
    while (detailedPageCache_->pages.size() > MaximumDetailedPages) {
        auto oldest = detailedPageCache_->pages.end();
        for (auto iterator = detailedPageCache_->pages.begin();
            iterator != detailedPageCache_->pages.end(); ++iterator) {
            if (iterator->first == cacheKey) continue;
            if (oldest == detailedPageCache_->pages.end()
                || iterator->second.lastUse < oldest->second.lastUse) oldest = iterator;
        }
        if (oldest == detailedPageCache_->pages.end()) break;
        detailedPageCache_->pages.erase(oldest);
    }
    std::fprintf(stderr, "Detailed scale-8 page ready: page=(%lld,%lld) %.1f ms cache=%zu/%zu\n",
        static_cast<long long>(pageX), static_cast<long long>(pageZ),
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - pageLoadStart).count(),
        detailedPageCache_->pages.size(), MaximumDetailedPages);
    return page;
}

RawTile TerrainClient::fetchDetailedTile(
    const TileKey& key,
    const LodSpec& spec,
    std::uint64_t seed,
    TileFetchTiming* timing) const {
    constexpr std::int32_t derivativeBorder = 2;
    const auto width = spec.segments + 1 + derivativeBorder * 2;
    const auto startI = key.z * static_cast<std::int64_t>(spec.segments) * spec.stride
        - static_cast<std::int64_t>(derivativeBorder) * spec.stride;
    const auto startJ = key.x * static_cast<std::int64_t>(spec.segments) * spec.stride
        - static_cast<std::int64_t>(derivativeBorder) * spec.stride;

    std::unordered_map<DetailedPageKey, std::shared_ptr<const DetailedPage>, DetailedPageKeyHash> pages;
    const auto tileCenterI = startI + static_cast<std::int64_t>(width - 1) * spec.stride / 2;
    const auto tileCenterJ = startJ + static_cast<std::int64_t>(width - 1) * spec.stride / 2;
    const auto preferredPageX = detailedPageIndex(tileCenterJ);
    const auto preferredPageZ = detailedPageIndex(tileCenterI);
    const DetailedPageKey preferredKey{seed, preferredPageX, preferredPageZ};
    const auto preferredPage = fetchDetailedPage(preferredPageX, preferredPageZ, seed, timing);
    pages.emplace(preferredKey, preferredPage);
    const auto pageFor = [&](std::int64_t sampleI, std::int64_t sampleJ) -> const DetailedPage& {
        if (sampleI >= preferredPage->storageOriginI
            && sampleJ >= preferredPage->storageOriginJ
            && sampleI < preferredPage->storageOriginI + preferredPage->elevationWidth
            && sampleJ < preferredPage->storageOriginJ + preferredPage->elevationWidth) {
            return *preferredPage;
        }
        const auto pageX = detailedPageIndex(sampleJ);
        const auto pageZ = detailedPageIndex(sampleI);
        const DetailedPageKey pageKey{seed, pageX, pageZ};
        auto found = pages.find(pageKey);
        if (found == pages.end()) {
            found = pages.emplace(pageKey, fetchDetailedPage(pageX, pageZ, seed, timing)).first;
        }
        return *found->second;
    };

    RawTile tile;
    tile.key = key;
    tile.spec = spec;
    tile.width = width;
    tile.height = width;
    tile.borderSamples = derivativeBorder;
    tile.origin = glm::dvec2{
        static_cast<double>(key.x) * spec.tileWorldSize,
        static_cast<double>(key.z) * spec.tileWorldSize,
    };
    const auto sampleCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(width);
    tile.elevations.resize(sampleCount);
    tile.climate.resize(sampleCount * 4U);

    const auto assemblyStart = std::chrono::steady_clock::now();
    for (std::int32_t z = 0; z < width; ++z) {
        const auto sampleI = startI + static_cast<std::int64_t>(z) * spec.stride;
        for (std::int32_t x = 0; x < width; ++x) {
            const auto sampleJ = startJ + static_cast<std::int64_t>(x) * spec.stride;
            const auto& page = pageFor(sampleI, sampleJ);
            const auto localI = sampleI - page.storageOriginI;
            const auto localJ = sampleJ - page.storageOriginJ;
            const auto outputIndex = static_cast<std::size_t>(z * width + x);
            tile.elevations[outputIndex] = page.elevation[static_cast<std::size_t>(
                localI * page.elevationWidth + localJ)];

            const auto climateI = static_cast<double>(sampleI) / DetailedScale
                - static_cast<double>(page.climateOriginI);
            const auto climateJ = static_cast<double>(sampleJ) / DetailedScale
                - static_cast<double>(page.climateOriginJ);
            const auto i0 = std::clamp(static_cast<std::int32_t>(std::floor(climateI)), 0, page.climateWidth - 2);
            const auto j0 = std::clamp(static_cast<std::int32_t>(std::floor(climateJ)), 0, page.climateWidth - 2);
            const auto ti = static_cast<float>(climateI - i0);
            const auto tj = static_cast<float>(climateJ - j0);
            for (std::int32_t channel = 0; channel < 4; ++channel) {
                const auto at = [&](std::int32_t ci, std::int32_t cj) {
                    return page.climate[(static_cast<std::size_t>(ci)
                        * static_cast<std::size_t>(page.climateWidth)
                        + static_cast<std::size_t>(cj)) * 4U
                        + static_cast<std::size_t>(channel)];
                };
                const auto top = std::lerp(at(i0, j0), at(i0, j0 + 1), tj);
                const auto bottom = std::lerp(at(i0 + 1, j0), at(i0 + 1, j0 + 1), tj);
                tile.climate[outputIndex * 4U + static_cast<std::size_t>(channel)]
                    = std::lerp(top, bottom, ti);
            }
        }
    }
    if (timing != nullptr) {
        timing->decodeMilliseconds += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - assemblyStart).count();
    }
    return tile;
}

std::int32_t TerrainClient::headerDimension(const httplib::Response& response, const char* name) {
    const auto text = response.get_header_value(name);
    std::int32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 2 || value > 8193) {
        throw std::runtime_error(std::string{"Invalid "} + name + " terrain response header");
    }
    return value;
}

std::string TerrainClient::encodeQuery(const httplib::Params& params) {
    std::string result;
    for (const auto& [key, value] : params) {
        if (!result.empty()) result.push_back('&');
        result += key;
        result.push_back('=');
        result += value;
    }
    return result;
}

} // namespace terrain
