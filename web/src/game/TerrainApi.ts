import type { LodTerrainData, TerrainChunkData, WorldInfo } from './types';

export class TerrainApi {
  async getWorld(): Promise<WorldInfo> {
    const response = await fetch('/world');
    if (!response.ok) throw new Error(`World metadata failed: ${response.status}`);
    const info = await response.json() as Partial<WorldInfo>;
    if (
      typeof info.seed !== 'string'
      || typeof info.native_resolution !== 'number'
      || !Number.isFinite(info.native_resolution)
      || typeof info.latent_resolution !== 'number'
      || !Number.isFinite(info.latent_resolution)
    ) throw new Error('World metadata is missing latent_resolution; restart the Python terrain API');
    return info as WorldInfo;
  }

  async getChunk(
    chunkX: number,
    chunkZ: number,
    segments: number,
    scale: number,
  ): Promise<TerrainChunkData> {
    const j1 = chunkX * segments;
    const i1 = chunkZ * segments;
    const query = new URLSearchParams({
      i1: String(i1),
      j1: String(j1),
      i2: String(i1 + segments + 1),
      j2: String(j1 + segments + 1),
      scale: String(scale),
      climate: '1',
    });
    const response = await fetch(`/terrain?${query}`);
    if (!response.ok) {
      const message = await response.text();
      throw new Error(`Terrain ${chunkX},${chunkZ} failed: ${message}`);
    }

    const width = Number(response.headers.get('X-Width'));
    const height = Number(response.headers.get('X-Height'));
    const buffer = await response.arrayBuffer();
    if (!Number.isInteger(width) || !Number.isInteger(height) || width < 2 || height < 2) {
      throw new Error(`Terrain ${chunkX},${chunkZ} returned invalid dimensions`);
    }
    if (buffer.byteLength < width * height * Int16Array.BYTES_PER_ELEMENT) {
      throw new Error(`Terrain ${chunkX},${chunkZ} returned an incomplete heightmap`);
    }
    const elevationBytes = width * height * Int16Array.BYTES_PER_ELEMENT;
    const climateLength = width * height * 4;
    let climate: Float32Array | undefined;
    if (buffer.byteLength >= elevationBytes + climateLength * Float32Array.BYTES_PER_ELEMENT) {
      climate = new Float32Array(climateLength);
      const view = new DataView(buffer, elevationBytes);
      for (let index = 0; index < climateLength; index += 1) {
        climate[index] = view.getFloat32(index * Float32Array.BYTES_PER_ELEMENT, true);
      }
    }
    return {
      key: `${scale}:${chunkX}:${chunkZ}`,
      chunkX,
      chunkZ,
      scale,
      width,
      height,
      elevations: new Int16Array(buffer, 0, width * height),
      climate,
    };
  }

  async getLodChunk(
    level: number,
    tileX: number,
    tileZ: number,
    segments: number,
    stride: number,
  ): Promise<LodTerrainData> {
    const startI = tileZ * segments * stride;
    const startJ = tileX * segments * stride;
    const query = new URLSearchParams({
      i1: String(startI),
      j1: String(startJ),
      i2: String(startI + segments * stride + 1),
      j2: String(startJ + segments * stride + 1),
      stride: String(stride),
    });
    const response = await fetch(`/terrain/lod?${query}`);
    if (!response.ok) throw new Error(`Far terrain LOD ${level} failed: ${await response.text()}`);
    const width = Number(response.headers.get('X-Width'));
    const height = Number(response.headers.get('X-Height'));
    const buffer = await response.arrayBuffer();
    if (!Number.isInteger(width) || !Number.isInteger(height) || width < 2 || height < 2) {
      throw new Error(`Far terrain LOD ${level} returned invalid dimensions`);
    }
    const length = width * height;
    if (buffer.byteLength < length * Int16Array.BYTES_PER_ELEMENT) {
      throw new Error(`Far terrain LOD ${level} returned incomplete data`);
    }
    return {
      level,
      tileX,
      tileZ,
      stride,
      startI,
      startJ,
      width,
      height,
      elevations: new Int16Array(buffer, 0, length),
    };
  }
}
