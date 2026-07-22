#include <terrain/TerrainClient.hpp>

#include <nlohmann/json.hpp>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace terrain {
namespace {

constexpr std::int32_t DetailedScale = 8;
constexpr std::int64_t DetailedPageSamples = 4096;
constexpr std::int64_t DetailedPageBorderSamples = 64;
// A 128-segment L5 tile includes both endpoints plus two 32-sample
// derivative rows. The positive edge therefore reaches origin+4096+64 and
// needs one more stored sample than the negative edge.
constexpr std::int64_t DetailedPageTrailingBorderSamples = 65;
constexpr std::int32_t DetailedClimateBorder = 1;
constexpr std::size_t DefaultMaximumDetailedPages = 8;
constexpr std::size_t MinimumDetailedPages = 4;
constexpr std::size_t MaximumDetailedPages = 64;

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
    return floorDiv(coordinate, DetailedPageSamples);
}

[[nodiscard]] std::int64_t detailedPageOrigin(std::int64_t page) noexcept {
    return page * DetailedPageSamples;
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
    // Source-page generation is intentionally serial because the local
    // Terrain Diffusion service owns one MPS pipeline. Keep that serialization
    // separate from the cache map lock: a slow 40 MiB miss must not block
    // unrelated resident-page hits for its entire HTTP/decode lifetime.
    std::mutex missMutex;
    std::unordered_map<DetailedPageKey, Entry, DetailedPageKeyHash> pages;
    std::uint64_t useCounter{};
    std::size_t maximumPages{DefaultMaximumDetailedPages};
};

struct TerrainMapArchive final {
    struct IndexEntry final {
        std::uint64_t offset{};
        std::uint32_t size{};
        std::uint32_t checksum{};
    };

    struct CachedChunk final {
        std::shared_ptr<const std::vector<std::uint16_t>> pixels;
        std::uint64_t lastUse{};
    };

    struct CachedEcologyTile final {
        std::shared_ptr<const std::vector<float>> values;
        std::uint64_t lastUse{};
    };

    explicit TerrainMapArchive(std::filesystem::path archivePath);

    [[nodiscard]] WorldInfo worldInfo() const;
    [[nodiscard]] float sampleElevation(double worldX, double worldZ) const;
    [[nodiscard]] bool hasClimate() const noexcept { return !climate.empty(); }
    [[nodiscard]] std::array<float, 4> sampleClimate(double worldX, double worldZ) const;
    [[nodiscard]] bool hasEcology() const noexcept { return !ecologyIndex.empty(); }
    [[nodiscard]] std::array<float, 8> sampleEcology(double worldX, double worldZ) const;

    [[nodiscard]] float samplePixel(std::int64_t row, std::int64_t column) const;
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint16_t>> loadChunk(
        std::int32_t row,
        std::int32_t column) const;
    [[nodiscard]] std::shared_ptr<const std::vector<float>> loadEcologyTile(
        std::int32_t row,
        std::int32_t column) const;

    std::filesystem::path path;
    std::string seed;
    std::int32_t rows{};
    std::int32_t columns{};
    std::int32_t chunkWidth{};
    std::int32_t chunkHeight{};
    double nativeResolution{};
    double latentResolution{};
    double coarseResolution{};
    double originX{};
    double originZ{};
    double outsideElevation{-6500.0};
    std::int32_t elevationOffset{32768};
    double elevationUnitsPerMetre{2.0};
    std::int32_t climateWidth{};
    std::int32_t climateHeight{};
    double climateResolution{};
    double climateOriginX{};
    double climateOriginZ{};
    std::vector<float> climate;
    std::int32_t ecologyRows{};
    std::int32_t ecologyColumns{};
    std::int32_t ecologyChunkWidth{};
    std::int32_t ecologyChunkHeight{};
    std::int32_t ecologyChannels{};
    double ecologyResolution{};
    double ecologyOriginX{};
    double ecologyOriginZ{};
    std::vector<IndexEntry> index;
    std::vector<IndexEntry> ecologyIndex;
    mutable std::mutex cacheMutex;
    // Distinct archive chunks can be read and decoded in parallel. A small
    // striped miss gate still prevents the four stream workers from decoding
    // the same PNG simultaneously after a shared cache miss.
    mutable std::array<std::mutex, 16> chunkLoadMutexes;
    mutable std::unordered_map<std::size_t, CachedChunk> cache;
    mutable std::uint64_t useCounter{};
    std::size_t maximumCachedChunks{256};
    mutable std::mutex ecologyCacheMutex;
    mutable std::array<std::mutex, 16> ecologyLoadMutexes;
    mutable std::unordered_map<std::size_t, CachedEcologyTile> ecologyCache;
    mutable std::uint64_t ecologyUseCounter{};
    std::size_t maximumCachedEcologyTiles{64};
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

[[nodiscard]] float readLittleHalf(const char* bytes) noexcept {
    const auto bits = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0]))
        | static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[1])) << 8U));
    const auto sign = (bits & 0x8000U) != 0U ? -1.0F : 1.0F;
    const auto exponent = static_cast<std::uint16_t>((bits >> 10U) & 0x1fU);
    const auto mantissa = static_cast<std::uint16_t>(bits & 0x03ffU);
    if (exponent == 0U) {
        if (mantissa == 0U) return std::copysign(0.0F, sign);
        return sign * std::ldexp(static_cast<float>(mantissa) / 1'024.0F, -14);
    }
    if (exponent == 0x1fU) {
        return mantissa == 0U
            ? std::copysign(std::numeric_limits<float>::infinity(), sign)
            : std::numeric_limits<float>::quiet_NaN();
    }
    return sign * std::ldexp(
        1.0F + static_cast<float>(mantissa) / 1'024.0F,
        static_cast<int>(exponent) - 15);
}

[[nodiscard]] std::uint32_t readLittleUint32(const char* bytes) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0]))
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 8U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 16U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3])) << 24U);
}

[[nodiscard]] std::uint64_t readLittleUint64(const char* bytes) noexcept {
    std::uint64_t value{};
    for (std::uint32_t byte = 0; byte < 8U; ++byte) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[byte])) << (byte * 8U);
    }
    return value;
}

[[nodiscard]] std::uint32_t crc32(const unsigned char* bytes, const std::size_t size) noexcept {
    auto crc = std::uint32_t{0xffffffffU};
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (std::uint32_t bit = 0; bit < 8U; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-
                static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] double cubic(
    const double p0,
    const double p1,
    const double p2,
    const double p3,
    const double t) noexcept {
    return p1 + 0.5 * t * (p2 - p0 + t * (
        2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3
        + t * (3.0 * (p1 - p2) + p3 - p0)));
}

} // namespace

TerrainMapArchive::TerrainMapArchive(std::filesystem::path archivePath)
    : path(std::move(archivePath)) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) throw std::runtime_error("Could not open terrain map archive: " + path.string());

    std::array<char, 16> prefix{};
    stream.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    if (!stream || std::string_view{prefix.data(), 8} != "TDMAP001") {
        throw std::runtime_error("Terrain map archive has an invalid magic header: " + path.string());
    }
    const auto headerSize = readLittleUint32(prefix.data() + 8);
    const auto indexEntrySize = readLittleUint32(prefix.data() + 12);
    if (headerSize == 0U || headerSize > 1024U * 1024U || indexEntrySize != 16U) {
        throw std::runtime_error("Terrain map archive has an unsupported header layout");
    }
    std::string header(headerSize, '\0');
    stream.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (!stream) throw std::runtime_error("Terrain map archive header is truncated");
    const auto metadata = nlohmann::json::parse(header);
    const auto version = metadata.at("version").get<std::int32_t>();
    if (metadata.at("format").get<std::string>() != "terrain-diffusion-map"
        || (version != 1 && version != 2 && version != 3)
        || metadata.at("chunk_codec").get<std::string>() != "png16") {
        throw std::runtime_error("Terrain map archive uses an unsupported format version or codec");
    }
    seed = metadata.at("seed").get<std::string>();
    rows = metadata.at("rows").get<std::int32_t>();
    columns = metadata.at("columns").get<std::int32_t>();
    chunkWidth = metadata.at("chunk_width").get<std::int32_t>();
    chunkHeight = metadata.at("chunk_height").get<std::int32_t>();
    nativeResolution = metadata.at("native_resolution_m").get<double>();
    latentResolution = metadata.at("latent_resolution_m").get<double>();
    coarseResolution = metadata.at("coarse_resolution_m").get<double>();
    originX = metadata.at("origin_x_m").get<double>();
    originZ = metadata.at("origin_z_m").get<double>();
    outsideElevation = metadata.value("outside_elevation_m", -6500.0);
    elevationOffset = metadata.at("elevation_offset").get<std::int32_t>();
    elevationUnitsPerMetre = metadata.at("elevation_units_per_m").get<double>();
    if (rows <= 0 || columns <= 0 || chunkWidth <= 1 || chunkHeight <= 1
        || nativeResolution <= 0.0 || latentResolution <= 0.0 || coarseResolution <= 0.0
        || elevationUnitsPerMetre <= 0.0) {
        throw std::runtime_error("Terrain map archive metadata contains invalid dimensions or scales");
    }

    const auto entryCount = static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns);
    index.resize(entryCount);
    const auto archiveSize = std::filesystem::file_size(path);
    auto payloadEnd = static_cast<std::uint64_t>(16U + headerSize)
        + static_cast<std::uint64_t>(entryCount * 16U);
    std::array<char, 16> encodedEntry{};
    for (auto& entry : index) {
        stream.read(encodedEntry.data(), static_cast<std::streamsize>(encodedEntry.size()));
        if (!stream) throw std::runtime_error("Terrain map archive index is truncated");
        entry.offset = readLittleUint64(encodedEntry.data());
        entry.size = readLittleUint32(encodedEntry.data() + 8);
        entry.checksum = readLittleUint32(encodedEntry.data() + 12);
        if (entry.size == 0U || entry.offset > archiveSize
            || static_cast<std::uint64_t>(entry.size) > archiveSize - entry.offset) {
            throw std::runtime_error("Terrain map archive index points outside the file");
        }
        payloadEnd = std::max(payloadEnd, entry.offset + entry.size);
    }

    ecologyChannels = metadata.value("ecology_channels", 0);
    if (ecologyChannels != 0) {
        if (version < 3 || ecologyChannels != 8
            || metadata.value("ecology_encoding", std::string{})
                != "float16le_interleaved"
            || metadata.value("ecology_codec", std::string{}) != "raw") {
            throw std::runtime_error("Terrain map archive uses an unsupported ecology layout");
        }
        ecologyRows = metadata.at("ecology_rows").get<std::int32_t>();
        ecologyColumns = metadata.at("ecology_columns").get<std::int32_t>();
        ecologyChunkWidth = metadata.at("ecology_chunk_width").get<std::int32_t>();
        ecologyChunkHeight = metadata.at("ecology_chunk_height").get<std::int32_t>();
        ecologyResolution = metadata.at("ecology_resolution_m").get<double>();
        ecologyOriginX = metadata.at("ecology_origin_x_m").get<double>();
        ecologyOriginZ = metadata.at("ecology_origin_z_m").get<double>();
        const auto ecologyEntryCount = metadata.at("ecology_index_entries").get<std::size_t>();
        if (ecologyRows != rows || ecologyColumns != columns
            || ecologyChunkWidth < 2 || ecologyChunkHeight < 2 || ecologyResolution <= 0.0
            || ecologyEntryCount != entryCount
            || metadata.at("ecology_index_entry_bytes").get<std::uint32_t>() != 16U) {
            throw std::runtime_error("Terrain map archive ecology metadata is invalid");
        }
        const auto expectedEcologyBytes = static_cast<std::uint64_t>(ecologyChunkWidth)
            * static_cast<std::uint64_t>(ecologyChunkHeight)
            * static_cast<std::uint64_t>(ecologyChannels) * sizeof(std::uint16_t);
        if (expectedEcologyBytes > std::numeric_limits<std::uint32_t>::max()
            || metadata.at("ecology_bytes_per_chunk").get<std::uint64_t>()
                != expectedEcologyBytes) {
            throw std::runtime_error("Terrain map archive ecology tile size is invalid");
        }
        const auto ecologyPayloadStart = payloadEnd
            + metadata.value("climate_payload_bytes", std::uint64_t{});
        ecologyIndex.resize(ecologyEntryCount);
        for (auto& entry : ecologyIndex) {
            stream.read(encodedEntry.data(), static_cast<std::streamsize>(encodedEntry.size()));
            if (!stream) throw std::runtime_error("Terrain map ecology index is truncated");
            entry.offset = readLittleUint64(encodedEntry.data());
            entry.size = readLittleUint32(encodedEntry.data() + 8);
            entry.checksum = readLittleUint32(encodedEntry.data() + 12);
            if (entry.size != expectedEcologyBytes || entry.offset < ecologyPayloadStart
                || entry.offset > archiveSize
                || static_cast<std::uint64_t>(entry.size) > archiveSize - entry.offset) {
                throw std::runtime_error("Terrain map ecology index points outside the file");
            }
        }
        if (metadata.at("ecology_payload_bytes").get<std::uint64_t>()
            != expectedEcologyBytes * ecologyEntryCount) {
            throw std::runtime_error("Terrain map archive ecology payload size is invalid");
        }
    }

    const auto climateChannels = metadata.value("climate_channels", 0);
    if (climateChannels != 0) {
        if (version < 2 || climateChannels != 4
            || metadata.value("climate_encoding", std::string{})
                != "float32le_interleaved") {
            throw std::runtime_error("Terrain map archive uses an unsupported climate layout");
        }
        climateWidth = metadata.at("climate_width").get<std::int32_t>();
        climateHeight = metadata.at("climate_height").get<std::int32_t>();
        climateResolution = metadata.at("climate_resolution_m").get<double>();
        climateOriginX = metadata.at("climate_origin_x_m").get<double>();
        climateOriginZ = metadata.at("climate_origin_z_m").get<double>();
        if (climateWidth < 2 || climateHeight < 2 || climateResolution <= 0.0) {
            throw std::runtime_error("Terrain map archive climate metadata is invalid");
        }
        const auto climateSampleCount = static_cast<std::size_t>(climateWidth)
            * static_cast<std::size_t>(climateHeight) * 4U;
        const auto climateBytes = climateSampleCount * sizeof(float);
        if (metadata.at("climate_payload_bytes").get<std::size_t>() != climateBytes
            || payloadEnd > archiveSize || climateBytes > archiveSize - payloadEnd) {
            throw std::runtime_error("Terrain map archive climate payload is truncated");
        }
        std::vector<char> encodedClimate(climateBytes);
        stream.seekg(static_cast<std::streamoff>(payloadEnd));
        stream.read(encodedClimate.data(), static_cast<std::streamsize>(encodedClimate.size()));
        if (!stream) throw std::runtime_error("Terrain map archive climate payload is truncated");
        const auto checksum = crc32(
            reinterpret_cast<const unsigned char*>(encodedClimate.data()), encodedClimate.size());
        if (checksum != metadata.at("climate_checksum").get<std::uint32_t>()) {
            throw std::runtime_error("Terrain map archive climate checksum mismatch");
        }
        climate.resize(climateSampleCount);
        for (std::size_t climateIndex = 0; climateIndex < climate.size(); ++climateIndex) {
            climate[climateIndex] = readLittleFloat(
                encodedClimate.data() + climateIndex * sizeof(float));
            if (!std::isfinite(climate[climateIndex])) {
                throw std::runtime_error("Terrain map archive climate contains a non-finite value");
            }
        }
    }
    std::fprintf(stderr,
        "Terrain map archive ready: %s seed=%s chunks=%dx%d pixels=%dx%d resolution=%.2fm climate=%dx%dx%d ecology=%dx%dx%d@%.0fm\n",
        path.string().c_str(), seed.c_str(), columns, rows, chunkWidth, chunkHeight,
        nativeResolution, climateWidth, climateHeight, climateChannels,
        ecologyColumns * ecologyChunkWidth, ecologyRows * ecologyChunkHeight,
        ecologyChannels, ecologyResolution);
}

WorldInfo TerrainMapArchive::worldInfo() const {
    return WorldInfo{
        .seed = seed,
        .nativeResolution = static_cast<std::int32_t>(std::llround(nativeResolution)),
        .latentResolution = static_cast<std::int32_t>(std::llround(latentResolution)),
        .coarseResolution = static_cast<std::int32_t>(std::llround(coarseResolution)),
    };
}

std::shared_ptr<const std::vector<std::uint16_t>> TerrainMapArchive::loadChunk(
    const std::int32_t row,
    const std::int32_t column) const {
    const auto chunkIndex = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns)
        + static_cast<std::size_t>(column);
    {
        std::scoped_lock cacheLock{cacheMutex};
        if (auto found = cache.find(chunkIndex); found != cache.end()) {
            found->second.lastUse = ++useCounter;
            return found->second.pixels;
        }
    }

    std::scoped_lock missLock{chunkLoadMutexes[chunkIndex % chunkLoadMutexes.size()]};
    {
        std::scoped_lock cacheLock{cacheMutex};
        if (auto found = cache.find(chunkIndex); found != cache.end()) {
            found->second.lastUse = ++useCounter;
            return found->second.pixels;
        }
    }

    const auto& entry = index.at(chunkIndex);
    std::vector<unsigned char> compressed(entry.size);
    std::ifstream stream{path, std::ios::binary};
    stream.seekg(static_cast<std::streamoff>(entry.offset));
    stream.read(
        reinterpret_cast<char*>(compressed.data()),
        static_cast<std::streamsize>(compressed.size()));
    if (!stream) throw std::runtime_error("Terrain map chunk payload is truncated");
    if (crc32(compressed.data(), compressed.size()) != entry.checksum) {
        throw std::runtime_error("Terrain map chunk checksum mismatch");
    }
    if (compressed.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Terrain map PNG chunk is too large to decode");
    }
    int decodedWidth{};
    int decodedHeight{};
    int channels{};
    stbi_us* decoded = stbi_load_16_from_memory(
        compressed.data(),
        static_cast<int>(compressed.size()),
        &decodedWidth,
        &decodedHeight,
        &channels,
        1);
    if (decoded == nullptr || decodedWidth != chunkWidth || decodedHeight != chunkHeight) {
        const auto reason = stbi_failure_reason();
        if (decoded != nullptr) stbi_image_free(decoded);
        throw std::runtime_error(std::string{"Could not decode terrain map PNG chunk: "}
            + (reason != nullptr ? reason : "invalid dimensions"));
    }
    const auto pixelCount = static_cast<std::size_t>(chunkWidth)
        * static_cast<std::size_t>(chunkHeight);
    auto pixels = std::make_shared<std::vector<std::uint16_t>>(decoded, decoded + pixelCount);
    stbi_image_free(decoded);
    std::scoped_lock cacheLock{cacheMutex};
    const auto use = ++useCounter;
    cache.emplace(chunkIndex, CachedChunk{pixels, use});
    while (cache.size() > maximumCachedChunks) {
        auto oldest = std::ranges::min_element(
            cache, {}, [](const auto& item) { return item.second.lastUse; });
        if (oldest == cache.end() || oldest->first == chunkIndex) break;
        cache.erase(oldest);
    }
    return pixels;
}

std::shared_ptr<const std::vector<float>> TerrainMapArchive::loadEcologyTile(
    const std::int32_t row,
    const std::int32_t column) const {
    const auto tileIndex = static_cast<std::size_t>(row)
        * static_cast<std::size_t>(ecologyColumns) + static_cast<std::size_t>(column);
    {
        std::scoped_lock cacheLock{ecologyCacheMutex};
        if (auto found = ecologyCache.find(tileIndex); found != ecologyCache.end()) {
            found->second.lastUse = ++ecologyUseCounter;
            return found->second.values;
        }
    }

    std::scoped_lock missLock{
        ecologyLoadMutexes[tileIndex % ecologyLoadMutexes.size()]};
    {
        std::scoped_lock cacheLock{ecologyCacheMutex};
        if (auto found = ecologyCache.find(tileIndex); found != ecologyCache.end()) {
            found->second.lastUse = ++ecologyUseCounter;
            return found->second.values;
        }
    }

    const auto& entry = ecologyIndex.at(tileIndex);
    std::vector<char> encoded(entry.size);
    std::ifstream stream{path, std::ios::binary};
    stream.seekg(static_cast<std::streamoff>(entry.offset));
    stream.read(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!stream) throw std::runtime_error("Terrain map ecology payload is truncated");
    if (crc32(reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size())
        != entry.checksum) {
        throw std::runtime_error("Terrain map ecology checksum mismatch");
    }
    const auto valueCount = static_cast<std::size_t>(ecologyChunkWidth)
        * static_cast<std::size_t>(ecologyChunkHeight)
        * static_cast<std::size_t>(ecologyChannels);
    if (encoded.size() != valueCount * sizeof(std::uint16_t)) {
        throw std::runtime_error("Terrain map ecology tile has an invalid size");
    }
    auto values = std::make_shared<std::vector<float>>(valueCount);
    for (std::size_t valueIndex = 0; valueIndex < valueCount; ++valueIndex) {
        const auto value = readLittleHalf(encoded.data() + valueIndex * sizeof(std::uint16_t));
        if (!std::isfinite(value)) {
            throw std::runtime_error("Terrain map ecology tile contains a non-finite value");
        }
        (*values)[valueIndex] = value;
    }

    std::scoped_lock cacheLock{ecologyCacheMutex};
    const auto use = ++ecologyUseCounter;
    ecologyCache.emplace(tileIndex, CachedEcologyTile{values, use});
    while (ecologyCache.size() > maximumCachedEcologyTiles) {
        auto oldest = std::ranges::min_element(
            ecologyCache, {}, [](const auto& item) { return item.second.lastUse; });
        if (oldest == ecologyCache.end() || oldest->first == tileIndex) break;
        ecologyCache.erase(oldest);
    }
    return values;
}

float TerrainMapArchive::samplePixel(const std::int64_t row, const std::int64_t column) const {
    const auto totalRows = static_cast<std::int64_t>(rows) * chunkHeight;
    const auto totalColumns = static_cast<std::int64_t>(columns) * chunkWidth;
    if (row < 0 || column < 0 || row >= totalRows || column >= totalColumns) {
        return static_cast<float>(outsideElevation);
    }
    const auto chunkRow = static_cast<std::int32_t>(row / chunkHeight);
    const auto chunkColumn = static_cast<std::int32_t>(column / chunkWidth);
    const auto localRow = static_cast<std::int32_t>(row % chunkHeight);
    const auto localColumn = static_cast<std::int32_t>(column % chunkWidth);
    const auto pixels = loadChunk(chunkRow, chunkColumn);
    const auto encoded = (*pixels)[static_cast<std::size_t>(localRow * chunkWidth + localColumn)];
    return static_cast<float>(
        (static_cast<double>(encoded) - elevationOffset) / elevationUnitsPerMetre);
}

float TerrainMapArchive::sampleElevation(const double worldX, const double worldZ) const {
    const auto pixelX = (worldX - originX) / nativeResolution;
    const auto pixelZ = (worldZ - originZ) / nativeResolution;
    const auto x1 = static_cast<std::int64_t>(std::floor(pixelX));
    const auto z1 = static_cast<std::int64_t>(std::floor(pixelZ));
    const auto tx = std::clamp(pixelX - static_cast<double>(x1), 0.0, 1.0);
    const auto tz = std::clamp(pixelZ - static_cast<double>(z1), 0.0, 1.0);
    std::array<double, 4> rowsInterpolated{};
    for (std::int64_t dz = -1; dz <= 2; ++dz) {
        rowsInterpolated[static_cast<std::size_t>(dz + 1)] = cubic(
            samplePixel(z1 + dz, x1 - 1),
            samplePixel(z1 + dz, x1),
            samplePixel(z1 + dz, x1 + 1),
            samplePixel(z1 + dz, x1 + 2),
            tx);
    }
    const auto elevation = cubic(
        rowsInterpolated[0],
        rowsInterpolated[1],
        rowsInterpolated[2],
        rowsInterpolated[3],
        tz);
    return static_cast<float>(std::clamp(elevation, -16'384.0, 16'383.5));
}

std::array<float, 4> TerrainMapArchive::sampleClimate(
    const double worldX,
    const double worldZ) const {
    if (climate.empty()) return {};
    const auto pixelX = std::clamp(
        (worldX - climateOriginX) / climateResolution,
        0.0,
        static_cast<double>(climateWidth - 1));
    const auto pixelZ = std::clamp(
        (worldZ - climateOriginZ) / climateResolution,
        0.0,
        static_cast<double>(climateHeight - 1));
    const auto x0 = static_cast<std::int32_t>(std::floor(pixelX));
    const auto z0 = static_cast<std::int32_t>(std::floor(pixelZ));
    const auto x1 = std::min(x0 + 1, climateWidth - 1);
    const auto z1 = std::min(z0 + 1, climateHeight - 1);
    const auto tx = static_cast<float>(pixelX - x0);
    const auto tz = static_cast<float>(pixelZ - z0);
    const auto at = [this](
        const std::int32_t x,
        const std::int32_t z,
        const std::size_t channel) {
        return climate[(static_cast<std::size_t>(z) * static_cast<std::size_t>(climateWidth)
            + static_cast<std::size_t>(x)) * 4U + channel];
    };
    std::array<float, 4> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
        result[channel] = std::lerp(
            std::lerp(at(x0, z0, channel), at(x1, z0, channel), tx),
            std::lerp(at(x0, z1, channel), at(x1, z1, channel), tx),
            tz);
    }
    return result;
}

std::array<float, 8> TerrainMapArchive::sampleEcology(
    const double worldX,
    const double worldZ) const {
    if (ecologyIndex.empty()) return {};
    const auto totalWidth = ecologyColumns * ecologyChunkWidth;
    const auto totalHeight = ecologyRows * ecologyChunkHeight;
    const auto pixelX = std::clamp(
        (worldX - ecologyOriginX) / ecologyResolution,
        0.0,
        static_cast<double>(totalWidth - 1));
    const auto pixelZ = std::clamp(
        (worldZ - ecologyOriginZ) / ecologyResolution,
        0.0,
        static_cast<double>(totalHeight - 1));
    const auto x0 = static_cast<std::int32_t>(std::floor(pixelX));
    const auto z0 = static_cast<std::int32_t>(std::floor(pixelZ));
    const auto x1 = std::min(x0 + 1, totalWidth - 1);
    const auto z1 = std::min(z0 + 1, totalHeight - 1);
    const auto tx = static_cast<float>(pixelX - x0);
    const auto tz = static_cast<float>(pixelZ - z0);

    struct EcologySample final {
        std::shared_ptr<const std::vector<float>> tile;
        std::size_t base{};
    };
    const auto sampleAt = [this](const std::int32_t x, const std::int32_t z) {
        const auto tileColumn = x / ecologyChunkWidth;
        const auto tileRow = z / ecologyChunkHeight;
        const auto localX = x % ecologyChunkWidth;
        const auto localZ = z % ecologyChunkHeight;
        return EcologySample{
            .tile = loadEcologyTile(tileRow, tileColumn),
            .base = (static_cast<std::size_t>(localZ)
                * static_cast<std::size_t>(ecologyChunkWidth)
                + static_cast<std::size_t>(localX))
                * static_cast<std::size_t>(ecologyChannels),
        };
    };
    const auto a = sampleAt(x0, z0);
    const auto b = sampleAt(x1, z0);
    const auto c = sampleAt(x0, z1);
    const auto d = sampleAt(x1, z1);
    std::array<float, 8> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
        result[channel] = std::lerp(
            std::lerp((*a.tile)[a.base + channel], (*b.tile)[b.base + channel], tx),
            std::lerp((*c.tile)[c.base + channel], (*d.tile)[d.base + channel], tx),
            tz);
    }
    return result;
}

TerrainClient::TerrainClient(std::string terrainSource)
    : serverUrl_(std::move(terrainSource)), detailedPageCache_(std::make_shared<DetailedPageCache>()) {
    if (serverUrl_.starts_with("http://")) {
        while (!serverUrl_.empty() && serverUrl_.back() == '/') serverUrl_.pop_back();
        return;
    }
    mapArchive_ = std::make_shared<TerrainMapArchive>(std::filesystem::path{serverUrl_});
}

bool TerrainClient::isLocalMap() const noexcept {
    return mapArchive_ != nullptr;
}

void TerrainClient::setDetailedPageCacheBudgetMegabytes(const std::size_t megabytes) {
    // One page currently stores a 4225-square int16 elevation field and a
    // roughly 530-square float4 climate field. Use an explicit estimate so
    // the typed memory budget controls the LRU without needing a page first.
    constexpr auto elevationSamples = DetailedPageSamples + DetailedPageBorderSamples
        + DetailedPageTrailingBorderSamples;
    constexpr auto climateSamples = elevationSamples / DetailedScale
        + DetailedClimateBorder * 2;
    constexpr auto estimatedPageBytes = static_cast<std::size_t>(
        elevationSamples * elevationSamples * static_cast<std::int64_t>(sizeof(std::int16_t))
        + climateSamples * climateSamples * 4 * static_cast<std::int64_t>(sizeof(float)));
    const auto budgetBytes = megabytes * 1024U * 1024U;
    const auto maximumPages = std::clamp(
        budgetBytes / estimatedPageBytes,
        MinimumDetailedPages,
        MaximumDetailedPages);

    std::scoped_lock lock{detailedPageCache_->mutex};
    if (detailedPageCache_->maximumPages == maximumPages) return;
    detailedPageCache_->maximumPages = maximumPages;
    while (detailedPageCache_->pages.size() > detailedPageCache_->maximumPages) {
        auto oldest = std::ranges::min_element(
            detailedPageCache_->pages,
            {},
            [](const auto& entry) { return entry.second.lastUse; });
        if (oldest == detailedPageCache_->pages.end()) break;
        detailedPageCache_->pages.erase(oldest);
    }
    std::fprintf(stderr,
        "Detailed scale-8 page cache budget: %zu MiB -> %zu pages (~%.0f MiB)\n",
        megabytes,
        detailedPageCache_->maximumPages,
        static_cast<double>(detailedPageCache_->maximumPages * estimatedPageBytes)
            / static_cast<double>(1024U * 1024U));
}

httplib::Client& TerrainClient::client() const {
    // Terrain workers are long-lived. Give each thread one client per server
    // so requests remain sequential for that client while HTTP keep-alive can
    // actually reuse its connection across tile requests.
    thread_local std::unordered_map<std::string, std::unique_ptr<httplib::Client>> clients;
    auto& client = clients[serverUrl_];
    if (!client) {
        client = std::make_unique<httplib::Client>(serverUrl_);
        client->set_connection_timeout(3, 0);
        client->set_read_timeout(300, 0);
        client->set_write_timeout(10, 0);
        client->set_keep_alive(true);
    }
    return *client;
}

WorldInfo TerrainClient::fetchWorld() const {
    if (mapArchive_) return mapArchive_->worldInfo();
    const auto response = client().Get("/world");
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
    if (timing != nullptr) *timing = {};
    if (mapArchive_) return fetchMapTile(key, spec, timing);
    httplib::Params params;
    std::string path;
    constexpr std::int64_t derivativeBorder = 2;
    double detailedNativeStep{};
    std::int64_t detailedSourceStartI{};
    std::int64_t detailedSourceStartJ{};
    std::int32_t expectedResponseWidth{};
    if (spec.source == TileSource::Detailed) {
        // Fetch a bounded native window and evaluate every LOD from that one
        // canonical grid. The API's scale-2/4/8 routes use scale-dependent
        // half-pixel interpolation, so mixing them makes parent and child
        // heights disagree at otherwise shared coordinates.
        detailedNativeStep = spec.spacing / static_cast<double>(world.nativeResolution);
        const auto desiredStartI = (static_cast<double>(key.z * spec.segments)
            - static_cast<double>(derivativeBorder)) * detailedNativeStep;
        const auto desiredStartJ = (static_cast<double>(key.x * spec.segments)
            - static_cast<double>(derivativeBorder)) * detailedNativeStep;
        const auto desiredEndI = (static_cast<double>(key.z * spec.segments + spec.segments)
            + static_cast<double>(derivativeBorder)) * detailedNativeStep;
        const auto desiredEndJ = (static_cast<double>(key.x * spec.segments + spec.segments)
            + static_cast<double>(derivativeBorder)) * detailedNativeStep;
        detailedSourceStartI = static_cast<std::int64_t>(std::floor(desiredStartI)) - 1;
        detailedSourceStartJ = static_cast<std::int64_t>(std::floor(desiredStartJ)) - 1;
        const auto sourceEndI = static_cast<std::int64_t>(std::ceil(desiredEndI)) + 2;
        const auto sourceEndJ = static_cast<std::int64_t>(std::ceil(desiredEndJ)) + 2;
        if (sourceEndI - detailedSourceStartI != sourceEndJ - detailedSourceStartJ) {
            throw std::logic_error("Detailed terrain source window must be square");
        }
        expectedResponseWidth = static_cast<std::int32_t>(
            sourceEndI - detailedSourceStartI);
        params.emplace("i1", std::to_string(detailedSourceStartI));
        params.emplace("j1", std::to_string(detailedSourceStartJ));
        params.emplace("i2", std::to_string(sourceEndI));
        params.emplace("j2", std::to_string(sourceEndJ));
        params.emplace("scale", "1");
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

    const auto requestStart = std::chrono::steady_clock::now();
    const auto response = client().Get(path + "?" + encodeQuery(params));
    const auto decodeStart = std::chrono::steady_clock::now();
    if (!response) throw std::runtime_error("Terrain request lost connection to " + serverUrl_);
    if (response->status != 200) {
        throw std::runtime_error(path + " returned HTTP " + std::to_string(response->status) + ": " + response->body);
    }

    RawTile tile;
    tile.key = key;
    tile.spec = spec;
    const auto responseWidth = headerDimension(*response, "X-Width");
    const auto responseHeight = headerDimension(*response, "X-Height");
    tile.borderSamples = static_cast<std::int32_t>(derivativeBorder);
    tile.width = spec.segments + 1 + tile.borderSamples * 2;
    tile.height = tile.width;
    const auto expectedResponseDimension = spec.source == TileSource::Detailed
        ? expectedResponseWidth : tile.width;
    if (responseWidth != expectedResponseDimension || responseHeight != expectedResponseDimension) {
        throw std::runtime_error(path + " returned " + std::to_string(responseWidth) + "x"
            + std::to_string(responseHeight) + ", expected " + std::to_string(expectedResponseDimension) + "x"
            + std::to_string(expectedResponseDimension));
    }
    tile.origin = glm::dvec2{
        static_cast<double>(key.x) * spec.tileWorldSize,
        static_cast<double>(key.z) * spec.tileWorldSize,
    };

    const auto responseSampleCount = static_cast<std::size_t>(responseWidth)
        * static_cast<std::size_t>(responseHeight);
    const auto elevationBytes = responseSampleCount * sizeof(std::int16_t);
    const auto expectsClimate = (spec.source == TileSource::Detailed && spec.vegetation)
        || spec.source == TileSource::Orbital;
    const auto climateBytes = responseSampleCount * 4U * sizeof(float);
    const auto expectedBytes = elevationBytes + (expectsClimate ? climateBytes : 0U);
    if (response->body.size() != expectedBytes) {
        throw std::runtime_error(path + " returned " + std::to_string(response->body.size())
            + " payload bytes, expected exactly " + std::to_string(expectedBytes));
    }
    const auto sampleCount = static_cast<std::size_t>(tile.width)
        * static_cast<std::size_t>(tile.height);
    tile.elevations.resize(sampleCount);
    if (spec.source == TileSource::Detailed) tile.renderElevations.resize(sampleCount);
    const auto responseElevationAt = [&](const std::int32_t x, const std::int32_t z) {
        const auto index = static_cast<std::size_t>(z)
            * static_cast<std::size_t>(responseWidth) + static_cast<std::size_t>(x);
        return readLittleInt16(response->body.data() + index * 2U);
    };
    const auto cubicElevation = [](const double p0, const double p1, const double p2,
                                   const double p3, const double t) {
        return p1 + 0.5 * t * (p2 - p0 + t * (
            2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3
            + t * (3.0 * (p1 - p2) + p3 - p0)));
    };
    const auto reconstructedResponseElevation = [&](const double sourceI, const double sourceJ) {
        const auto localI = sourceI - static_cast<double>(detailedSourceStartI);
        const auto localJ = sourceJ - static_cast<double>(detailedSourceStartJ);
        const auto i0 = std::clamp(static_cast<std::int32_t>(std::floor(localI)), 1, responseHeight - 3);
        const auto j0 = std::clamp(static_cast<std::int32_t>(std::floor(localJ)), 1, responseWidth - 3);
        const auto ti = std::clamp(localI - static_cast<double>(i0), 0.0, 1.0);
        const auto tj = std::clamp(localJ - static_cast<double>(j0), 0.0, 1.0);
        std::array<double, 4> rows{};
        for (std::int32_t row = -1; row <= 2; ++row) {
            rows[static_cast<std::size_t>(row + 1)] = cubicElevation(
                static_cast<double>(responseElevationAt(j0 - 1, i0 + row)),
                static_cast<double>(responseElevationAt(j0, i0 + row)),
                static_cast<double>(responseElevationAt(j0 + 1, i0 + row)),
                static_cast<double>(responseElevationAt(j0 + 2, i0 + row)),
                tj);
        }
        return cubicElevation(rows[0], rows[1], rows[2], rows[3], ti);
    };
    for (std::int32_t z = 0; z < tile.height; ++z) {
        for (std::int32_t x = 0; x < tile.width; ++x) {
            const auto destination = static_cast<std::size_t>(z * tile.width + x);
            if (spec.source == TileSource::Detailed) {
                const auto sourceI = (static_cast<double>(key.z * spec.segments)
                    + static_cast<double>(z - tile.borderSamples)) * detailedNativeStep;
                const auto sourceJ = (static_cast<double>(key.x * spec.segments)
                    + static_cast<double>(x - tile.borderSamples)) * detailedNativeStep;
                const auto renderElevation = reconstructedResponseElevation(sourceI, sourceJ);
                tile.renderElevations[destination] = static_cast<float>(renderElevation);
                tile.elevations[destination] = static_cast<std::int16_t>(std::clamp(
                    std::llround(renderElevation),
                    static_cast<long long>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<long long>(std::numeric_limits<std::int16_t>::max())));
            } else {
                tile.elevations[destination] = responseElevationAt(x, z);
            }
        }
    }

    if (expectsClimate) {
        tile.climate.resize(sampleCount * 4U);
        const char* climate = response->body.data() + elevationBytes;
        for (std::int32_t z = 0; z < tile.height; ++z) {
            for (std::int32_t x = 0; x < tile.width; ++x) {
                const auto destinationIndex = static_cast<std::size_t>(z * tile.width + x);
                for (std::size_t channel = 0; channel < 4U; ++channel) {
                    if (spec.source == TileSource::Detailed) {
                        const auto sourceI = (static_cast<double>(key.z * spec.segments)
                            + static_cast<double>(z - tile.borderSamples)) * detailedNativeStep;
                        const auto sourceJ = (static_cast<double>(key.x * spec.segments)
                            + static_cast<double>(x - tile.borderSamples)) * detailedNativeStep;
                        const auto localI = sourceI - static_cast<double>(detailedSourceStartI);
                        const auto localJ = sourceJ - static_cast<double>(detailedSourceStartJ);
                        const auto i0 = std::clamp(static_cast<std::int32_t>(std::floor(localI)), 0, responseHeight - 2);
                        const auto j0 = std::clamp(static_cast<std::int32_t>(std::floor(localJ)), 0, responseWidth - 2);
                        const auto ti = static_cast<float>(std::clamp(localI - i0, 0.0, 1.0));
                        const auto tj = static_cast<float>(std::clamp(localJ - j0, 0.0, 1.0));
                        const auto climateAt = [&](const std::int32_t ci, const std::int32_t cj) {
                            const auto sourceIndex = static_cast<std::size_t>(ci)
                                * static_cast<std::size_t>(responseWidth)
                                + static_cast<std::size_t>(cj);
                            return readLittleFloat(climate
                                + (sourceIndex * 4U + channel) * sizeof(float));
                        };
                        tile.climate[destinationIndex * 4U + channel] = std::lerp(
                            std::lerp(climateAt(i0, j0), climateAt(i0, j0 + 1), tj),
                            std::lerp(climateAt(i0 + 1, j0), climateAt(i0 + 1, j0 + 1), tj), ti);
                    } else {
                        tile.climate[destinationIndex * 4U + channel] = readLittleFloat(
                            climate + (destinationIndex * 4U + channel) * sizeof(float));
                    }
                }
            }
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

RawTile TerrainClient::fetchMapTile(
    const TileKey& key,
    const LodSpec& spec,
    TileFetchTiming* timing) const {
    if (!mapArchive_) throw std::logic_error("Local terrain map is not configured");
    const auto decodeStart = std::chrono::steady_clock::now();
    constexpr std::int32_t derivativeBorder = 3;
    RawTile tile;
    tile.key = key;
    tile.spec = spec;
    tile.borderSamples = derivativeBorder;
    tile.width = spec.segments + 1 + derivativeBorder * 2;
    tile.height = tile.width;
    tile.origin = glm::dvec2{
        static_cast<double>(key.x) * spec.tileWorldSize,
        static_cast<double>(key.z) * spec.tileWorldSize,
    };
    const auto sampleCount = static_cast<std::size_t>(tile.width)
        * static_cast<std::size_t>(tile.height);
    tile.elevations.resize(sampleCount);
    tile.renderElevations.resize(sampleCount);
    // Detailed ecology informs terrain materials at every LOD, not only the
    // two geometry rings that carry vegetation instances. Version 2 macro
    // climate retains its previous vegetation-only behaviour.
    const auto withEcology = mapArchive_->hasEcology();
    const auto withClimate = !withEcology && spec.vegetation && mapArchive_->hasClimate();
    tile.climateChannels = withEcology ? 8 : 4;
    if (withEcology || withClimate) {
        tile.climate.resize(sampleCount * static_cast<std::size_t>(tile.climateChannels));
    }
    for (std::int32_t z = 0; z < tile.height; ++z) {
        const auto worldZ = tile.origin.y
            + static_cast<double>(z - tile.borderSamples) * spec.spacing;
        for (std::int32_t x = 0; x < tile.width; ++x) {
            const auto worldX = tile.origin.x
                + static_cast<double>(x - tile.borderSamples) * spec.spacing;
            const auto elevation = mapArchive_->sampleElevation(worldX, worldZ);
            const auto index = static_cast<std::size_t>(z * tile.width + x);
            tile.renderElevations[index] = elevation;
            tile.elevations[index] = static_cast<std::int16_t>(std::clamp(
                std::llround(elevation),
                static_cast<long long>(std::numeric_limits<std::int16_t>::min()),
                static_cast<long long>(std::numeric_limits<std::int16_t>::max())));
            if (withEcology) {
                const auto ecology = mapArchive_->sampleEcology(worldX, worldZ);
                for (std::size_t channel = 0; channel < ecology.size(); ++channel) {
                    tile.climate[index * ecology.size() + channel] = ecology[channel];
                }
            } else if (withClimate) {
                const auto climate = mapArchive_->sampleClimate(worldX, worldZ);
                for (std::size_t channel = 0; channel < climate.size(); ++channel) {
                    tile.climate[index * 4U + channel] = climate[channel];
                }
            }
        }
    }
    tile.geometricErrorSourceUnits = spec.spacing <= mapArchive_->nativeResolution
        ? 0.5 : spec.spacing * 0.5;
    if (timing != nullptr) {
        timing->decodeMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - decodeStart).count();
    }
    return tile;
}

std::shared_ptr<const DetailedPage> TerrainClient::fetchDetailedPage(
    std::int64_t pageX,
    std::int64_t pageZ,
    std::uint64_t seed,
    TileFetchTiming* timing) const {
    const DetailedPageKey cacheKey{seed, pageX, pageZ};
    {
        std::scoped_lock cacheLock{detailedPageCache_->mutex};
        if (auto found = detailedPageCache_->pages.find(cacheKey);
            found != detailedPageCache_->pages.end()) {
            found->second.lastUse = ++detailedPageCache_->useCounter;
            return found->second.page;
        }
    }

    // The Terrain Diffusion pipeline is shared by all HTTP requests and a
    // source page is intentionally large. Serialize only misses to prevent
    // duplicate pages and concurrent MPS decoder pressure. Recheck after the
    // miss gate because another worker may have populated this exact page.
    std::unique_lock missLock{detailedPageCache_->missMutex};
    {
        std::scoped_lock cacheLock{detailedPageCache_->mutex};
        if (auto found = detailedPageCache_->pages.find(cacheKey);
            found != detailedPageCache_->pages.end()) {
            found->second.lastUse = ++detailedPageCache_->useCounter;
            return found->second.page;
        }
    }

    auto page = std::make_shared<DetailedPage>();
    page->originI = detailedPageOrigin(pageZ);
    page->originJ = detailedPageOrigin(pageX);
    page->storageOriginI = page->originI - DetailedPageBorderSamples;
    page->storageOriginJ = page->originJ - DetailedPageBorderSamples;
    page->elevationWidth = static_cast<std::int32_t>(
        DetailedPageSamples + DetailedPageBorderSamples
        + DetailedPageTrailingBorderSamples);
    const auto pageLoadStart = std::chrono::steady_clock::now();
    std::fprintf(stderr,
        "Detailed scale-8 page load: page=(%lld,%lld) target-origin=(%lld,%lld) samples=4096x4096\n",
        static_cast<long long>(pageX), static_cast<long long>(pageZ),
        static_cast<long long>(page->originJ), static_cast<long long>(page->originI));

    const auto fetchResponse = [&](const httplib::Params& params) {
        const auto requestStart = std::chrono::steady_clock::now();
        auto response = client().Get("/terrain?" + encodeQuery(params));
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

    std::size_t cacheSize{};
    std::size_t maximumPages{};
    {
        std::scoped_lock cacheLock{detailedPageCache_->mutex};
        const auto use = ++detailedPageCache_->useCounter;
        detailedPageCache_->pages.emplace(cacheKey, DetailedPageCache::Entry{page, use});
        while (detailedPageCache_->pages.size() > detailedPageCache_->maximumPages) {
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
        cacheSize = detailedPageCache_->pages.size();
        maximumPages = detailedPageCache_->maximumPages;
    }
    std::fprintf(stderr, "Detailed scale-8 page ready: page=(%lld,%lld) %.1f ms cache=%zu/%zu\n",
        static_cast<long long>(pageX), static_cast<long long>(pageZ),
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - pageLoadStart).count(),
        cacheSize, maximumPages);
    return page;
}

RawTile TerrainClient::fetchDetailedTile(
    const TileKey& key,
    const LodSpec& spec,
    std::uint64_t seed,
    TileFetchTiming* timing) const {
    // L0 uses a one-sample reconstruction filter before physical
    // derivatives. Three samples keep edge normals identical across tiles;
    // coarser levels retain their existing two-sample derivative border.
    const std::int32_t derivativeBorder = spec.stride == 1 ? 3 : 2;
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

    if (spec.stride == 1) {
        // This is the exact scale-8 source grid (apart from int16 metre
        // quantization), so there are no omitted source samples to measure.
        tile.geometricErrorSourceUnits = 0.0;
    } else {
        const auto sourceAt = [&](const std::int64_t sampleI, const std::int64_t sampleJ) {
            const auto& page = pageFor(sampleI, sampleJ);
            const auto localI = sampleI - page.storageOriginI;
            const auto localJ = sampleJ - page.storageOriginJ;
            return static_cast<double>(page.elevation[static_cast<std::size_t>(
                localI * page.elevationWidth + localJ)]);
        };
        const auto coarseAt = [&](const std::int32_t x, const std::int32_t z) {
            return static_cast<double>(tile.elevations[static_cast<std::size_t>(
                (z + derivativeBorder) * width + x + derivativeBorder)]);
        };
        double maximumError{};
        for (std::int32_t z = 0; z < spec.segments; ++z) {
            const auto sampleI = key.z * static_cast<std::int64_t>(spec.segments) * spec.stride
                + static_cast<std::int64_t>(z) * spec.stride;
            for (std::int32_t x = 0; x < spec.segments; ++x) {
                const auto sampleJ = key.x * static_cast<std::int64_t>(spec.segments) * spec.stride
                    + static_cast<std::int64_t>(x) * spec.stride;
                const auto h00 = coarseAt(x, z);
                const auto h10 = coarseAt(x + 1, z);
                const auto h01 = coarseAt(x, z + 1);
                const auto h11 = coarseAt(x + 1, z + 1);
                const auto accumulateError = [&](const std::int64_t offsetI, const std::int64_t offsetJ) {
                    if (offsetI <= 0 || offsetJ <= 0
                        || offsetI >= spec.stride || offsetJ >= spec.stride) return;
                    const auto u = static_cast<double>(offsetJ) / spec.stride;
                    const auto v = static_cast<double>(offsetI) / spec.stride;
                    const auto top = std::lerp(h00, h10, u);
                    const auto bottom = std::lerp(h01, h11, u);
                    const auto approximation = std::lerp(top, bottom, v);
                    maximumError = std::max(maximumError, std::abs(
                        sourceAt(sampleI + offsetI, sampleJ + offsetJ) - approximation));
                };
                const std::array<std::int64_t, 3> offsets{
                    spec.stride / 4,
                    spec.stride / 2,
                    spec.stride * 3 / 4,
                };
                for (std::size_t row = 0; row < offsets.size(); ++row) {
                    if (row > 0 && offsets[row] == offsets[row - 1]) continue;
                    for (std::size_t column = 0; column < offsets.size(); ++column) {
                        if (column > 0 && offsets[column] == offsets[column - 1]) continue;
                        accumulateError(offsets[row], offsets[column]);
                    }
                }
            }
        }
        tile.geometricErrorSourceUnits = maximumError;
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
