#include "Hud.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <format>
#include <string>
#include <string_view>

namespace terrain {
namespace {

using Glyph = std::array<std::uint8_t, 7>;

[[nodiscard]] Glyph glyph(char value) noexcept {
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(value)))) {
    case 'A': return {14,17,17,31,17,17,17}; case 'B': return {30,17,17,30,17,17,30};
    case 'C': return {14,17,16,16,16,17,14}; case 'D': return {30,17,17,17,17,17,30};
    case 'E': return {31,16,16,30,16,16,31}; case 'F': return {31,16,16,30,16,16,16};
    case 'G': return {14,17,16,23,17,17,15}; case 'H': return {17,17,17,31,17,17,17};
    case 'I': return {14,4,4,4,4,4,14}; case 'J': return {7,2,2,2,18,18,12};
    case 'K': return {17,18,20,24,20,18,17}; case 'L': return {16,16,16,16,16,16,31};
    case 'M': return {17,27,21,21,17,17,17}; case 'N': return {17,25,21,19,17,17,17};
    case 'O': return {14,17,17,17,17,17,14}; case 'P': return {30,17,17,30,16,16,16};
    case 'Q': return {14,17,17,17,21,18,13}; case 'R': return {30,17,17,30,20,18,17};
    case 'S': return {15,16,16,14,1,1,30}; case 'T': return {31,4,4,4,4,4,4};
    case 'U': return {17,17,17,17,17,17,14}; case 'V': return {17,17,17,17,17,10,4};
    case 'W': return {17,17,17,21,21,21,10}; case 'X': return {17,17,10,4,10,17,17};
    case 'Y': return {17,17,10,4,4,4,4}; case 'Z': return {31,1,2,4,8,16,31};
    case '0': return {14,17,19,21,25,17,14}; case '1': return {4,12,4,4,4,4,14};
    case '2': return {14,17,1,2,4,8,31}; case '3': return {30,1,1,14,1,1,30};
    case '4': return {2,6,10,18,31,2,2}; case '5': return {31,16,16,30,1,1,30};
    case '6': return {14,16,16,30,17,17,14}; case '7': return {31,1,2,4,8,8,8};
    case '8': return {14,17,17,14,17,17,14}; case '9': return {14,17,17,15,1,1,14};
    case '-': return {0,0,0,31,0,0,0}; case '.': return {0,0,0,0,0,6,6};
    case ':': return {0,6,6,0,6,6,0}; case '/': return {1,2,2,4,8,8,16};
    case '[': return {14,8,8,8,8,8,14}; case ']': return {14,2,2,2,2,2,14};
    case '>': return {16,8,4,2,4,8,16}; case '+': return {0,4,4,31,4,4,0};
    case '=': return {0,0,31,0,31,0,0}; case '_': return {0,0,0,0,0,0,31};
    case ' ': return {};
    default: return {14,17,1,2,4,0,4};
    }
}

[[nodiscard]] std::string distanceText(const std::optional<double> distance) {
    if (!distance) return "-";
    if (*distance < 1'000.0) return std::format("{:.1f} M", *distance);
    return std::format("{:.2f} KM", *distance / 1'000.0);
}

[[nodiscard]] std::string_view debugViewName(const std::uint32_t view) noexcept {
    constexpr std::array names{
        std::string_view{"FINAL"}, std::string_view{"LOD"}, std::string_view{"ELEVATION"},
        std::string_view{"SLOPE"}, std::string_view{"CURVATURE"}, std::string_view{"WETNESS"},
        std::string_view{"FOREST"}, std::string_view{"ROCK"}, std::string_view{"SCREE"},
        std::string_view{"SNOW"}, std::string_view{"NORMALS"}, std::string_view{"FOG"},
        std::string_view{"TREE LOD"}, std::string_view{"SHADOW CASCADES"},
    };
    return names[std::min<std::size_t>(view, names.size() - 1U)];
}

} // namespace

HudRasterizer::HudRasterizer()
    : pixels_(static_cast<std::size_t>(Width) * Height * 4U, 0U) {}

void HudRasterizer::render(const HudState& state) {
    clear();
    constexpr Color primary{248, 250, 247, 242};
    constexpr Color secondary{235, 240, 234, 210};
    constexpr Color panel{8, 15, 19, 212};
    constexpr Color border{238, 242, 236, 92};
    constexpr Color shadow{0, 0, 0, 205};

    drawText(17, 13, "TERRAIN STREAM", 2, shadow);
    drawText(16, 12, "TERRAIN STREAM", 2, primary);
    drawText(16, 30, std::format("SEED {}", state.seed), 1, secondary);

    fillRect(799, 9, 153, 27, panel);
    strokeRect(799, 9, 153, 27, border);
    drawText(810, 19, "RANDOM TERRAIN", 1, primary);
    strokeRect(931, 15, 13, 13, border);
    drawText(935, 18, "R", 1, primary);

    fillRect(8, 466, 438, 66, Color{3, 8, 11, 132});
    fillRect(612, 450, 340, 82, Color{3, 8, 11, 132});
    const auto mode = state.flying ? "F WALK" : "F FLY";
    drawText(16, 479, std::format("WASD MOVE  SPACE JUMP  SHIFT RUN  {}", mode), 1, secondary);
    if (state.flying) drawText(16, 493, std::format("C DESCEND  WHEEL {:.0f} M/S  1-6 CAMERA", state.flySpeed), 1, secondary);
    else drawText(16, 493, "1 EYE  2 CANOPY  3 3KM  4 30KM  5 NADIR  6 HORIZON", 1, secondary);
    drawText(16, 507, "G LOG  V VIEW  K CLOUDS  T TUNE  [ ] SET  P SAVE", 1, secondary);

    drawTextRight(944, 454, std::format("NET {:.0f} DEC {:.1f} DER {:.1f} VEG {:.1f} MS",
        state.streamFetchMilliseconds, state.streamDecodeMilliseconds,
        state.streamDerivedMilliseconds, state.streamVegetationMilliseconds), 1, secondary);
    drawTextRight(944, 466, std::format("UPLOAD {:.1f} MS  CPU {:.0f}+{:.0f} MB",
        state.gpuUploadMilliseconds, state.terrainMemoryMegabytes,
        state.vegetationMemoryMegabytes), 1, secondary);
    drawTextRight(944, 478, "HUMAN SCALE - EYE 1.68 M - 1 UNIT = 1 M", 1, secondary);
    drawTextRight(944, 492, std::format("X {:.0f} M   Y {:.0f} M   Z {:.0f} M", state.position.x, state.position.y, state.position.z), 1, secondary);
    const auto farText = state.pendingTiles > 0
        ? std::format("FAR GEOMETRY {:.0f} > {:.0f} KM LOADING", state.installedRadius / 1'000.0, state.requestedRadius / 1'000.0)
        : std::format("FAR GEOMETRY {:.0f} KM", state.installedRadius / 1'000.0);
    drawTextRight(944, 506, farText, 1, secondary);
    drawTextRight(944, 520, std::format("{:.0f} FPS  1P {:.0f}  {} DRAWS  {} TERRAIN  {} TREES  {} GRASS",
        state.fps, state.onePercentLowFps, state.drawCalls, state.terrainDraws, state.trees, state.grass), 1, secondary);
    if (state.debugView != 0U) {
        fillRect(16, 42, 126, 18, panel);
        drawText(23, 48, std::format("VIEW {}", debugViewName(state.debugView)), 1, primary);
    }
    if (!state.cloudsEnabled) {
        fillRect(150, 42, 104, 18, panel);
        drawText(157, 48, "CLOUDS OFF", 1, primary);
    }
    if (!state.tuningLabel.empty()) {
        fillRect(262, 42, 268, 18, panel);
        drawText(269, 48, state.tuningLabel, 1, primary);
    }

    if (state.captured) {
        fillRect(477, 269, 7, 1, primary);
        fillRect(480, 266, 1, 7, primary);
        drawTextCentered(480, 280, distanceText(state.aimDistance), 1, primary);
    } else {
        fillRect(370, 233, 220, 63, panel);
        strokeRect(370, 233, 220, 63, border);
        drawTextCentered(480, 249, state.ready ? "ENTER WORLD" : "GENERATING TERRAIN", 2, primary);
        drawTextCentered(480, 274, state.message, 1, secondary);
    }

    const auto total = state.loadedTiles + state.pendingTiles;
    const auto progress = total == 0 ? 0.0 : static_cast<double>(state.loadedTiles) / static_cast<double>(total);
    fillRect(0, 538, static_cast<std::int32_t>(Width), 2, Color{255, 255, 255, 28});
    fillRect(0, 538, static_cast<std::int32_t>(std::round(progress * Width)), 2, Color{245, 246, 241, 205});
}

void HudRasterizer::clear() noexcept {
    std::fill(pixels_.begin(), pixels_.end(), 0U);
}

void HudRasterizer::blendPixel(std::int32_t x, std::int32_t y, Color color) noexcept {
    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(Width) || y >= static_cast<std::int32_t>(Height)) return;
    const auto index = static_cast<std::size_t>((y * static_cast<std::int32_t>(Width) + x) * 4);
    const auto alpha = static_cast<std::uint32_t>(color.a);
    const auto inverse = 255U - alpha;
    pixels_[index] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.r) * alpha + pixels_[index] * inverse) / 255U);
    pixels_[index + 1U] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.g) * alpha + pixels_[index + 1U] * inverse) / 255U);
    pixels_[index + 2U] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.b) * alpha + pixels_[index + 2U] * inverse) / 255U);
    pixels_[index + 3U] = static_cast<std::uint8_t>(std::min(255U, alpha + static_cast<std::uint32_t>(pixels_[index + 3U]) * inverse / 255U));
}

void HudRasterizer::fillRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, Color color) noexcept {
    for (std::int32_t py = std::max(y, 0); py < std::min(y + height, static_cast<std::int32_t>(Height)); ++py)
        for (std::int32_t px = std::max(x, 0); px < std::min(x + width, static_cast<std::int32_t>(Width)); ++px)
            blendPixel(px, py, color);
}

void HudRasterizer::strokeRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, Color color) noexcept {
    fillRect(x, y, width, 1, color); fillRect(x, y + height - 1, width, 1, color);
    fillRect(x, y, 1, height, color); fillRect(x + width - 1, y, 1, height, color);
}

void HudRasterizer::drawText(std::int32_t x, std::int32_t y, std::string_view text, std::int32_t scale, Color color) noexcept {
    auto cursor = x;
    for (const char character : text) {
        const auto rows = glyph(character);
        for (std::int32_t row = 0; row < 7; ++row) {
            for (std::int32_t column = 0; column < 5; ++column) {
                if ((rows[static_cast<std::size_t>(row)] & (1U << (4 - column))) != 0U)
                    fillRect(cursor + column * scale, y + row * scale, scale, scale, color);
            }
        }
        cursor += 6 * scale;
    }
}

void HudRasterizer::drawTextRight(std::int32_t right, std::int32_t y, std::string_view text, std::int32_t scale, Color color) noexcept {
    drawText(right - textWidth(text, scale), y, text, scale, color);
}

void HudRasterizer::drawTextCentered(std::int32_t center, std::int32_t y, std::string_view text, std::int32_t scale, Color color) noexcept {
    drawText(center - textWidth(text, scale) / 2, y, text, scale, color);
}

std::int32_t HudRasterizer::textWidth(std::string_view text, std::int32_t scale) noexcept {
    return text.empty() ? 0 : static_cast<std::int32_t>(text.size()) * 6 * scale - scale;
}

} // namespace terrain
