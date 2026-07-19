#pragma once

#include "VisualConfig.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace terrain {

struct Config final {
    std::string serverUrl{"http://127.0.0.1:8000"};
    std::uint32_t windowWidth{1600};
    std::uint32_t windowHeight{900};
    std::uint32_t maxUploadsPerFrame{2};
    std::uint32_t shadowResolution{2048};
    float verticalFovDegrees{60.0F};
    float eyeHeight{1.68F};
    float walkSpeed{1.45F};
    float runSpeed{5.5F};
    float jumpSpeed{5.2F};
    float initialFlySpeed{24.0F};
    float grassDistance{190.0F};
    float treeDistance{6'200.0F};
    bool gpuDebug{false};
    std::filesystem::path visualConfigPath{"native/config/world_visuals.json"};
    WorldVisualConfig visuals;

    [[nodiscard]] static Config fromArgs(int argc, char** argv) {
        Config result;
        for (int index = 1; index < argc; ++index) {
            const std::string_view arg{argv[index]};
            const auto next = [&]() -> std::string {
                if (index + 1 >= argc) {
                    throw std::runtime_error("Missing value after " + std::string{arg});
                }
                return argv[++index];
            };
            if (arg == "--server") result.serverUrl = next();
            else if (arg == "--width") result.windowWidth = static_cast<std::uint32_t>(std::stoul(next()));
            else if (arg == "--height") result.windowHeight = static_cast<std::uint32_t>(std::stoul(next()));
            else if (arg == "--gpu-debug") result.gpuDebug = true;
            else if (arg == "--visual-config") {
                result.visualConfigPath = next();
                result.visuals = WorldVisualConfig::load(result.visualConfigPath);
            }
            else if (arg == "--help" || arg == "-h") {
                throw std::runtime_error(
                    "usage: terrain_native [--server http://127.0.0.1:8000] "
                    "[--width 1600] [--height 900] [--visual-config native/config/world_visuals.json] [--gpu-debug]");
            } else {
                throw std::runtime_error("Unknown argument: " + std::string{arg});
            }
        }
        result.windowWidth = std::clamp(result.windowWidth, 640U, 7680U);
        result.windowHeight = std::clamp(result.windowHeight, 360U, 4320U);
        return result;
    }
};

} // namespace terrain
