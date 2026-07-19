#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace terrain {

struct HudState final {
    std::string seed{"-"};
    std::string message{"CONNECTING TO TERRAIN GENERATOR"};
    glm::dvec3 position{};
    std::optional<double> aimDistance;
    double installedRadius{};
    double requestedRadius{};
    float flySpeed{};
    float fps{};
    float onePercentLowFps{};
    std::size_t loadedTiles{};
    std::size_t pendingTiles{};
    std::size_t trees{};
    std::size_t grass{};
    std::size_t drawCalls{};
    std::size_t terrainDraws{};
    double streamFetchMilliseconds{};
    double streamDecodeMilliseconds{};
    double streamDerivedMilliseconds{};
    double streamVegetationMilliseconds{};
    double streamBuildMilliseconds{};
    double gpuUploadMilliseconds{};
    double terrainMemoryMegabytes{};
    double vegetationMemoryMegabytes{};
    bool flying{};
    bool ready{};
    bool captured{};
    bool cloudsEnabled{true};
    std::string tuningLabel;
    std::uint32_t debugView{};
};

class HudRasterizer final {
public:
    static constexpr std::uint32_t Width = 960;
    static constexpr std::uint32_t Height = 540;

    HudRasterizer();
    void render(const HudState& state);
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }

private:
    struct Color final { std::uint8_t r{}, g{}, b{}, a{}; };

    void clear() noexcept;
    void blendPixel(std::int32_t x, std::int32_t y, Color color) noexcept;
    void fillRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, Color color) noexcept;
    void strokeRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, Color color) noexcept;
    void drawText(std::int32_t x, std::int32_t y, std::string_view text, std::int32_t scale, Color color) noexcept;
    void drawTextRight(std::int32_t right, std::int32_t y, std::string_view text, std::int32_t scale, Color color) noexcept;
    void drawTextCentered(std::int32_t center, std::int32_t y, std::string_view text, std::int32_t scale, Color color) noexcept;
    [[nodiscard]] static std::int32_t textWidth(std::string_view text, std::int32_t scale) noexcept;

    std::vector<std::uint8_t> pixels_;
};

} // namespace terrain
