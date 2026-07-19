#pragma once

#include "Config.hpp"
#include "Hud.hpp"
#include <terrain/TerrainTypes.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace terrain {

class Renderer final {
public:
    explicit Renderer(Config config);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void initialize(SDL_Window* window);
    void shutdown() noexcept;
    void resize();
    void addTile(const TileMeshPtr& tile);
    void removeTile(const TileKey& key);
    void clearTiles();
    void render(const CameraState& camera, float timeSeconds, const HudState& hud);
    void setCloudsEnabled(bool enabled) noexcept { cloudsEnabled_ = enabled; }
    void setVisualConfig(const WorldVisualConfig& visuals) noexcept { config_.visuals = visuals; }

    [[nodiscard]] const char* driverName() const noexcept;
    [[nodiscard]] std::size_t treeCount() const noexcept { return renderedTreeCount_; }
    [[nodiscard]] std::size_t grassCount() const noexcept { return renderedGrassCount_; }
    [[nodiscard]] std::size_t drawCallCount() const noexcept { return renderedDrawCallCount_; }
    [[nodiscard]] std::size_t terrainTileDrawCount() const noexcept { return renderedTerrainTileCount_; }

private:
    struct GpuMesh final {
        SDL_GPUBuffer* vertices{};
        SDL_GPUBuffer* indices{};
        std::uint32_t indexCount{};
    };

    struct GpuTile final {
        TileMeshPtr cpu;
        SDL_GPUBuffer* vertices{};
        SDL_GPUBuffer* indices{};
        SDL_GPUBuffer* trees{};
        SDL_GPUBuffer* grass{};
        std::uint32_t indexCount{};
        std::uint32_t treeCount{};
        std::uint32_t grassCount{};
    };

    void createRenderTargets();
    void createPipelines();
    void createStaticMeshes();
    void createTreeAtlas();
    void createHudResources();
    void updateHudTexture(SDL_GPUCommandBuffer* command, const HudState& state, float timeSeconds);
    void releaseTile(GpuTile& tile) noexcept;
    void releaseMesh(GpuMesh& mesh) noexcept;
    [[nodiscard]] SDL_GPUShader* compileShader(std::string_view filename, SDL_GPUShaderStage stage) const;
    [[nodiscard]] SDL_GPUBuffer* uploadBuffer(SDL_GPUBufferUsageFlags usage, const void* data, std::size_t size) const;
    [[nodiscard]] GpuMesh uploadMesh(const void* vertices, std::size_t vertexBytes, const std::vector<std::uint32_t>& indices) const;

    Config config_;
    SDL_Window* window_{};
    SDL_GPUDevice* device_{};
    SDL_GPUTexture* depthTexture_{};
    SDL_GPUTexture* sceneColorTexture_{};
    SDL_GPUTexture* cloudTexture_{};
    SDL_GPUTexture* shadowTexture_{};
    SDL_GPUTexture* farShadowTexture_{};
    SDL_GPUTexture* treeAtlas_{};
    SDL_GPUTexture* hudTexture_{};
    SDL_GPUTransferBuffer* hudTransfer_{};
    SDL_GPUSampler* shadowSampler_{};
    SDL_GPUSampler* vegetationSampler_{};
    SDL_GPUSampler* hudSampler_{};
    SDL_GPUSampler* sceneSampler_{};
    SDL_GPUSampler* cloudSampler_{};
    SDL_GPUTextureFormat depthFormat_{SDL_GPU_TEXTUREFORMAT_D32_FLOAT};
    std::filesystem::path shaderDirectory_;
    std::filesystem::path assetDirectory_;
    std::uint32_t pixelWidth_{};
    std::uint32_t pixelHeight_{};

    SDL_GPUGraphicsPipeline* terrainPipeline_{};
    SDL_GPUGraphicsPipeline* vegetationPipeline_{};
    SDL_GPUGraphicsPipeline* terrainShadowPipeline_{};
    SDL_GPUGraphicsPipeline* vegetationShadowPipeline_{};
    SDL_GPUGraphicsPipeline* skyPipeline_{};
    SDL_GPUGraphicsPipeline* cloudOverlayPipeline_{};
    SDL_GPUGraphicsPipeline* cloudCompositePipeline_{};
    SDL_GPUGraphicsPipeline* waterPipeline_{};
    SDL_GPUGraphicsPipeline* toneMapPipeline_{};
    SDL_GPUGraphicsPipeline* reticlePipeline_{};
    SDL_GPUGraphicsPipeline* hudPipeline_{};

    GpuMesh nearTreeMesh_;
    GpuMesh farTreeMesh_;
    GpuMesh grassMesh_;
    GpuMesh waterMesh_;
    std::unordered_map<TileKey, GpuTile, TileKeyHash> tiles_;
    std::size_t treeCount_{};
    std::size_t grassCount_{};
    std::size_t renderedTreeCount_{};
    std::size_t renderedGrassCount_{};
    std::size_t renderedDrawCallCount_{};
    std::size_t renderedTerrainTileCount_{};
    HudRasterizer hudRasterizer_;
    float lastHudUpload_{-1.0F};
    bool shaderCrossInitialized_{};
    bool cloudsEnabled_{true};
};

} // namespace terrain
