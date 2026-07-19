export type WorldInfo = {
  seed: string;
  native_resolution: number;
  latent_resolution: number;
};

export type TerrainChunkData = {
  key: string;
  chunkX: number;
  chunkZ: number;
  scale: number;
  width: number;
  height: number;
  elevations: Int16Array;
  climate?: Float32Array;
};

export type LodTerrainData = {
  level: number;
  tileX: number;
  tileZ: number;
  stride: number;
  startI: number;
  startJ: number;
  width: number;
  height: number;
  elevations: Int16Array;
};

export type GameStatus = {
  seed: string;
  loaded: number;
  expected: number;
  position: { x: number; y: number; z: number };
  flying: boolean;
  flySpeed: number;
  aimDistance: number | null;
  farCoverage: { installedKm: number; requestedKm: number; loading: boolean; failed: boolean };
  ready: boolean;
  message: string;
};
