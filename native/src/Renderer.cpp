#include "Renderer.hpp"

#include <SDL3/SDL.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace terrain {
namespace {

struct alignas(16) TerrainVertexUniform final {
    glm::mat4 viewProjection;
    glm::mat4 shadowViewProjection;
    glm::mat4 farShadowViewProjection;
    glm::vec4 tileOffsetAndLevel;
    glm::vec4 cameraLocalAndTime;
    glm::vec4 lodRadiiAndNoiseOriginX;
    glm::vec4 noiseOriginZAndPadding;
};

struct alignas(16) FragmentUniform final {
    glm::vec4 sunDirectionAndIntensity;
    glm::vec4 cameraHeightFogTimeTexel;
    glm::vec4 horizonColor;
    glm::vec4 zenithColor;
    glm::vec4 atmosphereParameters;
    glm::vec4 cloudShadowParameters;
    glm::vec4 skyLightingParameters;
};

struct alignas(16) VegetationVertexUniform final {
    glm::mat4 viewProjection;
    glm::mat4 shadowViewProjection;
    glm::vec4 tileOffsetAndLevel;
    glm::vec4 cameraLocalAndTime;
    glm::vec4 drawParameters;
    glm::vec4 lodRadiiAndType;
};

struct alignas(16) ShadowUniform final {
    glm::mat4 shadowViewProjection;
    glm::vec4 tileOffset;
    glm::vec4 cameraLocal;
};

struct alignas(16) ShadowVegetationUniform final {
    glm::mat4 shadowViewProjection;
    glm::vec4 tileOffset;
    glm::vec4 drawParameters;
    glm::vec4 cameraLocal;
};

struct alignas(16) SkyUniform final {
    glm::vec4 sunDirectionAndTime;
    glm::vec4 horizonColor;
    glm::vec4 zenithColor;
    glm::vec4 cameraPositionAndAltitude;
    glm::vec4 cameraForwardAndTanHalfFov;
    glm::vec4 cameraRightAndAspect;
    glm::vec4 cameraUpAndOrbital;
    glm::vec4 cloudLayer;
    glm::vec4 cloudNoise;
    glm::vec4 cloudLight;
};

struct alignas(16) WaterVertexUniform final {
    glm::mat4 viewProjection;
    glm::vec4 cameraLocalAndTime;
    glm::vec4 waterParameters;
};

struct alignas(16) WaterFragmentUniform final {
    glm::vec4 cameraHeightFogTime;
    glm::vec4 horizonColor;
    glm::vec4 zenithColor;
    glm::vec4 atmosphereParameters;
    glm::vec4 skyLightingParameters;
    glm::vec4 sunDirectionAndIntensity;
    glm::vec4 cloudShadowParameters;
    glm::vec4 cloudShadowProjection;
};

struct alignas(16) ReticleUniform final {
    glm::vec4 screenSizeAndPadding;
};

struct VegetationVertex final {
    glm::vec3 position{};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    std::uint32_t color{0xffffffffU};
    glm::vec2 uv{};
};

[[nodiscard]] float srgbChannelToLinear(float value) noexcept {
    return value <= 0.04045F
        ? value / 12.92F
        : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] glm::vec3 authoredSrgbToLinear(const glm::vec3 color) noexcept {
    return {
        srgbChannelToLinear(color.r),
        srgbChannelToLinear(color.g),
        srgbChannelToLinear(color.b),
    };
}

void appendTreeCard(
    std::vector<VegetationVertex>& vertices,
    std::vector<std::uint32_t>& indices,
    float angle) {
    const auto right = glm::vec3{std::cos(angle), 0.0F, std::sin(angle)};
    const auto normal = glm::normalize(glm::vec3{-right.z, 0.24F, right.x});
    const auto start = static_cast<std::uint32_t>(vertices.size());
    vertices.insert(vertices.end(), {
        VegetationVertex{-right * 0.5F, normal, 0xffffffffU, {0.0F, 1.0F}},
        VegetationVertex{right * 0.5F, normal, 0xffffffffU, {1.0F, 1.0F}},
        VegetationVertex{right * 0.5F + glm::vec3{0.0F, 1.0F, 0.0F}, normal, 0xffffffffU, {1.0F, 0.0F}},
        VegetationVertex{-right * 0.5F + glm::vec3{0.0F, 1.0F, 0.0F}, normal, 0xffffffffU, {0.0F, 0.0F}},
    });
    indices.insert(indices.end(), {start, start + 1U, start + 2U, start, start + 2U, start + 3U});
}

[[nodiscard]] std::pair<std::vector<VegetationVertex>, std::vector<std::uint32_t>> makeTreeMesh(bool crossed) {
    std::vector<VegetationVertex> vertices;
    std::vector<std::uint32_t> indices;
    appendTreeCard(vertices, indices, 0.0F);
    if (crossed) appendTreeCard(vertices, indices, glm::half_pi<float>());
    return {std::move(vertices), std::move(indices)};
}

[[nodiscard]] float staticRandom(std::uint32_t value) noexcept {
    auto bits = value * 747796405U + 2891336453U;
    bits = ((bits >> ((bits >> 28U) + 4U)) ^ bits) * 277803737U;
    return static_cast<float>((bits >> 8U) & 0x00ffffffU) / static_cast<float>(0x01000000U);
}

[[nodiscard]] std::pair<std::vector<VegetationVertex>, std::vector<std::uint32_t>> makeGrassMesh() {
    std::vector<VegetationVertex> vertices;
    std::vector<std::uint32_t> indices;
    constexpr std::uint32_t bladeCount = 64;
    vertices.reserve(bladeCount * 3U);
    indices.reserve(bladeCount * 3U);
    for (std::uint32_t blade = 0; blade < bladeCount; ++blade) {
        const auto distributionAngle = static_cast<float>(blade) * 2.399963F + staticRandom(blade + 1U) * 0.7F;
        // Grass placement cells are 3.2 m apart. Fill and slightly overlap
        // that footprint so the static instanced clusters merge into a field
        // instead of revealing isolated circular tufts. This changes no draw
        // count: each accepted ecology sample still emits one 64-blade clump.
        const auto distributionRadius = std::sqrt((static_cast<float>(blade) + 0.35F)
            / static_cast<float>(bladeCount)) * 1.78F;
        const auto center = glm::vec3{std::cos(distributionAngle) * distributionRadius, 0.0F,
            std::sin(distributionAngle) * distributionRadius};
        const auto facing = staticRandom(blade + 23U) * glm::pi<float>();
        const auto halfWidth = glm::mix(0.012F, 0.034F, staticRandom(blade + 47U));
        const auto height = glm::mix(0.38F, 0.82F, staticRandom(blade + 79U));
        const auto side = glm::vec3{std::cos(facing) * halfWidth, 0.0F, std::sin(facing) * halfWidth};
        const auto bend = glm::mix(-0.17F, 0.17F, staticRandom(blade + 113U));
        const auto tip = center + glm::vec3{std::cos(facing + glm::half_pi<float>()) * bend, height,
            std::sin(facing + glm::half_pi<float>()) * bend};
        const auto normal = glm::normalize(glm::cross((center + side) - (center - side), tip - (center - side)));
        const auto shade = staticRandom(blade + 149U);
        const auto baseColor = static_cast<std::uint32_t>(180U + static_cast<std::uint32_t>(shade * 60.0F));
        const auto packed = baseColor | (baseColor << 8U) | (baseColor << 16U) | 0xff000000U;
        const auto start = static_cast<std::uint32_t>(vertices.size());
        vertices.insert(vertices.end(), {
            VegetationVertex{center - side, normal, packed, {0.0F, 1.0F}},
            VegetationVertex{center + side, normal, packed, {1.0F, 1.0F}},
            VegetationVertex{tip, normal, 0xffffffffU, {0.5F, 0.0F}},
        });
        indices.insert(indices.end(), {start, start + 1U, start + 2U});
    }
    return {std::move(vertices), std::move(indices)};
}

[[nodiscard]] std::pair<std::vector<TerrainVertex>, std::vector<std::uint32_t>> makeWaterMesh() {
    // Concentrating a camera-relative flat grid around zero gives the water
    // metre-scale geometry near the viewer and progressively cheaper cells
    // toward the view-distance boundary, where atmosphere hides their size.
    constexpr std::uint32_t segments = 128;
    constexpr std::uint32_t row = segments + 1U;
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(static_cast<std::size_t>(row) * row);
    indices.reserve(static_cast<std::size_t>(segments) * segments * 6U);
    const auto warp = [](float coordinate) {
        return std::copysign(std::pow(std::abs(coordinate), 2.7F), coordinate);
    };
    for (std::uint32_t z = 0; z <= segments; ++z) {
        const auto normalZ = static_cast<float>(z) / static_cast<float>(segments) * 2.0F - 1.0F;
        for (std::uint32_t x = 0; x <= segments; ++x) {
            const auto normalX = static_cast<float>(x) / static_cast<float>(segments) * 2.0F - 1.0F;
            vertices.push_back(TerrainVertex{{warp(normalX), 0.0F, warp(normalZ)}});
        }
    }
    for (std::uint32_t z = 0; z < segments; ++z) {
        for (std::uint32_t x = 0; x < segments; ++x) {
            const auto a = z * row + x;
            const auto b = a + 1U;
            const auto c = a + row;
            const auto d = c + 1U;
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    return {std::move(vertices), std::move(indices)};
}

[[nodiscard]] glm::mat4 makeShadowMatrix(
    const CameraState& camera,
    const glm::vec3 sunDirection,
    float radius,
    float resolution) {
    const auto center = glm::vec3{camera.localPosition.x, std::max(camera.localPosition.y - 200.0F, 0.0F), camera.localPosition.z};
    const auto forward = glm::normalize(sunDirection);
    const auto right = glm::normalize(glm::cross(glm::vec3{0.0F, 1.0F, 0.0F}, forward));
    const auto up = glm::cross(forward, right);
    // Snap the ortho window to a world-fixed texel grid. A window that tracks
    // the camera continuously resamples the height field every frame, so
    // shadow terminators crawl and swim as the player moves. Snapping in
    // whole-texel increments keeps the shadow map stationary in the world
    // between steps.
    const float texel = radius * 2.0F / std::max(resolution, 1.0F);
    const glm::vec3 lightSpace{
        glm::dot(center, right), glm::dot(center, up), glm::dot(center, forward)};
    const glm::vec3 snapped = glm::round(lightSpace / texel) * texel;
    const auto target = right * snapped.x + up * snapped.y + forward * snapped.z;
    const auto eye = target - forward * (radius * 2.2F);
    const auto view = glm::lookAtLH(eye, target, glm::vec3{0.0F, 1.0F, 0.0F});
    const auto projection = glm::orthoLH_ZO(-radius, radius, -radius, radius, 50.0F, radius * 4.8F);
    return projection * view;
}

} // namespace

Renderer::Renderer(Config config) : config_(std::move(config)) {}

Renderer::~Renderer() {
    shutdown();
}

void Renderer::shutdown() noexcept {
    if (device_ != nullptr) SDL_WaitForGPUIdle(device_);
    clearTiles();
    releaseMesh(nearTreeMesh_);
    releaseMesh(farTreeMesh_);
    releaseMesh(grassMesh_);
    releaseMesh(waterMesh_);
    if (terrainPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, terrainPipeline_);
    if (vegetationPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, vegetationPipeline_);
    if (terrainShadowPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, terrainShadowPipeline_);
    if (vegetationShadowPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, vegetationShadowPipeline_);
    if (skyPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, skyPipeline_);
    if (cloudOverlayPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, cloudOverlayPipeline_);
    if (cloudCompositePipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, cloudCompositePipeline_);
    if (waterPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, waterPipeline_);
    if (toneMapPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, toneMapPipeline_);
    if (reticlePipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, reticlePipeline_);
    if (hudPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, hudPipeline_);
    if (shadowSampler_) SDL_ReleaseGPUSampler(device_, shadowSampler_);
    if (vegetationSampler_) SDL_ReleaseGPUSampler(device_, vegetationSampler_);
    if (hudSampler_) SDL_ReleaseGPUSampler(device_, hudSampler_);
    if (sceneSampler_) SDL_ReleaseGPUSampler(device_, sceneSampler_);
    if (cloudSampler_) SDL_ReleaseGPUSampler(device_, cloudSampler_);
    if (treeAtlas_) SDL_ReleaseGPUTexture(device_, treeAtlas_);
    if (hudTexture_) SDL_ReleaseGPUTexture(device_, hudTexture_);
    if (hudTransfer_) SDL_ReleaseGPUTransferBuffer(device_, hudTransfer_);
    if (depthTexture_) SDL_ReleaseGPUTexture(device_, depthTexture_);
    if (sceneColorTexture_) SDL_ReleaseGPUTexture(device_, sceneColorTexture_);
    if (cloudTexture_) SDL_ReleaseGPUTexture(device_, cloudTexture_);
    if (shadowTexture_) SDL_ReleaseGPUTexture(device_, shadowTexture_);
    if (farShadowTexture_) SDL_ReleaseGPUTexture(device_, farShadowTexture_);
    if (device_ && window_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
    if (device_) SDL_DestroyGPUDevice(device_);
    if (shaderCrossInitialized_) SDL_ShaderCross_Quit();
    window_ = nullptr;
    device_ = nullptr;
    depthTexture_ = nullptr;
    sceneColorTexture_ = nullptr;
    cloudTexture_ = nullptr;
    shadowTexture_ = nullptr;
    farShadowTexture_ = nullptr;
    shadowSampler_ = nullptr;
    vegetationSampler_ = nullptr;
    treeAtlas_ = nullptr;
    hudTexture_ = nullptr;
    hudTransfer_ = nullptr;
    hudSampler_ = nullptr;
    sceneSampler_ = nullptr;
    cloudSampler_ = nullptr;
    terrainPipeline_ = nullptr;
    vegetationPipeline_ = nullptr;
    terrainShadowPipeline_ = nullptr;
    vegetationShadowPipeline_ = nullptr;
    skyPipeline_ = nullptr;
    cloudOverlayPipeline_ = nullptr;
    cloudCompositePipeline_ = nullptr;
    waterPipeline_ = nullptr;
    toneMapPipeline_ = nullptr;
    reticlePipeline_ = nullptr;
    hudPipeline_ = nullptr;
    shaderCrossInitialized_ = false;
}

void Renderer::initialize(SDL_Window* window) {
    window_ = window;
    if (!SDL_ShaderCross_Init()) throw std::runtime_error(std::string{"SDL_shadercross initialization failed: "} + SDL_GetError());
    shaderCrossInitialized_ = true;
    device_ = SDL_CreateGPUDevice(SDL_ShaderCross_GetSPIRVShaderFormats(), config_.gpuDebug, nullptr);
    if (!device_) throw std::runtime_error(std::string{"SDL GPU device creation failed: "} + SDL_GetError());
    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        throw std::runtime_error(std::string{"SDL GPU window claim failed: "} + SDL_GetError());
    }
    shaderDirectory_ = std::filesystem::path{SDL_GetBasePath()} / "shaders";
    assetDirectory_ = std::filesystem::path{SDL_GetBasePath()} / "assets";
    createRenderTargets();
    createPipelines();
    createStaticMeshes();
    createHudResources();
}

void Renderer::resize() {
    if (!device_) return;
    SDL_WaitForGPUIdle(device_);
    if (depthTexture_) SDL_ReleaseGPUTexture(device_, depthTexture_);
    if (sceneColorTexture_) SDL_ReleaseGPUTexture(device_, sceneColorTexture_);
    if (cloudTexture_) SDL_ReleaseGPUTexture(device_, cloudTexture_);
    depthTexture_ = nullptr;
    sceneColorTexture_ = nullptr;
    cloudTexture_ = nullptr;
    createRenderTargets();
}

void Renderer::addTile(const TileMeshPtr& tile) {
    removeTile(tile->raw.key);
    GpuTile gpu;
    try {
        gpu.cpu = tile;
        gpu.vertices = uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, tile->vertices.data(), tile->vertices.size() * sizeof(TerrainVertex));
        gpu.indices = uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX, tile->indices.data(), tile->indices.size() * sizeof(std::uint32_t));
        gpu.indexCount = static_cast<std::uint32_t>(tile->indices.size());
        if (!tile->trees.empty()) {
            gpu.trees = uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, tile->trees.data(), tile->trees.size() * sizeof(VegetationInstance));
            gpu.treeCount = static_cast<std::uint32_t>(tile->trees.size());
        }
        if (!tile->grass.empty()) {
            gpu.grass = uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, tile->grass.data(), tile->grass.size() * sizeof(VegetationInstance));
            gpu.grassCount = static_cast<std::uint32_t>(tile->grass.size());
        }
    } catch (...) {
        releaseTile(gpu);
        throw;
    }
    treeCount_ += gpu.treeCount;
    grassCount_ += gpu.grassCount;
    tiles_.emplace(tile->raw.key, std::move(gpu));
}

void Renderer::removeTile(const TileKey& key) {
    const auto iterator = tiles_.find(key);
    if (iterator == tiles_.end()) return;
    treeCount_ -= iterator->second.treeCount;
    grassCount_ -= iterator->second.grassCount;
    releaseTile(iterator->second);
    tiles_.erase(iterator);
}

void Renderer::clearTiles() {
    for (auto& [key, tile] : tiles_) {
        static_cast<void>(key);
        releaseTile(tile);
    }
    tiles_.clear();
    treeCount_ = 0;
    grassCount_ = 0;
}

void Renderer::render(const CameraState& camera, float timeSeconds, const HudState& hud) {
    if (!device_) return;
    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    if (!command) throw std::runtime_error(std::string{"GPU command buffer failed: "} + SDL_GetError());
    SDL_GPUTexture* swapchain{};
    std::uint32_t width{};
    std::uint32_t height{};
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, window_, &swapchain, &width, &height)) {
        throw std::runtime_error(std::string{"Swapchain acquisition failed: "} + SDL_GetError());
    }
    if (!swapchain) {
        SDL_SubmitGPUCommandBuffer(command);
        return;
    }
    if (width != pixelWidth_ || height != pixelHeight_) {
        // A command buffer cannot be cancelled after acquiring a swapchain
        // texture. Submit the empty buffer, then rebuild the depth target.
        SDL_SubmitGPUCommandBuffer(command);
        resize();
        return;
    }
    updateHudTexture(command, hud, timeSeconds);

    const auto sunAzimuth = glm::radians(config_.visuals.lighting.sunAzimuthDegrees);
    const auto sunElevation = glm::radians(config_.visuals.lighting.sunElevationDegrees);
    const glm::vec3 sunDirection = glm::normalize(glm::vec3{
        std::sin(sunAzimuth) * std::cos(sunElevation),
        -std::sin(sunElevation),
        std::cos(sunAzimuth) * std::cos(sunElevation),
    });
    const auto shadowMatrix = makeShadowMatrix(
        camera, sunDirection, 2'200.0F, static_cast<float>(config_.shadowResolution));
    const auto farShadowMatrix = makeShadowMatrix(
        camera, sunDirection, 12'500.0F, static_cast<float>(config_.shadowResolution));
    const auto turbidity = config_.visuals.atmosphere.turbidity;
    // The art palette is authored as ordinary sRGB display colours. Convert
    // once at the boundary so the FP16 scene target contains linear radiance;
    // the final ACES pass is then the only linear-to-display transform.
    const auto horizonLinear = authoredSrgbToLinear({
        glm::mix(0.58F, 0.72F, std::clamp((turbidity - 1.5F) / 5.0F, 0.0F, 1.0F)),
        glm::mix(0.72F, 0.78F, std::clamp((turbidity - 1.5F) / 5.0F, 0.0F, 1.0F)),
        0.86F,
    });
    const auto zenithLinear = authoredSrgbToLinear({0.20F, 0.43F, 0.68F});
    const glm::vec4 horizon{horizonLinear, 1.0F};
    const glm::vec4 zenith{zenithLinear, 1.0F};
    renderedTreeCount_ = 0;
    renderedGrassCount_ = 0;
    renderedDrawCallCount_ = 0;
    renderedTerrainTileCount_ = 0;
    const auto circleVisible = [&camera](const glm::dvec2 center, const float radius) {
        const auto forward = glm::vec2{camera.forward.x, camera.forward.z};
        const auto forwardLength = glm::length(forward);
        // A steeply pitched aircraft/orbital view sees a radial footprint;
        // horizontal cone culling is only valid for ordinary traversal views.
        if (forwardLength < 0.35F) return true;
        const auto delta = glm::vec2{
            static_cast<float>(center.x - camera.absolutePosition.x),
            static_cast<float>(center.y - camera.absolutePosition.z),
        };
        const auto distance = glm::length(delta);
        if (distance <= radius) return true;
        const auto horizontalHalfFov = std::atan(std::tan(camera.verticalFovRadians * 0.5F) * camera.aspect);
        const auto tileHalfAngle = std::asin(std::clamp(radius / distance, 0.0F, 1.0F));
        const auto visibilityHalfAngle = std::min(horizontalHalfFov + tileHalfAngle + 0.14F, glm::pi<float>());
        return glm::dot(delta / distance, forward / forwardLength) >= std::cos(visibilityHalfAngle);
    };
    const auto tileVisible = [&circleVisible](const RawTile& raw) {
        const auto center = glm::dvec2{raw.origin.x, raw.origin.y}
            + glm::dvec2{raw.spec.tileWorldSize * 0.5};
        return circleVisible(center, static_cast<float>(raw.spec.tileWorldSize * 0.72));
    };

    SDL_GPUDepthStencilTargetInfo farShadowTarget{};
    farShadowTarget.texture = farShadowTexture_;
    farShadowTarget.clear_depth = 1.0F;
    farShadowTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    farShadowTarget.store_op = SDL_GPU_STOREOP_STORE;
    farShadowTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    farShadowTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* farShadowPass = SDL_BeginGPURenderPass(command, nullptr, 0, &farShadowTarget);
    SDL_BindGPUGraphicsPipeline(farShadowPass, terrainShadowPipeline_);
    for (const auto& [key, tile] : tiles_) {
        static_cast<void>(key);
        // One continuous caster source only. Rendering L0, L1 and L2 on top
        // of each other z-fights because parent elevations are approximations,
        // corrupting the far shadow map.
        if (tile.cpu->raw.spec.level != 2) continue;
        const auto offset = glm::vec3{
            static_cast<float>(tile.cpu->raw.origin.x - camera.floatingOrigin.x), 0.0F,
            static_cast<float>(tile.cpu->raw.origin.y - camera.floatingOrigin.y),
        };
        if (glm::length(glm::vec2{offset.x - camera.localPosition.x, offset.z - camera.localPosition.z}) > 20'000.0F) continue;
        const ShadowUniform uniform{farShadowMatrix,
            glm::vec4{offset, static_cast<float>(tile.cpu->raw.spec.level)},
            glm::vec4{camera.localPosition, 0.0F}};
        const SDL_GPUBufferBinding vertexBinding{tile.vertices, 0};
        const SDL_GPUBufferBinding indexBinding{tile.indices, 0};
        SDL_PushGPUVertexUniformData(command, 0, &uniform, sizeof(uniform));
        SDL_BindGPUVertexBuffers(farShadowPass, 0, &vertexBinding, 1);
        SDL_BindGPUIndexBuffer(farShadowPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(farShadowPass, tile.indexCount, 1, 0, 0, 0);
        ++renderedDrawCallCount_;
    }
    SDL_EndGPURenderPass(farShadowPass);

    SDL_GPUDepthStencilTargetInfo shadowTarget{};
    shadowTarget.texture = shadowTexture_;
    shadowTarget.clear_depth = 1.0F;
    shadowTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    shadowTarget.store_op = SDL_GPU_STOREOP_STORE;
    shadowTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    shadowTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* shadowPass = SDL_BeginGPURenderPass(command, nullptr, 0, &shadowTarget);
    SDL_BindGPUGraphicsPipeline(shadowPass, terrainShadowPipeline_);
    for (const auto& [key, tile] : tiles_) {
        static_cast<void>(key);
        // The finest detailed source owns the near cascade. Do not overlap it
        // with its L1 parent in depth; the far cascade covers beyond this ring.
        if (tile.cpu->raw.spec.level != 0 || !tile.cpu->raw.spec.castsShadow) continue;
        const auto offset = glm::vec3{
            static_cast<float>(tile.cpu->raw.origin.x - camera.floatingOrigin.x), 0.0F,
            static_cast<float>(tile.cpu->raw.origin.y - camera.floatingOrigin.y),
        };
        if (glm::length(glm::vec2{offset.x - camera.localPosition.x, offset.z - camera.localPosition.z}) > 4'800.0F) continue;
        const ShadowUniform uniform{shadowMatrix,
            glm::vec4{offset, static_cast<float>(tile.cpu->raw.spec.level)},
            glm::vec4{camera.localPosition, 0.0F}};
        const SDL_GPUBufferBinding vertexBinding{tile.vertices, 0};
        const SDL_GPUBufferBinding indexBinding{tile.indices, 0};
        SDL_PushGPUVertexUniformData(command, 0, &uniform, sizeof(uniform));
        SDL_BindGPUVertexBuffers(shadowPass, 0, &vertexBinding, 1);
        SDL_BindGPUIndexBuffer(shadowPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(shadowPass, tile.indexCount, 1, 0, 0, 0);
        ++renderedDrawCallCount_;
    }
    SDL_BindGPUGraphicsPipeline(shadowPass, vegetationShadowPipeline_);
    const SDL_GPUTextureSamplerBinding vegetationShadowBinding{treeAtlas_, vegetationSampler_};
    SDL_BindGPUFragmentSamplers(shadowPass, 0, &vegetationShadowBinding, 1);
    for (const auto& [key, tile] : tiles_) {
        static_cast<void>(key);
        if (camera.altitudeAboveGround > 1'200.0F || !tile.trees
            || tile.cpu->raw.spec.level != 0 || !tile.cpu->raw.spec.castsShadow) continue;
        const auto offset = glm::vec3{
            static_cast<float>(tile.cpu->raw.origin.x - camera.floatingOrigin.x), 0.0F,
            static_cast<float>(tile.cpu->raw.origin.y - camera.floatingOrigin.y),
        };
        const ShadowVegetationUniform uniform{shadowMatrix, glm::vec4{offset, 0.0F},
            glm::vec4{config_.visuals.forest.billboardSizeMultiplier, 0.0F, 0.0F, 0.0F},
            glm::vec4{camera.localPosition, 0.0F}};
        SDL_PushGPUVertexUniformData(command, 0, &uniform, sizeof(uniform));
        const SDL_GPUBufferBinding indexBinding{nearTreeMesh_.indices, 0};
        SDL_BindGPUIndexBuffer(shadowPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        for (const auto& range : tile.cpu->treeRanges) {
            const auto rangeCenter = glm::vec2{offset.x, offset.z} + range.center;
            if (glm::length(rangeCenter - glm::vec2{camera.localPosition.x, camera.localPosition.z})
                > config_.visuals.forest.nearShadowDistanceMeters + range.radius) continue;
            const std::array bindings{
                SDL_GPUBufferBinding{nearTreeMesh_.vertices, 0},
                SDL_GPUBufferBinding{tile.trees, range.offset * static_cast<std::uint32_t>(sizeof(VegetationInstance))},
            };
            SDL_BindGPUVertexBuffers(shadowPass, 0, bindings.data(), static_cast<std::uint32_t>(bindings.size()));
            SDL_DrawGPUIndexedPrimitives(shadowPass, nearTreeMesh_.indexCount, range.count, 0, 0, 0);
            ++renderedDrawCallCount_;
        }
    }
    SDL_EndGPURenderPass(shadowPass);

    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture = sceneColorTexture_;
    colorTarget.clear_color = SDL_FColor{horizon.r, horizon.g, horizon.b, 1.0F};
    colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTarget.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* skyPass = SDL_BeginGPURenderPass(command, &colorTarget, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(skyPass, skyPipeline_);
    const auto cameraUp = glm::normalize(glm::cross(camera.forward, camera.right));
    const auto orbital = std::clamp((static_cast<float>(camera.absolutePosition.y) - 25'000.0F) / 155'000.0F, 0.0F, 1.0F);
    const SkyUniform sky{
        glm::vec4{sunDirection, timeSeconds},
        horizon, zenith,
        glm::vec4{glm::vec3{camera.absolutePosition}, camera.altitudeAboveGround},
        glm::vec4{camera.forward, std::tan(camera.verticalFovRadians * 0.5F)},
        glm::vec4{camera.right, camera.aspect},
        glm::vec4{cameraUp, orbital},
        glm::vec4{config_.visuals.clouds.baseHeightMeters, config_.visuals.clouds.topHeightMeters,
            config_.visuals.clouds.coverage, cloudsEnabled_ ? config_.visuals.clouds.density : 0.0F},
        glm::vec4{config_.visuals.clouds.weatherScaleMeters, config_.visuals.clouds.detailScaleMeters,
            config_.visuals.clouds.windMetresPerSecond, static_cast<float>(config_.visuals.clouds.primarySteps)},
        glm::vec4{static_cast<float>(config_.visuals.clouds.lightSteps), turbidity,
            config_.visuals.lighting.exposure, config_.visuals.lighting.skyIntensity},
    };
    SDL_PushGPUFragmentUniformData(command, 0, &sky, sizeof(sky));
    SDL_DrawGPUPrimitives(skyPass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(skyPass);
    ++renderedDrawCallCount_;

    const auto renderCloudVolume = cloudsEnabled_ && camera.absolutePosition.y < 180'000.0;
    const auto cloudsCompositeInFront = camera.absolutePosition.y
        >= static_cast<double>(config_.visuals.clouds.baseHeightMeters);
    const auto compositeCloudVolume = [&]() {
        SDL_GPUColorTargetInfo cloudTarget{};
        cloudTarget.texture = cloudTexture_;
        cloudTarget.clear_color = SDL_FColor{0.0F, 0.0F, 0.0F, 0.0F};
        cloudTarget.load_op = SDL_GPU_LOADOP_CLEAR;
        cloudTarget.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* cloudPass = SDL_BeginGPURenderPass(command, &cloudTarget, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(cloudPass, cloudOverlayPipeline_);
        SDL_PushGPUFragmentUniformData(command, 0, &sky, sizeof(sky));
        SDL_DrawGPUPrimitives(cloudPass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(cloudPass);
        ++renderedDrawCallCount_;

        SDL_GPUColorTargetInfo cloudCompositeTarget{};
        cloudCompositeTarget.texture = sceneColorTexture_;
        cloudCompositeTarget.load_op = SDL_GPU_LOADOP_LOAD;
        cloudCompositeTarget.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* cloudCompositePass = SDL_BeginGPURenderPass(
            command, &cloudCompositeTarget, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(cloudCompositePass, cloudCompositePipeline_);
        const SDL_GPUTextureSamplerBinding cloudBinding{cloudTexture_, cloudSampler_};
        SDL_BindGPUFragmentSamplers(cloudCompositePass, 0, &cloudBinding, 1);
        SDL_DrawGPUPrimitives(cloudCompositePass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(cloudCompositePass);
        ++renderedDrawCallCount_;
    };

    // From the ground the cloud shell is a background layer, so terrain and
    // vegetation naturally occlude it. Above the shell the same premultiplied
    // result is composited after geometry and correctly lies in the foreground.
    if (renderCloudVolume && !cloudsCompositeInFront) compositeCloudVolume();

    colorTarget.load_op = SDL_GPU_LOADOP_LOAD;
    SDL_GPUDepthStencilTargetInfo depthTarget{};
    depthTarget.texture = depthTexture_;
    depthTarget.clear_depth = 0.0F;
    depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    depthTarget.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &colorTarget, 1, &depthTarget);

    const FragmentUniform fragment{
        glm::vec4{sunDirection, config_.visuals.lighting.sunIntensity},
        glm::vec4{static_cast<float>(camera.absolutePosition.y), static_cast<float>(hud.debugView),
            timeSeconds, 1.0F / static_cast<float>(config_.shadowResolution)},
        horizon, zenith,
        glm::vec4{config_.visuals.atmosphere.distanceDensity, config_.visuals.atmosphere.heightFalloff,
            config_.visuals.atmosphere.baseHeightMeters, config_.visuals.atmosphere.aerialPerspectiveStrength},
        glm::vec4{config_.visuals.clouds.coverage, cloudsEnabled_ ? config_.visuals.clouds.density : 0.0F,
            1.0F / config_.visuals.clouds.weatherScaleMeters, config_.visuals.clouds.windMetresPerSecond},
        glm::vec4{config_.visuals.lighting.skyIntensity, config_.visuals.lighting.exposure,
            turbidity, (config_.visuals.clouds.baseHeightMeters + config_.visuals.clouds.topHeightMeters) * 0.5F},
    };
    const std::array shadowBindings{
        SDL_GPUTextureSamplerBinding{shadowTexture_, shadowSampler_},
        SDL_GPUTextureSamplerBinding{farShadowTexture_, shadowSampler_},
    };
    SDL_BindGPUGraphicsPipeline(pass, terrainPipeline_);
    SDL_BindGPUFragmentSamplers(pass, 0, shadowBindings.data(), static_cast<std::uint32_t>(shadowBindings.size()));
    SDL_PushGPUFragmentUniformData(command, 0, &fragment, sizeof(fragment));
    std::vector<const GpuTile*> terrainDrawOrder;
    terrainDrawOrder.reserve(tiles_.size());
    for (const auto& [key, tile] : tiles_) {
        static_cast<void>(key);
        terrainDrawOrder.push_back(&tile);
    }
    std::sort(terrainDrawOrder.begin(), terrainDrawOrder.end(), [](const GpuTile* left, const GpuTile* right) {
        return left->cpu->raw.spec.level > right->cpu->raw.spec.level;
    });
    // Measure the radius actually covered by GPU-resident children. A parent
    // is excluded only inside this proven footprint, so asynchronous uploads
    // can neither expose a hole nor leave a coarse heightfield above the
    // player. The radius grows naturally as child tiles arrive.
    std::unordered_map<std::int32_t, float> readyInnerRadius;
    const auto pointCoveredAtLevel = [this](std::int32_t level, double worldX, double worldZ) {
        return std::ranges::any_of(tiles_, [=](const auto& entry) {
            const auto& raw = entry.second.cpu->raw;
            return raw.spec.level == level
                && worldX >= raw.origin.x && worldZ >= raw.origin.y
                && worldX <= raw.origin.x + raw.spec.tileWorldSize
                && worldZ <= raw.origin.y + raw.spec.tileWorldSize;
        });
    };
    for (const auto* tilePointer : terrainDrawOrder) {
        const auto& spec = tilePointer->cpu->raw.spec;
        if (spec.level <= 0 || readyInnerRadius.contains(spec.level)) continue;
        if (!pointCoveredAtLevel(spec.level - 1, camera.absolutePosition.x, camera.absolutePosition.z)) {
            readyInnerRadius.emplace(spec.level, 0.0F);
            continue;
        }
        const auto childIterator = std::ranges::find_if(terrainDrawOrder, [&spec](const GpuTile* candidate) {
            return candidate->cpu->raw.spec.level == spec.level - 1;
        });
        if (childIterator == terrainDrawOrder.end()) {
            readyInnerRadius.emplace(spec.level, 0.0F);
            continue;
        }
        const auto step = std::max(32.0, (*childIterator)->cpu->raw.spec.tileWorldSize * 0.20);
        auto coveredRadius = spec.innerRadius;
        constexpr std::int32_t directionCount = 16;
        for (std::int32_t directionIndex = 0; directionIndex < directionCount; ++directionIndex) {
            const auto angle = static_cast<double>(directionIndex) * glm::two_pi<double>()
                / static_cast<double>(directionCount);
            const auto direction = glm::dvec2{std::cos(angle), std::sin(angle)};
            double directionRadius{};
            for (double radius = step; radius <= spec.innerRadius + step; radius += step) {
                const auto point = glm::dvec2{camera.absolutePosition.x, camera.absolutePosition.z} + direction * radius;
                if (!pointCoveredAtLevel(spec.level - 1, point.x, point.y)) break;
                directionRadius = radius;
            }
            coveredRadius = std::min(coveredRadius, directionRadius);
        }
        readyInnerRadius.emplace(spec.level, static_cast<float>(std::clamp(coveredRadius * 0.86, 0.0, spec.innerRadius)));
    }
    for (const auto* tilePointer : terrainDrawOrder) {
        const auto& tile = *tilePointer;
        const auto& raw = tile.cpu->raw;
        if (!tileVisible(raw)) continue;
        const auto offset = glm::vec3{
            static_cast<float>(raw.origin.x - camera.floatingOrigin.x), 0.0F,
            static_cast<float>(raw.origin.y - camera.floatingOrigin.y),
        };
        const auto noiseOriginX = static_cast<float>(camera.floatingOrigin.x);
        const auto noiseOriginZ = static_cast<float>(camera.floatingOrigin.y);
        const auto readyInner = readyInnerRadius.contains(raw.spec.level)
            ? readyInnerRadius.at(raw.spec.level) : 0.0F;
        const TerrainVertexUniform uniform{
            camera.viewProjection, shadowMatrix, farShadowMatrix,
            glm::vec4{offset, static_cast<float>(raw.spec.level)},
            glm::vec4{camera.localPosition.x, camera.localPosition.y, camera.localPosition.z, timeSeconds},
            glm::vec4{readyInner, static_cast<float>(raw.spec.outerRadius), static_cast<float>(raw.spec.level), noiseOriginX},
            glm::vec4{noiseOriginZ, static_cast<float>(raw.spec.tileWorldSize), 0.0F, 0.0F},
        };
        const SDL_GPUBufferBinding vertexBinding{tile.vertices, 0};
        const SDL_GPUBufferBinding indexBinding{tile.indices, 0};
        SDL_PushGPUVertexUniformData(command, 0, &uniform, sizeof(uniform));
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, tile.indexCount, 1, 0, 0, 0);
        ++renderedDrawCallCount_;
        ++renderedTerrainTileCount_;
    }

    SDL_BindGPUGraphicsPipeline(pass, vegetationPipeline_);
    const std::array vegetationBindings{
        SDL_GPUTextureSamplerBinding{shadowTexture_, shadowSampler_},
        SDL_GPUTextureSamplerBinding{treeAtlas_, vegetationSampler_},
    };
    SDL_BindGPUFragmentSamplers(pass, 0, vegetationBindings.data(), static_cast<std::uint32_t>(vegetationBindings.size()));
    SDL_PushGPUFragmentUniformData(command, 0, &fragment, sizeof(fragment));
    const auto altitudeFade = std::clamp((camera.altitudeAboveGround - 1'200.0F) / 6'800.0F, 0.0F, 1.0F);
    // Vertical billboard cards become crosses/stars when viewed from above.
    // At steep nadir angles transition mostly to the continuous forest
    // response already present in the terrain material. The fade is capped so
    // looking straight down never removes every tree at once.
    const auto overheadView = glm::smoothstep(0.52F, 0.88F, -camera.forward.y)
        * glm::smoothstep(45.0F, 120.0F, camera.altitudeAboveGround);
    const auto treeDrawDistance = config_.treeDistance * (1.0F - altitudeFade)
        * (1.0F - overheadView * 0.72F);
    const auto projectionScale = static_cast<float>(pixelHeight_)
        / (2.0F * std::tan(camera.verticalFovRadians * 0.5F));
    const auto drawGrass = camera.altitudeAboveGround < 320.0F;
    for (const auto& [key, tile] : tiles_) {
        static_cast<void>(key);
        if (!tileVisible(tile.cpu->raw)) continue;
        const auto offset = glm::vec3{
            static_cast<float>(tile.cpu->raw.origin.x - camera.floatingOrigin.x), 0.0F,
            static_cast<float>(tile.cpu->raw.origin.y - camera.floatingOrigin.y),
        };
        const auto tileDistance = glm::length(glm::vec2{offset.x - camera.localPosition.x, offset.z - camera.localPosition.z});
        if (treeDrawDistance > 1.0F && tile.trees
            && tileDistance < treeDrawDistance + static_cast<float>(tile.cpu->raw.spec.tileWorldSize)) {
            const auto farTrees = tile.cpu->raw.spec.level > 0;
            const auto innerRadius = farTrees ? std::max(1'050.0F, static_cast<float>(tile.cpu->raw.spec.innerRadius)) : 0.0F;
            const auto outerRadius = farTrees
                ? std::min(treeDrawDistance, static_cast<float>(tile.cpu->raw.spec.outerRadius))
                : std::min(1'180.0F, static_cast<float>(tile.cpu->raw.spec.outerRadius));
            for (const auto& range : tile.cpu->treeRanges) {
                const auto rangeCenter = glm::dvec2{tile.cpu->raw.origin.x, tile.cpu->raw.origin.y}
                    + glm::dvec2{range.center};
                const auto rangeDistance = static_cast<float>(glm::length(
                    rangeCenter - glm::dvec2{camera.absolutePosition.x, camera.absolutePosition.z}));
                if (rangeDistance > outerRadius + range.radius
                    || rangeDistance + range.radius < innerRadius
                    || !circleVisible(rangeCenter, range.radius)) continue;
                const auto projectedHeight = 18.0F * projectionScale / std::max(rangeDistance, 1.0F);
                const auto useCrossedCards = !farTrees && overheadView < 0.28F
                    && projectedHeight >= config_.visuals.forest.nearPixelThreshold;
                const auto& treeMesh = useCrossedCards ? nearTreeMesh_ : farTreeMesh_;
                const VegetationVertexUniform uniform{
                    camera.viewProjection, shadowMatrix,
                    glm::vec4{offset, static_cast<float>(tile.cpu->raw.spec.level)},
                    glm::vec4{camera.localPosition.x, camera.localPosition.y, camera.localPosition.z, timeSeconds},
                    glm::vec4{config_.visuals.forest.billboardSizeMultiplier, 0.22F, projectionScale,
                        config_.visuals.forest.geometryCullPixelThreshold},
                    glm::vec4{innerRadius, outerRadius, useCrossedCards ? 0.0F : 2.0F,
                        config_.visuals.forest.midPixelThreshold},
                };
                SDL_PushGPUVertexUniformData(command, 0, &uniform, sizeof(uniform));
                const SDL_GPUBufferBinding indexBinding{treeMesh.indices, 0};
                SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                const std::array bindings{
                    SDL_GPUBufferBinding{treeMesh.vertices, 0},
                    SDL_GPUBufferBinding{tile.trees,
                        range.offset * static_cast<std::uint32_t>(sizeof(VegetationInstance))},
                };
                SDL_BindGPUVertexBuffers(pass, 0, bindings.data(), static_cast<std::uint32_t>(bindings.size()));
                SDL_DrawGPUIndexedPrimitives(pass, treeMesh.indexCount, range.count, 0, 0, 0);
                ++renderedDrawCallCount_;
                renderedTreeCount_ += range.count;
            }
        }
        if (drawGrass && tile.grass
            && tileDistance < config_.grassDistance + static_cast<float>(tile.cpu->raw.spec.tileWorldSize)) {
            const VegetationVertexUniform uniform{
                camera.viewProjection, shadowMatrix,
                glm::vec4{offset, 0.0F},
                glm::vec4{camera.localPosition.x, camera.localPosition.y, camera.localPosition.z, timeSeconds},
                glm::vec4{1.65F, 0.12F, config_.grassDistance, 1.0F},
                glm::vec4{0.0F, config_.grassDistance, 1.0F, 0.0F},
            };
            SDL_PushGPUVertexUniformData(command, 0, &uniform, sizeof(uniform));
            const SDL_GPUBufferBinding indexBinding{grassMesh_.indices, 0};
            SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            for (const auto& range : tile.cpu->grassRanges) {
                const auto rangeCenter = glm::dvec2{tile.cpu->raw.origin.x, tile.cpu->raw.origin.y}
                    + glm::dvec2{range.center};
                const auto rangeDistance = static_cast<float>(glm::length(
                    rangeCenter - glm::dvec2{camera.absolutePosition.x, camera.absolutePosition.z}));
                if (rangeDistance > config_.grassDistance + range.radius
                    || !circleVisible(rangeCenter, range.radius)) continue;
                const std::array bindings{
                    SDL_GPUBufferBinding{grassMesh_.vertices, 0},
                    SDL_GPUBufferBinding{tile.grass,
                        range.offset * static_cast<std::uint32_t>(sizeof(VegetationInstance))},
                };
                SDL_BindGPUVertexBuffers(pass, 0, bindings.data(), static_cast<std::uint32_t>(bindings.size()));
                SDL_DrawGPUIndexedPrimitives(pass, grassMesh_.indexCount, range.count, 0, 0, 0);
                ++renderedDrawCallCount_;
                renderedGrassCount_ += range.count;
            }
        }
    }

    SDL_BindGPUGraphicsPipeline(pass, waterPipeline_);
    // The flat ocean follows the floating origin and covers the same fixed
    // visibility budget as terrain. It never shrinks with camera altitude.
    constexpr float waterRadius = 240'000.0F;
    const WaterVertexUniform waterVertex{
        camera.viewProjection,
        glm::vec4{camera.localPosition.x, camera.localPosition.y, camera.localPosition.z, timeSeconds},
        glm::vec4{waterRadius, -0.08F, 0.0F, 0.0F},
    };
    const WaterFragmentUniform waterFragment{
        glm::vec4{static_cast<float>(camera.absolutePosition.y), camera.localPosition.x,
            timeSeconds, camera.localPosition.z}, horizon, zenith,
        glm::vec4{config_.visuals.atmosphere.distanceDensity, config_.visuals.atmosphere.heightFalloff,
            config_.visuals.atmosphere.baseHeightMeters, config_.visuals.atmosphere.aerialPerspectiveStrength},
        glm::vec4{config_.visuals.lighting.skyIntensity, config_.visuals.lighting.exposure, turbidity, 0.0F},
        glm::vec4{sunDirection, config_.visuals.lighting.sunIntensity},
        glm::vec4{config_.visuals.clouds.coverage, cloudsEnabled_ ? config_.visuals.clouds.density : 0.0F,
            1.0F / config_.visuals.clouds.weatherScaleMeters, config_.visuals.clouds.windMetresPerSecond},
        glm::vec4{static_cast<float>(camera.floatingOrigin.x), static_cast<float>(camera.floatingOrigin.y),
            (config_.visuals.clouds.baseHeightMeters + config_.visuals.clouds.topHeightMeters) * 0.5F, 0.0F},
    };
    SDL_PushGPUVertexUniformData(command, 0, &waterVertex, sizeof(waterVertex));
    SDL_PushGPUFragmentUniformData(command, 0, &waterFragment, sizeof(waterFragment));
    const SDL_GPUBufferBinding waterVertexBinding{waterMesh_.vertices, 0};
    const SDL_GPUBufferBinding waterIndexBinding{waterMesh_.indices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &waterVertexBinding, 1);
    SDL_BindGPUIndexBuffer(pass, &waterIndexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(pass, waterMesh_.indexCount, 1, 0, 0, 0);
    ++renderedDrawCallCount_;

    SDL_EndGPURenderPass(pass);

    if (renderCloudVolume && cloudsCompositeInFront) compositeCloudVolume();

    SDL_GPUColorTargetInfo finalTarget{};
    finalTarget.texture = swapchain;
    finalTarget.clear_color = SDL_FColor{0.0F, 0.0F, 0.0F, 1.0F};
    finalTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    finalTarget.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* finalPass = SDL_BeginGPURenderPass(command, &finalTarget, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(finalPass, toneMapPipeline_);
    const SDL_GPUTextureSamplerBinding sceneBinding{sceneColorTexture_, sceneSampler_};
    SDL_BindGPUFragmentSamplers(finalPass, 0, &sceneBinding, 1);
    SDL_DrawGPUPrimitives(finalPass, 3, 1, 0, 0);
    ++renderedDrawCallCount_;

    SDL_BindGPUGraphicsPipeline(finalPass, hudPipeline_);
    const SDL_GPUTextureSamplerBinding hudBinding{hudTexture_, hudSampler_};
    SDL_BindGPUFragmentSamplers(finalPass, 0, &hudBinding, 1);
    SDL_DrawGPUPrimitives(finalPass, 3, 1, 0, 0);
    ++renderedDrawCallCount_;
    SDL_EndGPURenderPass(finalPass);
    SDL_SubmitGPUCommandBuffer(command);
}

const char* Renderer::driverName() const noexcept {
    return device_ ? SDL_GetGPUDeviceDriver(device_) : "uninitialized";
}

void Renderer::createRenderTargets() {
    int width{};
    int height{};
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    pixelWidth_ = static_cast<std::uint32_t>(std::max(width, 1));
    pixelHeight_ = static_cast<std::uint32_t>(std::max(height, 1));
    const SDL_GPUTextureCreateInfo depthTextureInfo{
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = depthFormat_,
        .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
        .width = pixelWidth_, .height = pixelHeight_, .layer_count_or_depth = 1, .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    depthTexture_ = SDL_CreateGPUTexture(device_, &depthTextureInfo);
    const SDL_GPUTextureCreateInfo sceneTextureInfo{
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = pixelWidth_, .height = pixelHeight_, .layer_count_or_depth = 1, .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    sceneColorTexture_ = SDL_CreateGPUTexture(device_, &sceneTextureInfo);
    const SDL_GPUTextureCreateInfo cloudTextureInfo{
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        // One-sixteenth of the full-resolution fragment count. Clouds are
        // naturally band-limited, and linear upscale hides the reduced grid.
        .width = std::max(pixelWidth_ / 4U, 1U), .height = std::max(pixelHeight_ / 4U, 1U),
        .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    cloudTexture_ = SDL_CreateGPUTexture(device_, &cloudTextureInfo);
    if (!sceneSampler_ || !cloudSampler_) {
        const SDL_GPUSamplerCreateInfo sceneSamplerInfo{
            .min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR,
            .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
            .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        };
        if (!sceneSampler_) sceneSampler_ = SDL_CreateGPUSampler(device_, &sceneSamplerInfo);
        if (!cloudSampler_) cloudSampler_ = SDL_CreateGPUSampler(device_, &sceneSamplerInfo);
    }
    if (!depthTexture_ || !sceneColorTexture_ || !cloudTexture_ || !sceneSampler_ || !cloudSampler_) {
        throw std::runtime_error(std::string{"Scene render-target creation failed: "} + SDL_GetError());
    }
    if (!shadowTexture_) {
        const SDL_GPUTextureCreateInfo shadowTextureInfo{
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = depthFormat_,
            .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = config_.shadowResolution, .height = config_.shadowResolution,
            .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1,
        };
        shadowTexture_ = SDL_CreateGPUTexture(device_, &shadowTextureInfo);
        const SDL_GPUTextureCreateInfo farShadowTextureInfo{
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = depthFormat_,
            .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = config_.shadowResolution, .height = config_.shadowResolution,
            .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1,
        };
        farShadowTexture_ = SDL_CreateGPUTexture(device_, &farShadowTextureInfo);
        const SDL_GPUSamplerCreateInfo shadowSamplerInfo{
            .min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR,
            .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
            .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        };
        shadowSampler_ = SDL_CreateGPUSampler(device_, &shadowSamplerInfo);
        if (!shadowTexture_ || !farShadowTexture_ || !shadowSampler_) throw std::runtime_error(std::string{"Shadow resource creation failed: "} + SDL_GetError());
    }
}

void Renderer::createPipelines() {
    SDL_GPUShader* terrainVertex = compileShader("terrain.vert.hlsl", SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader* terrainFragment = compileShader("terrain.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader* vegetationVertex = compileShader("vegetation.vert.hlsl", SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader* vegetationFragment = compileShader("vegetation.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader* shadowTerrainVertex = compileShader("shadow_terrain.vert.hlsl", SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader* shadowVegetationVertex = compileShader("shadow_vegetation.vert.hlsl", SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader* shadowFragment = compileShader("shadow.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader* shadowVegetationFragment = compileShader("shadow_vegetation.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader* skyVertex = compileShader("sky.vert.hlsl", SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader* skyFragment = compileShader("sky.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader* cloudOverlayFragment = compileShader("cloud_overlay.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader* cloudCompositeFragment = compileShader("cloud_composite.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader* waterVertex = compileShader("water.vert.hlsl", SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader* waterFragment = compileShader("water.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader* toneMapFragment = compileShader("tone_map.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader* reticleFragment = compileShader("reticle.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader* hudFragment = compileShader("hud.frag.hlsl", SDL_GPU_SHADERSTAGE_FRAGMENT);

    const auto swapchainFormat = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    const SDL_GPUColorTargetDescription opaqueColor{.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT};
    const SDL_GPUVertexBufferDescription terrainBuffer{
        .slot = 0, .pitch = sizeof(TerrainVertex), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    };
    const std::array terrainAttributes{
        SDL_GPUVertexAttribute{.location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(TerrainVertex, position)},
        SDL_GPUVertexAttribute{.location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(TerrainVertex, normal)},
        SDL_GPUVertexAttribute{.location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = offsetof(TerrainVertex, color)},
        SDL_GPUVertexAttribute{.location = 3, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = offsetof(TerrainVertex, derived)},
        SDL_GPUVertexAttribute{.location = 4, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = offsetof(TerrainVertex, biome)},
    };
    SDL_GPUGraphicsPipelineCreateInfo terrainInfo{
        .vertex_shader = terrainVertex, .fragment_shader = terrainFragment,
        .vertex_input_state = {.vertex_buffer_descriptions = &terrainBuffer, .num_vertex_buffers = 1,
            .vertex_attributes = terrainAttributes.data(), .num_vertex_attributes = static_cast<std::uint32_t>(terrainAttributes.size())},
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
            .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE, .enable_depth_clip = true},
        .depth_stencil_state = {.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, .enable_depth_test = true, .enable_depth_write = true},
        .target_info = {.color_target_descriptions = &opaqueColor, .num_color_targets = 1,
            .depth_stencil_format = depthFormat_, .has_depth_stencil_target = true},
    };
    terrainPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &terrainInfo);

    const std::array vegetationBuffers{
        SDL_GPUVertexBufferDescription{.slot = 0, .pitch = sizeof(VegetationVertex),
            .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX, .instance_step_rate = 0},
        SDL_GPUVertexBufferDescription{.slot = 1, .pitch = sizeof(VegetationInstance),
            .input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE, .instance_step_rate = 0},
    };
    const std::array vegetationAttributes{
        SDL_GPUVertexAttribute{.location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(VegetationVertex, position)},
        SDL_GPUVertexAttribute{.location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(VegetationVertex, normal)},
        SDL_GPUVertexAttribute{.location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = offsetof(VegetationVertex, color)},
        SDL_GPUVertexAttribute{.location = 3, .buffer_slot = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(VegetationInstance, position)},
        SDL_GPUVertexAttribute{.location = 4, .buffer_slot = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, .offset = offsetof(VegetationInstance, scale)},
        SDL_GPUVertexAttribute{.location = 5, .buffer_slot = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = offsetof(VegetationInstance, color)},
        SDL_GPUVertexAttribute{.location = 6, .buffer_slot = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = offsetof(VegetationInstance, parameters)},
        SDL_GPUVertexAttribute{.location = 7, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = offsetof(VegetationVertex, uv)},
    };
    auto vegetationInfo = terrainInfo;
    vegetationInfo.vertex_shader = vegetationVertex;
    vegetationInfo.fragment_shader = vegetationFragment;
    vegetationInfo.vertex_input_state = {.vertex_buffer_descriptions = vegetationBuffers.data(),
        .num_vertex_buffers = static_cast<std::uint32_t>(vegetationBuffers.size()),
        .vertex_attributes = vegetationAttributes.data(),
        .num_vertex_attributes = static_cast<std::uint32_t>(vegetationAttributes.size())};
    vegetationPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &vegetationInfo);

    SDL_GPUGraphicsPipelineCreateInfo terrainShadowInfo{
        .vertex_shader = shadowTerrainVertex, .fragment_shader = shadowFragment,
        .vertex_input_state = terrainInfo.vertex_input_state,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
            .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE, .depth_bias_constant_factor = 1.25F,
            .depth_bias_clamp = 0.0F, .depth_bias_slope_factor = 1.75F, .enable_depth_bias = true,
            .enable_depth_clip = true},
        .depth_stencil_state = {.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL, .enable_depth_test = true, .enable_depth_write = true},
        .target_info = {.depth_stencil_format = depthFormat_, .has_depth_stencil_target = true},
    };
    terrainShadowPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &terrainShadowInfo);
    auto vegetationShadowInfo = terrainShadowInfo;
    vegetationShadowInfo.vertex_shader = shadowVegetationVertex;
    vegetationShadowInfo.fragment_shader = shadowVegetationFragment;
    vegetationShadowInfo.vertex_input_state = vegetationInfo.vertex_input_state;
    vegetationShadowPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &vegetationShadowInfo);

    SDL_GPUGraphicsPipelineCreateInfo skyInfo{
        .vertex_shader = skyVertex, .fragment_shader = skyFragment,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
            .enable_depth_clip = true},
        .target_info = {.color_target_descriptions = &opaqueColor, .num_color_targets = 1},
    };
    skyPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &skyInfo);

    SDL_GPUColorTargetDescription cloudColor{.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT};
    SDL_GPUColorTargetDescription cloudCompositeColor{.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT};
    cloudCompositeColor.blend_state = {
        .src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
        .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .color_blend_op = SDL_GPU_BLENDOP_ADD,
        .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
        .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
        .color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G
            | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A,
        .enable_blend = true,
        .enable_color_write_mask = true,
    };
    auto cloudOverlayInfo = skyInfo;
    cloudOverlayInfo.fragment_shader = cloudOverlayFragment;
    cloudOverlayInfo.depth_stencil_state = {};
    cloudOverlayInfo.target_info = {.color_target_descriptions = &cloudColor, .num_color_targets = 1};
    cloudOverlayPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &cloudOverlayInfo);
    auto cloudCompositeInfo = cloudOverlayInfo;
    cloudCompositeInfo.fragment_shader = cloudCompositeFragment;
    cloudCompositeInfo.target_info.color_target_descriptions = &cloudCompositeColor;
    cloudCompositePipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &cloudCompositeInfo);

    SDL_GPUColorTargetDescription waterColor{.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT};
    waterColor.blend_state = {
        .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
        .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .color_blend_op = SDL_GPU_BLENDOP_ADD,
        .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
        .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
        .color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A,
        .enable_blend = true,
        .enable_color_write_mask = true,
    };
    const SDL_GPUVertexBufferDescription waterBuffer{.slot = 0, .pitch = sizeof(TerrainVertex), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
    const SDL_GPUVertexAttribute waterAttribute{.location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(TerrainVertex, position)};
    SDL_GPUGraphicsPipelineCreateInfo waterInfo{
        .vertex_shader = waterVertex, .fragment_shader = waterFragment,
        .vertex_input_state = {.vertex_buffer_descriptions = &waterBuffer, .num_vertex_buffers = 1,
            .vertex_attributes = &waterAttribute, .num_vertex_attributes = 1},
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
            .enable_depth_clip = true},
        .depth_stencil_state = {.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, .enable_depth_test = true, .enable_depth_write = false},
        .target_info = {.color_target_descriptions = &waterColor, .num_color_targets = 1,
            .depth_stencil_format = depthFormat_, .has_depth_stencil_target = true},
    };
    waterPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &waterInfo);

    SDL_GPUColorTargetDescription overlayColor{.format = swapchainFormat};
    overlayColor.blend_state = waterColor.blend_state;
    auto reticleInfo = skyInfo;
    reticleInfo.fragment_shader = reticleFragment;
    reticleInfo.depth_stencil_state = {};
    reticleInfo.target_info = {.color_target_descriptions = &overlayColor, .num_color_targets = 1};
    reticlePipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &reticleInfo);
    auto hudInfo = reticleInfo;
    hudInfo.fragment_shader = hudFragment;
    hudPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &hudInfo);
    const SDL_GPUColorTargetDescription toneMapColor{.format = swapchainFormat};
    auto toneMapInfo = reticleInfo;
    toneMapInfo.fragment_shader = toneMapFragment;
    toneMapInfo.target_info.color_target_descriptions = &toneMapColor;
    toneMapPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &toneMapInfo);

    const std::array shaders{terrainVertex, terrainFragment, vegetationVertex, vegetationFragment,
        shadowTerrainVertex, shadowVegetationVertex, shadowFragment, shadowVegetationFragment, skyVertex, skyFragment,
        cloudOverlayFragment, cloudCompositeFragment,
        waterVertex, waterFragment, toneMapFragment, reticleFragment, hudFragment};
    for (auto* shader : shaders) SDL_ReleaseGPUShader(device_, shader);
    if (!terrainPipeline_ || !vegetationPipeline_ || !terrainShadowPipeline_ || !vegetationShadowPipeline_
        || !skyPipeline_ || !cloudOverlayPipeline_ || !cloudCompositePipeline_
        || !waterPipeline_ || !toneMapPipeline_
        || !reticlePipeline_ || !hudPipeline_) {
        throw std::runtime_error(std::string{"GPU pipeline creation failed: "} + SDL_GetError());
    }
}

void Renderer::createStaticMeshes() {
    auto [nearTreeVertices, nearTreeIndices] = makeTreeMesh(true);
    nearTreeMesh_ = uploadMesh(nearTreeVertices.data(), nearTreeVertices.size() * sizeof(VegetationVertex), nearTreeIndices);
    auto [farTreeVertices, farTreeIndices] = makeTreeMesh(false);
    farTreeMesh_ = uploadMesh(farTreeVertices.data(), farTreeVertices.size() * sizeof(VegetationVertex), farTreeIndices);
    auto [grassVertices, grassIndices] = makeGrassMesh();
    grassMesh_ = uploadMesh(grassVertices.data(), grassVertices.size() * sizeof(VegetationVertex), grassIndices);
    auto [waterVertices, waterIndices] = makeWaterMesh();
    waterMesh_ = uploadMesh(waterVertices.data(), waterVertices.size() * sizeof(TerrainVertex), waterIndices);
    createTreeAtlas();
}

void Renderer::createTreeAtlas() {
    constexpr std::int32_t cellWidth = 512;
    constexpr std::int32_t atlasHeight = 1024;
    constexpr std::array<std::string_view, 5> files{
        "spruce-v3.png", "birch.png", "oak.png", "acacia.png", "tropical.png",
    };
    constexpr auto atlasWidth = cellWidth * static_cast<std::int32_t>(files.size());
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(atlasWidth) * atlasHeight * 4U, 0U);

    for (std::size_t species = 0; species < files.size(); ++species) {
        const auto path = assetDirectory_ / files[species];
        int sourceWidth{};
        int sourceHeight{};
        int channels{};
        stbi_uc* source = stbi_load(path.string().c_str(), &sourceWidth, &sourceHeight, &channels, 4);
        if (!source) {
            throw std::runtime_error("Could not load tree asset " + path.string() + ": "
                + (stbi_failure_reason() ? stbi_failure_reason() : "unknown PNG error"));
        }
        const auto scale = std::min(
            static_cast<float>(cellWidth - 24) / static_cast<float>(sourceWidth),
            static_cast<float>(atlasHeight - 16) / static_cast<float>(sourceHeight));
        const auto drawWidth = std::max(1, static_cast<int>(std::round(static_cast<float>(sourceWidth) * scale)));
        const auto drawHeight = std::max(1, static_cast<int>(std::round(static_cast<float>(sourceHeight) * scale)));
        const auto offsetX = static_cast<int>(species) * cellWidth + (cellWidth - drawWidth) / 2;
        const auto offsetY = atlasHeight - drawHeight - 4;
        for (int y = 0; y < drawHeight; ++y) {
            const auto sourceY = std::min(sourceHeight - 1, y * sourceHeight / drawHeight);
            for (int x = 0; x < drawWidth; ++x) {
                const auto sourceX = std::min(sourceWidth - 1, x * sourceWidth / drawWidth);
                const auto sourceIndex = static_cast<std::size_t>((sourceY * sourceWidth + sourceX) * 4);
                const auto destinationIndex = static_cast<std::size_t>(((offsetY + y) * atlasWidth + offsetX + x) * 4);
                std::memcpy(pixels.data() + destinationIndex, source + sourceIndex, 4U);
            }
        }
        stbi_image_free(source);
    }

    // Preserve foliage colour in transparent texels so filtered mip levels do
    // not pull black into the silhouette. Alpha remains untouched; only RGB is
    // dilated, keeping alpha testing and depth writes deterministic.
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(atlasWidth) * atlasHeight);
    for (std::size_t pixel = 0; pixel < covered.size(); ++pixel) {
        covered[pixel] = pixels[pixel * 4U + 3U] > 8U ? 1U : 0U;
    }
    for (std::uint32_t pass = 0; pass < 8U; ++pass) {
        auto nextCovered = covered;
        auto nextPixels = pixels;
        for (std::int32_t y = 1; y < atlasHeight - 1; ++y) {
            for (std::int32_t x = 1; x < atlasWidth - 1; ++x) {
                const auto pixel = static_cast<std::size_t>(y * atlasWidth + x);
                if (covered[pixel] != 0U) continue;
                std::uint32_t red{};
                std::uint32_t green{};
                std::uint32_t blue{};
                std::uint32_t samples{};
                for (std::int32_t oy = -1; oy <= 1; ++oy) {
                    for (std::int32_t ox = -1; ox <= 1; ++ox) {
                        if (ox == 0 && oy == 0) continue;
                        const auto neighbour = static_cast<std::size_t>((y + oy) * atlasWidth + x + ox);
                        if (covered[neighbour] == 0U) continue;
                        red += pixels[neighbour * 4U];
                        green += pixels[neighbour * 4U + 1U];
                        blue += pixels[neighbour * 4U + 2U];
                        ++samples;
                    }
                }
                if (samples == 0U) continue;
                nextPixels[pixel * 4U] = static_cast<std::uint8_t>(red / samples);
                nextPixels[pixel * 4U + 1U] = static_cast<std::uint8_t>(green / samples);
                nextPixels[pixel * 4U + 2U] = static_cast<std::uint8_t>(blue / samples);
                nextCovered[pixel] = 1U;
            }
        }
        pixels.swap(nextPixels);
        covered.swap(nextCovered);
    }

    constexpr std::uint32_t mipLevels = 12;
    const SDL_GPUTextureCreateInfo textureInfo{
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
        .width = static_cast<std::uint32_t>(atlasWidth),
        .height = static_cast<std::uint32_t>(atlasHeight),
        .layer_count_or_depth = 1,
        .num_levels = mipLevels,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    treeAtlas_ = SDL_CreateGPUTexture(device_, &textureInfo);
    const SDL_GPUSamplerCreateInfo samplerInfo{
        .min_filter = SDL_GPU_FILTER_LINEAR,
        .mag_filter = SDL_GPU_FILTER_LINEAR,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .max_anisotropy = 8.0F,
        .min_lod = 0.0F,
        .max_lod = static_cast<float>(mipLevels - 1U),
        .enable_anisotropy = true,
    };
    vegetationSampler_ = SDL_CreateGPUSampler(device_, &samplerInfo);
    if (!treeAtlas_ || !vegetationSampler_) {
        throw std::runtime_error(std::string{"Tree atlas GPU allocation failed: "} + SDL_GetError());
    }

    const auto uploadSize = static_cast<std::uint32_t>(pixels.size());
    const SDL_GPUTransferBufferCreateInfo transferInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = uploadSize,
    };
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!transfer) throw std::runtime_error(std::string{"Tree atlas transfer allocation failed: "} + SDL_GetError());
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        throw std::runtime_error(std::string{"Tree atlas mapping failed: "} + SDL_GetError());
    }
    std::memcpy(mapped, pixels.data(), pixels.size());
    SDL_UnmapGPUTransferBuffer(device_, transfer);
    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    if (!command) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        throw std::runtime_error(std::string{"Tree atlas upload command failed: "} + SDL_GetError());
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    const SDL_GPUTextureTransferInfo source{
        .transfer_buffer = transfer,
        .offset = 0,
        .pixels_per_row = static_cast<std::uint32_t>(atlasWidth),
        .rows_per_layer = static_cast<std::uint32_t>(atlasHeight),
    };
    const SDL_GPUTextureRegion destination{
        .texture = treeAtlas_, .mip_level = 0, .layer = 0,
        .x = 0, .y = 0, .z = 0,
        .w = static_cast<std::uint32_t>(atlasWidth), .h = static_cast<std::uint32_t>(atlasHeight), .d = 1,
    };
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GenerateMipmapsForGPUTexture(command, treeAtlas_);
    SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
}

void Renderer::createHudResources() {
    const SDL_GPUTextureCreateInfo textureInfo{
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = HudRasterizer::Width,
        .height = HudRasterizer::Height,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    hudTexture_ = SDL_CreateGPUTexture(device_, &textureInfo);
    const SDL_GPUSamplerCreateInfo samplerInfo{
        .min_filter = SDL_GPU_FILTER_LINEAR,
        .mag_filter = SDL_GPU_FILTER_LINEAR,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    };
    hudSampler_ = SDL_CreateGPUSampler(device_, &samplerInfo);
    const SDL_GPUTransferBufferCreateInfo transferInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = HudRasterizer::Width * HudRasterizer::Height * 4U,
    };
    hudTransfer_ = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!hudTexture_ || !hudSampler_ || !hudTransfer_) {
        throw std::runtime_error(std::string{"HUD GPU resource creation failed: "} + SDL_GetError());
    }
}

void Renderer::updateHudTexture(SDL_GPUCommandBuffer* command, const HudState& state, float timeSeconds) {
    if (lastHudUpload_ >= 0.0F && timeSeconds - lastHudUpload_ < 0.10F) return;
    lastHudUpload_ = timeSeconds;
    hudRasterizer_.render(state);
    void* mapped = SDL_MapGPUTransferBuffer(device_, hudTransfer_, true);
    if (!mapped) throw std::runtime_error(std::string{"HUD transfer mapping failed: "} + SDL_GetError());
    std::memcpy(mapped, hudRasterizer_.pixels().data(), hudRasterizer_.pixels().size());
    SDL_UnmapGPUTransferBuffer(device_, hudTransfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    const SDL_GPUTextureTransferInfo source{
        .transfer_buffer = hudTransfer_,
        .offset = 0,
        .pixels_per_row = HudRasterizer::Width,
        .rows_per_layer = HudRasterizer::Height,
    };
    const SDL_GPUTextureRegion destination{
        .texture = hudTexture_, .mip_level = 0, .layer = 0,
        .x = 0, .y = 0, .z = 0,
        .w = HudRasterizer::Width, .h = HudRasterizer::Height, .d = 1,
    };
    SDL_UploadToGPUTexture(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
}

void Renderer::releaseTile(GpuTile& tile) noexcept {
    if (tile.vertices) SDL_ReleaseGPUBuffer(device_, tile.vertices);
    if (tile.indices) SDL_ReleaseGPUBuffer(device_, tile.indices);
    if (tile.trees) SDL_ReleaseGPUBuffer(device_, tile.trees);
    if (tile.grass) SDL_ReleaseGPUBuffer(device_, tile.grass);
    tile = {};
}

void Renderer::releaseMesh(GpuMesh& mesh) noexcept {
    if (mesh.vertices) SDL_ReleaseGPUBuffer(device_, mesh.vertices);
    if (mesh.indices) SDL_ReleaseGPUBuffer(device_, mesh.indices);
    mesh = {};
}

SDL_GPUShader* Renderer::compileShader(std::string_view filename, SDL_GPUShaderStage stage) const {
    const auto path = shaderDirectory_ / filename;
    std::ifstream stream{path, std::ios::binary};
    if (!stream) throw std::runtime_error("Could not open shader " + path.string());
    const std::string source{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    const auto crossStage = stage == SDL_GPU_SHADERSTAGE_VERTEX
        ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX : SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
    const SDL_ShaderCross_HLSL_Info hlsl{
        .source = source.c_str(), .entrypoint = "main", .shader_stage = crossStage,
    };
    std::size_t spirvSize{};
    void* spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl, &spirvSize);
    if (!spirv) throw std::runtime_error("HLSL compilation failed for " + path.string() + ": " + SDL_GetError());
    auto* metadata = SDL_ShaderCross_ReflectGraphicsSPIRV(static_cast<const Uint8*>(spirv), spirvSize, 0);
    if (!metadata) {
        SDL_free(spirv);
        throw std::runtime_error("Shader reflection failed for " + path.string() + ": " + SDL_GetError());
    }
    const SDL_ShaderCross_SPIRV_Info info{
        .bytecode = static_cast<const Uint8*>(spirv), .bytecode_size = spirvSize,
        .entrypoint = "main", .shader_stage = crossStage,
    };
    SDL_GPUShader* shader = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(device_, &info, &metadata->resource_info, 0);
    SDL_free(metadata);
    SDL_free(spirv);
    if (!shader) throw std::runtime_error("GPU shader creation failed for " + path.string() + ": " + SDL_GetError());
    return shader;
}

SDL_GPUBuffer* Renderer::uploadBuffer(SDL_GPUBufferUsageFlags usage, const void* data, std::size_t size) const {
    if (size == 0 || size > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("Invalid GPU upload size");
    const SDL_GPUBufferCreateInfo bufferInfo{.usage = usage, .size = static_cast<std::uint32_t>(size)};
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &bufferInfo);
    const SDL_GPUTransferBufferCreateInfo transferInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = static_cast<std::uint32_t>(size),
    };
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!buffer || !transfer) {
        if (buffer) SDL_ReleaseGPUBuffer(device_, buffer);
        if (transfer) SDL_ReleaseGPUTransferBuffer(device_, transfer);
        throw std::runtime_error(std::string{"GPU buffer allocation failed: "} + SDL_GetError());
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (!mapped) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUBuffer(device_, buffer);
        throw std::runtime_error(std::string{"GPU transfer mapping failed: "} + SDL_GetError());
    }
    std::memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(device_, transfer);
    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    if (!command) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUBuffer(device_, buffer);
        throw std::runtime_error(std::string{"GPU upload command buffer failed: "} + SDL_GetError());
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    const SDL_GPUTransferBufferLocation source{.transfer_buffer = transfer, .offset = 0};
    const SDL_GPUBufferRegion destination{.buffer = buffer, .offset = 0, .size = static_cast<std::uint32_t>(size)};
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return buffer;
}

Renderer::GpuMesh Renderer::uploadMesh(
    const void* vertices,
    std::size_t vertexBytes,
    const std::vector<std::uint32_t>& indices) const {
    return GpuMesh{
        .vertices = uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices, vertexBytes),
        .indices = uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(), indices.size() * sizeof(std::uint32_t)),
        .indexCount = static_cast<std::uint32_t>(indices.size()),
    };
}

} // namespace terrain
