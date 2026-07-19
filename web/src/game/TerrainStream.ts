import RAPIER from '@dimforge/rapier3d-compat';
import * as THREE from 'three';
import type { CSM } from 'three/addons/csm/CSM.js';

import { FarTerrainStream } from './FarTerrainStream';
import { Forest, type TreePlacement, type TreeSpecies } from './Forest';
import { Grass, type SurfaceSample } from './Grass';
import { TerrainApi } from './TerrainApi';
import type { TerrainChunkData } from './types';

type LoadedChunk = {
  key: string;
  chunkX: number;
  chunkZ: number;
  scale: number;
  mesh: THREE.Mesh<THREE.BufferGeometry, THREE.MeshStandardMaterial>;
  body?: RAPIER.RigidBody;
  trees: TreePlacement[];
  data: TerrainChunkData;
  spawn: THREE.Vector3;
  spawnWalkable: boolean;
};

type QueuedChunk = {
  x: number;
  z: number;
  segments: number;
  scale: number;
  priority: number;
};

type StreamCallbacks = {
  onProgress: (loaded: number, expected: number, message: string) => void;
  onFirstTerrain: (spawn: THREE.Vector3) => void;
};

type PendingSpawn = {
  chunkX: number;
  chunkZ: number;
  resolve: (spawn: THREE.Vector3 | undefined) => void;
  reject: (reason: unknown) => void;
};

type VegetationClimate = {
  suitability: number;
  species: TreeSpecies;
  temperature: number;
  moisture: number;
};

export class TerrainStream {
  readonly segments = 64;
  readonly scale = 4;
  readonly nearRadius = 2;
  readonly chunkWorldSize: number;

  private readonly chunks = new Map<string, LoadedChunk>();
  private readonly requested = new Set<string>();
  private readonly queue: QueuedChunk[] = [];
  private readonly forest: Forest;
  private readonly grass: Grass;
  private readonly farTerrain: FarTerrainStream;
  private readonly terrainNoiseTexture: THREE.DataTexture;
  private readonly terrainViewer = new THREE.Vector2();
  private readonly measurementRaycaster = new THREE.Raycaster();
  private readonly reticle = new THREE.Vector2(0, 0);
  private radius = 8;
  private activeRequests = 0;
  private firstTerrainSent = false;
  private centerChunkX = Number.NaN;
  private centerChunkZ = Number.NaN;
  private originX = 0;
  private originZ = 0;
  private disposed = false;
  private readonly seedHash: number;
  private pendingSpawn?: PendingSpawn;
  private forestDirty = false;
  private lastForestRebuild = 0;

  constructor(
    private readonly scene: THREE.Scene,
    private readonly physics: RAPIER.World,
    private readonly api: TerrainApi,
    private readonly csm: CSM,
    private readonly nativeResolution: number,
    private readonly latentResolution: number,
    worldSeed: string,
    private readonly callbacks: StreamCallbacks,
  ) {
    this.chunkWorldSize = this.segments * (nativeResolution / this.scale);
    this.seedHash = this.hashString(worldSeed);
    this.terrainNoiseTexture = this.createTerrainNoiseTexture();
    this.forest = new Forest(scene, csm);
    this.grass = new Grass(scene, csm, this.seedHash, (x, z) => this.sampleSurface(x, z));
    this.farTerrain = new FarTerrainStream(
      scene,
      api,
      this.latentResolution,
      this.terrainNoiseTexture,
    );
  }

  update(
    absoluteX: number,
    absoluteZ: number,
    originX: number,
    originZ: number,
    heightAboveGround = 1.68,
  ): void {
    this.originX = originX;
    this.originZ = originZ;
    const nextX = Math.floor(absoluteX / this.chunkWorldSize);
    const nextZ = Math.floor(absoluteZ / this.chunkWorldSize);
    const nextRadius = this.radiusForHeight(heightAboveGround);
    const radiusChanged = nextRadius !== this.radius;
    this.radius = nextRadius;

    if (nextX !== this.centerChunkX || nextZ !== this.centerChunkZ || radiusChanged) {
      this.centerChunkX = nextX;
      this.centerChunkZ = nextZ;
      this.enqueueNeighborhood();
      this.removeDistantChunks();
    }

    this.terrainViewer.set(absoluteX, absoluteZ);
    this.forest.updateViewer(absoluteX - originX, absoluteZ - originZ);
    if (this.forestDirty && performance.now() - this.lastForestRebuild > 250) this.rebuildForest();
    this.pumpQueue();
    this.grass.update(absoluteX, absoluteZ, originX, originZ, performance.now() * 0.001);
    const geometricHorizon = Math.sqrt(2 * 6_371_000 * Math.max(1.68, heightAboveGround));
    if (this.firstTerrainSent) {
      this.farTerrain.update(
        absoluteX,
        absoluteZ,
        originX,
        originZ,
        Math.min(800_000, geometricHorizon * 1.35),
      );
    }
  }

  groundElevation(absoluteX: number, absoluteZ: number): number | undefined {
    return this.sampleSurface(absoluteX, absoluteZ)?.elevation;
  }

  farCoverageStatus(): { installedKm: number; requestedKm: number; loading: boolean; failed: boolean } {
    return this.farTerrain.getCoverageStatus();
  }

  measureDistance(camera: THREE.PerspectiveCamera): number | null {
    this.measurementRaycaster.setFromCamera(this.reticle, camera);
    this.measurementRaycaster.near = 0.15;
    this.measurementRaycaster.far = Math.min(camera.far, (this.radius + 1) * this.chunkWorldSize * 1.5);
    const intersections = this.measurementRaycaster.intersectObjects(
      Array.from(this.chunks.values(), (chunk) => chunk.mesh),
      false,
    );
    return intersections[0]?.distance ?? null;
  }

  shiftOrigin(originX: number, originZ: number): void {
    this.originX = originX;
    this.originZ = originZ;
    for (const chunk of this.chunks.values()) this.placeChunk(chunk);
    this.rebuildForest();
    this.grass.invalidate();
  }

  requestSpawn(absoluteX: number, absoluteZ: number): Promise<THREE.Vector3 | undefined> {
    const chunkX = Math.floor(absoluteX / this.chunkWorldSize);
    const chunkZ = Math.floor(absoluteZ / this.chunkWorldSize);
    const loaded = this.chunks.get(this.spatialKey(chunkX, chunkZ));
    if (loaded?.scale === this.scale) {
      if (!loaded.spawnWalkable) return Promise.resolve(undefined);
      return Promise.resolve(new THREE.Vector3(
        loaded.spawn.x + chunkX * this.chunkWorldSize,
        loaded.spawn.y,
        loaded.spawn.z + chunkZ * this.chunkWorldSize,
      ));
    }
    this.pendingSpawn?.reject(new Error('Superseded by another teleport'));
    return new Promise<THREE.Vector3 | undefined>((resolve, reject) => {
      this.pendingSpawn = { chunkX, chunkZ, resolve, reject };
      this.update(absoluteX, absoluteZ, this.originX, this.originZ);
    });
  }

  dispose(): void {
    this.disposed = true;
    this.pendingSpawn?.reject(new Error('Terrain stream disposed'));
    this.pendingSpawn = undefined;
    this.queue.length = 0;
    for (const chunk of this.chunks.values()) this.removeChunk(chunk);
    this.chunks.clear();
    this.forest.dispose(this.scene);
    this.grass.dispose(this.scene);
    this.farTerrain.dispose();
    this.terrainNoiseTexture.dispose();
  }

  private enqueueNeighborhood(): void {
    const expected = (this.radius * 2 + 1) ** 2;
    for (let index = this.queue.length - 1; index >= 0; index -= 1) {
      const queued = this.queue[index];
      if (
        Math.abs(queued.x - this.centerChunkX) > this.radius + 1 ||
        Math.abs(queued.z - this.centerChunkZ) > this.radius + 1
      ) {
        this.queue.splice(index, 1);
        this.requested.delete(this.requestKey(queued.scale, queued.x, queued.z));
      }
    }

    for (let dz = -this.radius; dz <= this.radius; dz += 1) {
      for (let dx = -this.radius; dx <= this.radius; dx += 1) {
        const x = this.centerChunkX + dx;
        const z = this.centerChunkZ + dz;
        const distance = Math.max(Math.abs(dx), Math.abs(dz));
        const scale = distance <= this.nearRadius ? this.scale : distance <= 5 ? 2 : 1;
        const segments = Math.round(this.chunkWorldSize / (this.nativeResolution / scale));
        const spatialKey = this.spatialKey(x, z);
        const requestKey = this.requestKey(scale, x, z);
        const loaded = this.chunks.get(spatialKey);
        if ((loaded && loaded.scale >= scale) || this.requested.has(requestKey)) continue;
        this.requested.add(requestKey);
        this.queue.push({
          x,
          z,
          segments,
          scale,
          priority: dx * dx + dz * dz + (scale === this.scale ? 0 : scale === 2 ? 50 : 100),
        });
      }
    }
    this.queue.sort((a, b) => a.priority - b.priority);
    this.callbacks.onProgress(this.nearbyLoadedCount(), expected, 'Generating terrain');
  }

  private pumpQueue(): void {
    if (this.disposed) return;
    while (this.activeRequests < 2 && this.queue.length > 0) {
      const next = this.queue.shift()!;
      const requestKey = this.requestKey(next.scale, next.x, next.z);
      this.activeRequests += 1;
      void this.api.getChunk(next.x, next.z, next.segments, next.scale)
        .then((data) => this.installChunk(data))
        .catch((error: unknown) => {
          if (
            this.pendingSpawn
            && next.scale === this.scale
            && next.x === this.pendingSpawn.chunkX
            && next.z === this.pendingSpawn.chunkZ
          ) {
            const pending = this.pendingSpawn;
            this.pendingSpawn = undefined;
            pending.reject(error);
          }
          this.callbacks.onProgress(this.nearbyLoadedCount(), (this.radius * 2 + 1) ** 2, String(error));
        })
        .finally(() => {
          this.requested.delete(requestKey);
          this.activeRequests -= 1;
          this.pumpQueue();
        });
    }
  }

  private installChunk(data: TerrainChunkData): void {
    if (this.disposed) return;
    if (
      Math.abs(data.chunkX - this.centerChunkX) > this.radius + 1 ||
      Math.abs(data.chunkZ - this.centerChunkZ) > this.radius + 1
    ) return;

    const spatialKey = this.spatialKey(data.chunkX, data.chunkZ);
    const existing = this.chunks.get(spatialKey);
    if (existing && existing.scale >= data.scale) return;
    if (existing) this.removeChunk(existing);

    const { geometry, vertices, indices, spawn, spawnWalkable, trees } = this.buildGeometry(data);
    const material = this.createTerrainMaterial(data);
    const mesh = new THREE.Mesh(geometry, material);
    mesh.castShadow = data.scale === this.scale;
    mesh.receiveShadow = true;
    this.scene.add(mesh);

    const body = data.scale === this.scale
      ? this.physics.createRigidBody(RAPIER.RigidBodyDesc.fixed())
      : undefined;
    if (body) this.physics.createCollider(RAPIER.ColliderDesc.trimesh(vertices, indices), body);
    const chunk: LoadedChunk = {
      key: spatialKey,
      chunkX: data.chunkX,
      chunkZ: data.chunkZ,
      scale: data.scale,
      mesh,
      body,
      trees,
      data,
      spawn: spawn.clone(),
      spawnWalkable,
    };
    this.chunks.set(spatialKey, chunk);
    this.placeChunk(chunk);
    this.stitchChunkEdges(chunk);
    this.forestDirty = true;
    if (
      Math.abs(data.chunkX - this.centerChunkX) <= 1
      && Math.abs(data.chunkZ - this.centerChunkZ) <= 1
    ) this.grass.invalidate();

    if (!this.firstTerrainSent && data.scale === this.scale && data.chunkX === this.centerChunkX && data.chunkZ === this.centerChunkZ) {
      this.firstTerrainSent = true;
      spawn.x += data.chunkX * this.chunkWorldSize;
      spawn.z += data.chunkZ * this.chunkWorldSize;
      this.callbacks.onFirstTerrain(spawn);
    }

    if (
      this.pendingSpawn
      && data.scale === this.scale
      && data.chunkX === this.pendingSpawn.chunkX
      && data.chunkZ === this.pendingSpawn.chunkZ
    ) {
      const pending = this.pendingSpawn;
      this.pendingSpawn = undefined;
      pending.resolve(spawnWalkable
        ? new THREE.Vector3(
          spawn.x + data.chunkX * this.chunkWorldSize,
          spawn.y,
          spawn.z + data.chunkZ * this.chunkWorldSize,
        )
        : undefined);
    }

    const expected = (this.radius * 2 + 1) ** 2;
    const loaded = this.nearbyLoadedCount();
    this.callbacks.onProgress(loaded, expected, loaded === expected ? 'Terrain ready' : 'Streaming terrain');
  }

  private buildGeometry(data: TerrainChunkData): {
    geometry: THREE.BufferGeometry;
    vertices: Float32Array;
    indices: Uint32Array;
    spawn: THREE.Vector3;
    spawnWalkable: boolean;
    trees: TreePlacement[];
  } {
    const sampleSpacing = this.nativeResolution / data.scale;
    const vertexCount = data.width * data.height;
    const vertices = new Float32Array(vertexCount * 3);
    const colors = new Float32Array(vertexCount * 3);
    const indexList: number[] = [];
    const { position: spawn, walkable: spawnWalkable } = this.findSafeSpawn(data, sampleSpacing);

    for (let z = 0; z < data.height; z += 1) {
      for (let x = 0; x < data.width; x += 1) {
        const index = z * data.width + x;
        vertices[index * 3] = x * sampleSpacing;
        vertices[index * 3 + 1] = data.elevations[index];
        vertices[index * 3 + 2] = z * sampleSpacing;
      }
    }

    for (let z = 0; z < data.height - 1; z += 1) {
      for (let x = 0; x < data.width - 1; x += 1) {
        const a = z * data.width + x;
        const b = a + 1;
        const c = a + data.width;
        const d = c + 1;
        indexList.push(a, c, b, b, c, d);
      }
    }
    const indices = new Uint32Array(indexList);
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.BufferAttribute(vertices, 3));
    geometry.setIndex(new THREE.BufferAttribute(indices, 1));
    geometry.computeVertexNormals();

    const normals = geometry.getAttribute('normal');
    const grassDry = new THREE.Color(0x80784b);
    const grassLush = new THREE.Color(0x4f713d);
    const grassCold = new THREE.Color(0x65715a);
    const drySoil = new THREE.Color(0x8a7051);
    const rock = new THREE.Color(0x696963);
    const snow = new THREE.Color(0xd8dcda);
    const color = new THREE.Color();
    for (let index = 0; index < vertexCount; index += 1) {
      const elevation = vertices[index * 3 + 1];
      const steepness = 1 - Math.max(0, normals.getY(index));
      const heightMix = THREE.MathUtils.smoothstep(elevation, 250, 2400);
      const temperature = data.climate?.[index * 4] ?? 14;
      const precipitation = Math.max(0, data.climate?.[index * 4 + 2] ?? 650);
      const moisture = THREE.MathUtils.smoothstep(precipitation, 70, 900);
      color.copy(grassDry).lerp(grassLush, moisture);
      if (temperature < 5) color.lerp(grassCold, THREE.MathUtils.smoothstep(5 - temperature, 0, 18));
      if (temperature > 24 && moisture < 0.28) color.lerp(drySoil, (1 - moisture) * 0.72);
      color.multiplyScalar(THREE.MathUtils.lerp(1.02, 0.82, heightMix));
      color.lerp(rock, THREE.MathUtils.smoothstep(steepness, 0.12, 0.55));
      if (elevation > 2900) color.lerp(snow, THREE.MathUtils.smoothstep(elevation, 2900, 3800));
      if (elevation < 4) color.set(0x8b8067);
      colors[index * 3] = color.r;
      colors[index * 3 + 1] = color.g;
      colors[index * 3 + 2] = color.b;
    }
    geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
    geometry.computeBoundingSphere();
    return {
      geometry,
      vertices,
      indices,
      spawn,
      spawnWalkable,
      trees: this.createTrees(data, sampleSpacing),
    };
  }

  private createTerrainMaterial(data: TerrainChunkData): THREE.MeshStandardMaterial {
    const material = new THREE.MeshStandardMaterial({
      vertexColors: true,
      roughness: 0.96,
      metalness: 0,
    });
    this.csm.setupMaterial(material);
    const setupCsmShader = material.onBeforeCompile;
    const chunkOffset = new THREE.Vector2(
      data.chunkX * this.chunkWorldSize,
      data.chunkZ * this.chunkWorldSize,
    );
    material.onBeforeCompile = (shader, renderer) => {
      setupCsmShader(shader, renderer);
      shader.uniforms.uTerrainChunkOffset = { value: chunkOffset };
      shader.uniforms.uTerrainNoise = { value: this.terrainNoiseTexture };
      shader.uniforms.uTerrainViewer = { value: this.terrainViewer };
      shader.vertexShader = `uniform vec2 uTerrainChunkOffset;
      uniform vec2 uTerrainViewer;
      varying vec3 vTerrainWorldPosition;
      ${shader.vertexShader}`;
      shader.vertexShader = shader.vertexShader.replace(
        '#include <begin_vertex>',
        `#include <begin_vertex>
        vTerrainWorldPosition = vec3(position.x + uTerrainChunkOffset.x, transformed.y, position.z + uTerrainChunkOffset.y);
        vec2 terrainViewerDelta = vTerrainWorldPosition.xz - uTerrainViewer;
        transformed.y -= dot(terrainViewerDelta, terrainViewerDelta) / 12742000.0;`,
      );
      shader.fragmentShader = `uniform sampler2D uTerrainNoise;
      varying vec3 vTerrainWorldPosition;
      ${shader.fragmentShader}`;
      shader.fragmentShader = shader.fragmentShader.replace(
        '#include <color_fragment>',
        `#include <color_fragment>
        vec3 terrainSamples = texture2D(uTerrainNoise, vTerrainWorldPosition.xz * 0.004).rgb;
        float terrainMicro = texture2D(uTerrainNoise, vTerrainWorldPosition.xz * 0.09).b;
        float terrainPebble = texture2D(uTerrainNoise, vTerrainWorldPosition.xz * 0.35).r;
        float terrainFine = texture2D(uTerrainNoise, vTerrainWorldPosition.xz * 1.1).g;
        float terrainBroad = terrainSamples.r;
        float terrainDetail = terrainSamples.g;
        float terrainGrain = terrainSamples.b;
        float terrainVariation = (terrainBroad - 0.5) * 0.28
          + (terrainDetail - 0.5) * 0.17
          + (terrainGrain - 0.5) * 0.07
          + (terrainMicro - 0.5) * 0.09
          + (terrainPebble - 0.5) * 0.10
          + (terrainFine - 0.5) * 0.045;
        diffuseColor.rgb *= 1.0 + terrainVariation;
        float terrainSoil = smoothstep(0.76, 0.96, terrainGrain + terrainBroad * 0.12);
        diffuseColor.rgb = mix(diffuseColor.rgb, diffuseColor.rgb * vec3(0.84, 0.78, 0.67), terrainSoil * 0.24);`,
      );
      shader.fragmentShader = shader.fragmentShader.replace(
        '#include <opaque_fragment>',
        `// Dense vegetation can cover every direct-light sample. Preserve a
        // physically plausible diffuse skylight floor instead of crushing the
        // generated ground albedo to black.
        outgoingLight = max(outgoingLight, diffuseColor.rgb * 0.44);
        #include <opaque_fragment>`,
      );
    };
    material.customProgramCacheKey = () => 'terrain-procedural-ground-v6';
    return material;
  }

  private createTrees(data: TerrainChunkData, sampleSpacing: number): TreePlacement[] {
    const trees: TreePlacement[] = [];
    const detail: TreePlacement['detail'] = data.scale === this.scale ? 'near' : 'far';
    const subdivisions = 1;
    const candidateSpacing = sampleSpacing / subdivisions;
    const gridWidth = (data.width - 1) * subdivisions;
    const gridHeight = (data.height - 1) * subdivisions;
    const speciesColors: Record<TreeSpecies, [THREE.Color, THREE.Color]> = {
      spruce: [new THREE.Color(0xb7c3af), new THREE.Color(0xe1e4d7)],
      birch: [new THREE.Color(0xc4cdb2), new THREE.Color(0xe9e5d1)],
      oak: [new THREE.Color(0xb4c4a1), new THREE.Color(0xdeddbc)],
      acacia: [new THREE.Color(0xc5bf98), new THREE.Color(0xe6d6a5)],
      tropical: [new THREE.Color(0xa8c3a0), new THREE.Color(0xd1dfbd)],
    };
    for (let cellZ = 0; cellZ < data.height - 1; cellZ += 1) {
      for (let cellX = 0; cellX < data.width - 1; cellX += 1) {
        for (let subZ = 0; subZ < subdivisions; subZ += 1) {
          for (let subX = 0; subX < subdivisions; subX += 1) {
            const worldGridX = data.chunkX * gridWidth + cellX * subdivisions + subX;
            const worldGridZ = data.chunkZ * gridHeight + cellZ * subdivisions + subZ;
            const jitter = candidateSpacing * 0.44;
            const localX = THREE.MathUtils.clamp(
              (cellX * subdivisions + subX + 0.5) * candidateSpacing
                + (this.hash(worldGridX + 43, worldGridZ - 19) - 0.5) * jitter * 2,
              0.05,
              this.chunkWorldSize - 0.05,
            );
            const localZ = THREE.MathUtils.clamp(
              (cellZ * subdivisions + subZ + 0.5) * candidateSpacing
                + (this.hash(worldGridX - 61, worldGridZ + 37) - 0.5) * jitter * 2,
              0.05,
              this.chunkWorldSize - 0.05,
            );
            const elevation = this.sampleElevation(data, localX, localZ, sampleSpacing);
            if (elevation < 18 || elevation > 2250) continue;
            const left = this.sampleElevation(data, Math.max(0, localX - candidateSpacing), localZ, sampleSpacing);
            const right = this.sampleElevation(data, Math.min(this.chunkWorldSize, localX + candidateSpacing), localZ, sampleSpacing);
            const back = this.sampleElevation(data, localX, Math.max(0, localZ - candidateSpacing), sampleSpacing);
            const front = this.sampleElevation(data, localX, Math.min(this.chunkWorldSize, localZ + candidateSpacing), sampleSpacing);
            if (Math.max(Math.abs(right - left), Math.abs(front - back)) / (candidateSpacing * 2) > 0.48) continue;

            const absoluteX = data.chunkX * this.chunkWorldSize + localX;
            const absoluteZ = data.chunkZ * this.chunkWorldSize + localZ;
            const climate = this.vegetationClimate(data, localX, localZ, sampleSpacing, worldGridX, worldGridZ);
            if (climate.suitability < 0.08) continue;
            const standNoise = this.forestNoise(absoluteX, absoluteZ);
            const standStrength = climate.suitability * 0.78 + standNoise * 0.62;
            if (standStrength < 0.34) continue;
            const density = THREE.MathUtils.smoothstep(standStrength, 0.34, 0.82);
            const probability = detail === 'near'
              ? THREE.MathUtils.lerp(0.5, 0.98, density)
              : THREE.MathUtils.lerp(0.65, 0.98, density);
            if (this.hash(worldGridX + 719, worldGridZ - 283) > probability) continue;
            const variation = this.hash(worldGridX + 173, worldGridZ - 91);
            const dimensions = this.treeDimensions(climate.species, variation, worldGridX, worldGridZ);
            const [dark, light] = speciesColors[climate.species];
            trees.push({
              x: absoluteX,
              y: elevation - 0.28,
              z: absoluteZ,
              height: dimensions.height * (detail === 'far' ? 1.06 : 1),
              width: detail === 'far' ? dimensions.height * 2.15 : dimensions.width,
              rotation: this.hash(worldGridX + 11, worldGridZ + 29) * Math.PI,
              color: dark.clone().lerp(light, variation * 0.62),
              species: climate.species,
              detail,
            });
          }
        }
      }
    }
    return trees;
  }

  private findSafeSpawn(
    data: TerrainChunkData,
    sampleSpacing: number,
  ): { position: THREE.Vector3; walkable: boolean } {
    const centreX = (data.width - 1) / 2;
    const centreZ = (data.height - 1) / 2;
    let bestScore = Number.POSITIVE_INFINITY;
    let bestX = Math.floor(centreX);
    let bestZ = Math.floor(centreZ);
    let highestElevation = Number.NEGATIVE_INFINITY;
    let highestX = bestX;
    let highestZ = bestZ;

    for (let z = 1; z < data.height - 1; z += 1) {
      for (let x = 1; x < data.width - 1; x += 1) {
        const index = z * data.width + x;
        const elevation = data.elevations[index];
        if (elevation > highestElevation) {
          highestElevation = elevation;
          highestX = x;
          highestZ = z;
        }
        if (elevation < 8 || elevation > 3_200) continue;
        const riseX = Math.abs(data.elevations[index + 1] - data.elevations[index - 1]);
        const riseZ = Math.abs(data.elevations[index + data.width] - data.elevations[index - data.width]);
        const slope = Math.max(riseX, riseZ) / (sampleSpacing * 2);
        if (slope > 0.3) continue;
        const distance = Math.hypot(x - centreX, z - centreZ);
        const score = distance + Math.max(0, elevation - 2_200) * 0.003;
        if (score < bestScore) {
          bestScore = score;
          bestX = x;
          bestZ = z;
        }
      }
    }

    const walkable = Number.isFinite(bestScore);
    if (!walkable) {
      bestX = highestX;
      bestZ = highestZ;
    }
    const elevation = data.elevations[bestZ * data.width + bestX];
    return {
      position: new THREE.Vector3(bestX * sampleSpacing, elevation + 2.2, bestZ * sampleSpacing),
      walkable,
    };
  }

  private vegetationClimate(
    data: TerrainChunkData,
    localX: number,
    localZ: number,
    sampleSpacing: number,
    worldGridX: number,
    worldGridZ: number,
  ): VegetationClimate {
    if (!data.climate) {
      return { suitability: 0.58, species: 'oak', temperature: 14, moisture: 0.7 };
    }
    const temperature = this.sampleClimate(data, localX, localZ, sampleSpacing, 0, 14);
    const temperatureStd = this.sampleClimate(data, localX, localZ, sampleSpacing, 1, 800) / 100;
    const precipitation = Math.max(0, this.sampleClimate(data, localX, localZ, sampleSpacing, 2, 650));
    const precipitationCv = this.sampleClimate(data, localX, localZ, sampleSpacing, 3, 35);
    if (![temperature, temperatureStd, precipitation, precipitationCv].every(Number.isFinite)) {
      return { suitability: 0, species: 'oak', temperature: 0, moisture: 0 };
    }
    const effectiveTemperature = Math.max(0, temperature + temperatureStd * 0.5);
    const potentialEvapotranspiration = Math.max(
      250,
      250 + 25 * effectiveTemperature + 0.7 * effectiveTemperature * effectiveTemperature,
    );
    const treeMoisture = (precipitation / potentialEvapotranspiration)
      * (1 - 0.35 * Math.min(1, precipitationCv / 100));
    const amplitude = temperatureStd * Math.SQRT2;
    let growingSeason = 0;
    if (amplitude < 0.1) {
      growingSeason = temperature > 5 ? 365 : 0;
    } else {
      const threshold = (5 - temperature) / amplitude;
      growingSeason = threshold <= -1
        ? 365
        : threshold >= 1
          ? 0
          : 365 * (0.5 - Math.asin(THREE.MathUtils.clamp(threshold, -1, 1)) / Math.PI);
    }
    const growingSeasonFactor = THREE.MathUtils.clamp((growingSeason - 60) / 90, 0, 1);
    const suitability = THREE.MathUtils.smoothstep(treeMoisture * growingSeasonFactor, 0.18, 1.05);
    let species: TreeSpecies;
    if (temperature < 5) {
      species = 'spruce';
    } else if (temperature < 12) {
      species = this.hash(worldGridX + 331, worldGridZ - 257) < 0.58 ? 'birch' : 'spruce';
    } else if (temperature >= 22 && treeMoisture < 0.66) {
      species = 'acacia';
    } else if (temperature >= 20 && treeMoisture >= 0.9) {
      species = 'tropical';
    } else {
      species = 'oak';
    }
    return { suitability, species, temperature, moisture: treeMoisture };
  }

  private treeDimensions(
    species: TreeSpecies,
    variation: number,
    worldGridX: number,
    worldGridZ: number,
  ): { height: number; width: number } {
    const shape = this.hash(worldGridX - 33, worldGridZ + 47);
    const ranges: Record<TreeSpecies, [number, number, number, number]> = {
      spruce: [14, 30, 0.36, 0.44],
      birch: [12, 22, 0.28, 0.38],
      oak: [14, 24, 0.52, 0.68],
      acacia: [7, 14, 0.75, 1.05],
      tropical: [20, 34, 0.42, 0.56],
    };
    const [minimum, maximum, narrow, wide] = ranges[species];
    const height = THREE.MathUtils.lerp(minimum, maximum, variation);
    return { height, width: height * THREE.MathUtils.lerp(narrow, wide, shape) };
  }

  private sampleSurface(absoluteX: number, absoluteZ: number): SurfaceSample | undefined {
    const chunkX = Math.floor(absoluteX / this.chunkWorldSize);
    const chunkZ = Math.floor(absoluteZ / this.chunkWorldSize);
    const chunk = this.chunks.get(this.spatialKey(chunkX, chunkZ));
    if (!chunk) return undefined;
    const localX = absoluteX - chunkX * this.chunkWorldSize;
    const localZ = absoluteZ - chunkZ * this.chunkWorldSize;
    const spacing = this.nativeResolution / chunk.data.scale;
    const elevation = this.sampleElevation(chunk.data, localX, localZ, spacing);
    const left = this.sampleElevation(chunk.data, Math.max(0, localX - spacing), localZ, spacing);
    const right = this.sampleElevation(chunk.data, Math.min(this.chunkWorldSize, localX + spacing), localZ, spacing);
    const back = this.sampleElevation(chunk.data, localX, Math.max(0, localZ - spacing), spacing);
    const front = this.sampleElevation(chunk.data, localX, Math.min(this.chunkWorldSize, localZ + spacing), spacing);
    const slope = Math.max(Math.abs(right - left), Math.abs(front - back)) / (spacing * 2);
    return {
      elevation,
      slope,
      temperature: this.sampleClimate(chunk.data, localX, localZ, spacing, 0, 14),
      precipitation: Math.max(0, this.sampleClimate(chunk.data, localX, localZ, spacing, 2, 650)),
      precipitationCv: this.sampleClimate(chunk.data, localX, localZ, spacing, 3, 35),
    };
  }

  private sampleElevation(data: TerrainChunkData, localX: number, localZ: number, spacing: number): number {
    return this.sampleGrid(data.elevations, data.width, data.height, localX / spacing, localZ / spacing, 1, 0);
  }

  private sampleClimate(
    data: TerrainChunkData,
    localX: number,
    localZ: number,
    spacing: number,
    channel: number,
    fallback: number,
  ): number {
    if (!data.climate) return fallback;
    const value = this.sampleGrid(
      data.climate,
      data.width,
      data.height,
      localX / spacing,
      localZ / spacing,
      4,
      channel,
    );
    return Number.isFinite(value) ? value : fallback;
  }

  private sampleGrid(
    values: Int16Array | Float32Array,
    width: number,
    height: number,
    gridX: number,
    gridZ: number,
    stride: number,
    offset: number,
  ): number {
    const x0 = THREE.MathUtils.clamp(Math.floor(gridX), 0, width - 1);
    const z0 = THREE.MathUtils.clamp(Math.floor(gridZ), 0, height - 1);
    const x1 = Math.min(width - 1, x0 + 1);
    const z1 = Math.min(height - 1, z0 + 1);
    const tx = THREE.MathUtils.clamp(gridX - x0, 0, 1);
    const tz = THREE.MathUtils.clamp(gridZ - z0, 0, 1);
    const top = THREE.MathUtils.lerp(
      values[(z0 * width + x0) * stride + offset],
      values[(z0 * width + x1) * stride + offset],
      tx,
    );
    const bottom = THREE.MathUtils.lerp(
      values[(z1 * width + x0) * stride + offset],
      values[(z1 * width + x1) * stride + offset],
      tx,
    );
    return THREE.MathUtils.lerp(top, bottom, tz);
  }

  private forestNoise(x: number, z: number): number {
    return (
      this.valueNoise(x, z, 1200) * 0.56
      + this.valueNoise(x, z, 420) * 0.3
      + this.valueNoise(x, z, 130) * 0.14
    );
  }

  private createTerrainNoiseTexture(): THREE.DataTexture {
    const size = 256;
    const data = new Uint8Array(size * size * 4);
    const frequencies = [8, 32, 192];
    for (let y = 0; y < size; y += 1) {
      for (let x = 0; x < size; x += 1) {
        const pixel = (y * size + x) * 4;
        for (let channel = 0; channel < frequencies.length; channel += 1) {
          const cells = frequencies[channel];
          const gridX = x / size * cells;
          const gridY = y / size * cells;
          const x0 = Math.floor(gridX) % cells;
          const y0 = Math.floor(gridY) % cells;
          const x1 = (x0 + 1) % cells;
          const y1 = (y0 + 1) % cells;
          const tx = this.smooth(gridX - Math.floor(gridX));
          const ty = this.smooth(gridY - Math.floor(gridY));
          const salt = channel * 1_009;
          const top = THREE.MathUtils.lerp(this.hash(x0 + salt, y0 - salt), this.hash(x1 + salt, y0 - salt), tx);
          const bottom = THREE.MathUtils.lerp(this.hash(x0 + salt, y1 - salt), this.hash(x1 + salt, y1 - salt), tx);
          data[pixel + channel] = Math.round(THREE.MathUtils.lerp(top, bottom, ty) * 255);
        }
        data[pixel + 3] = 255;
      }
    }
    const texture = new THREE.DataTexture(data, size, size, THREE.RGBAFormat);
    texture.wrapS = THREE.RepeatWrapping;
    texture.wrapT = THREE.RepeatWrapping;
    texture.magFilter = THREE.LinearFilter;
    texture.minFilter = THREE.LinearMipmapLinearFilter;
    texture.generateMipmaps = true;
    texture.anisotropy = 4;
    texture.colorSpace = THREE.NoColorSpace;
    texture.needsUpdate = true;
    return texture;
  }

  private valueNoise(x: number, z: number, scale: number): number {
    const gridX = x / scale;
    const gridZ = z / scale;
    const x0 = Math.floor(gridX);
    const z0 = Math.floor(gridZ);
    const tx = this.smooth(gridX - x0);
    const tz = this.smooth(gridZ - z0);
    const top = THREE.MathUtils.lerp(this.hash(x0, z0), this.hash(x0 + 1, z0), tx);
    const bottom = THREE.MathUtils.lerp(this.hash(x0, z0 + 1), this.hash(x0 + 1, z0 + 1), tx);
    return THREE.MathUtils.lerp(top, bottom, tz);
  }

  private smooth(value: number): number {
    return value * value * (3 - 2 * value);
  }

  private hash(x: number, z: number): number {
    let value = Math.imul((x | 0) ^ this.seedHash, 374761393) + Math.imul(z | 0, 668265263);
    value = Math.imul(value ^ (value >>> 13), 1274126177);
    return ((value ^ (value >>> 16)) >>> 0) / 0xffffffff;
  }

  private hashString(value: string): number {
    let hash = 2166136261;
    for (let index = 0; index < value.length; index += 1) {
      hash ^= value.charCodeAt(index);
      hash = Math.imul(hash, 16777619);
    }
    return hash | 0;
  }

  private placeChunk(chunk: LoadedChunk): void {
    const x = chunk.chunkX * this.chunkWorldSize - this.originX;
    const z = chunk.chunkZ * this.chunkWorldSize - this.originZ;
    chunk.mesh.position.set(x, 0, z);
    chunk.body?.setTranslation({ x, y: 0, z }, true);
  }

  private stitchChunkEdges(chunk: LoadedChunk): void {
    const neighbors = [
      { dx: 1, dz: 0 },
      { dx: -1, dz: 0 },
      { dx: 0, dz: 1 },
      { dx: 0, dz: -1 },
    ];
    for (const { dx, dz } of neighbors) {
      const neighbor = this.chunks.get(this.spatialKey(chunk.chunkX + dx, chunk.chunkZ + dz));
      if (!neighbor) continue;
      const chunkEdge = this.edgeIndices(chunk, dx, dz);
      const neighborEdge = this.edgeIndices(neighbor, -dx, -dz);
      const chunkPositions = chunk.mesh.geometry.getAttribute('position');
      const neighborPositions = neighbor.mesh.geometry.getAttribute('position');
      for (const index of chunkEdge) chunkPositions.setY(index, chunk.data.elevations[index]);
      for (const index of neighborEdge) neighborPositions.setY(index, neighbor.data.elevations[index]);
      if (chunkEdge.length !== neighborEdge.length) {
        const highEdge = chunkEdge.length > neighborEdge.length ? chunkEdge : neighborEdge;
        const lowEdge = chunkEdge.length > neighborEdge.length ? neighborEdge : chunkEdge;
        const highPositions = chunkEdge.length > neighborEdge.length ? chunkPositions : neighborPositions;
        const lowPositions = chunkEdge.length > neighborEdge.length ? neighborPositions : chunkPositions;
        for (let sample = 0; sample < highEdge.length; sample += 1) {
          const lowPosition = sample / (highEdge.length - 1) * (lowEdge.length - 1);
          const lowA = Math.floor(lowPosition);
          const lowB = Math.min(lowEdge.length - 1, lowA + 1);
          highPositions.setY(
            highEdge[sample],
            THREE.MathUtils.lerp(
              lowPositions.getY(lowEdge[lowA]),
              lowPositions.getY(lowEdge[lowB]),
              lowPosition - lowA,
            ),
          );
        }
        highPositions.needsUpdate = true;
      }
      chunkPositions.needsUpdate = true;
      neighborPositions.needsUpdate = true;
      chunk.mesh.geometry.computeVertexNormals();
      neighbor.mesh.geometry.computeVertexNormals();
      chunk.mesh.geometry.computeBoundingSphere();
      neighbor.mesh.geometry.computeBoundingSphere();

      if (chunkEdge.length !== neighborEdge.length) continue;
      const chunkNormals = chunk.mesh.geometry.getAttribute('normal');
      const neighborNormals = neighbor.mesh.geometry.getAttribute('normal');
      for (let sample = 0; sample < chunkEdge.length; sample += 1) {
        const chunkIndex = chunkEdge[sample];
        const neighborIndex = neighborEdge[sample];
        const x = chunkNormals.getX(chunkIndex) + neighborNormals.getX(neighborIndex);
        const y = chunkNormals.getY(chunkIndex) + neighborNormals.getY(neighborIndex);
        const z = chunkNormals.getZ(chunkIndex) + neighborNormals.getZ(neighborIndex);
        const inverseLength = 1 / Math.max(0.0001, Math.hypot(x, y, z));
        chunkNormals.setXYZ(chunkIndex, x * inverseLength, y * inverseLength, z * inverseLength);
        neighborNormals.setXYZ(neighborIndex, x * inverseLength, y * inverseLength, z * inverseLength);
      }
      chunkNormals.needsUpdate = true;
      neighborNormals.needsUpdate = true;
    }
  }

  private edgeIndices(chunk: LoadedChunk, dx: number, dz: number): number[] {
    const width = chunk.data.width;
    const height = chunk.data.height;
    if (dx === 1) return Array.from({ length: height }, (_, z) => z * width + width - 1);
    if (dx === -1) return Array.from({ length: height }, (_, z) => z * width);
    if (dz === 1) return Array.from({ length: width }, (_, x) => (height - 1) * width + x);
    return Array.from({ length: width }, (_, x) => x);
  }

  private rebuildForest(): void {
    const trees: TreePlacement[] = [];
    for (const chunk of this.chunks.values()) trees.push(...chunk.trees);
    this.forest.setTrees(trees, this.originX, this.originZ);
    this.forestDirty = false;
    this.lastForestRebuild = performance.now();
  }

  private nearbyLoadedCount(): number {
    let count = 0;
    for (const chunk of this.chunks.values()) {
      if (
        Math.abs(chunk.chunkX - this.centerChunkX) <= this.radius &&
        Math.abs(chunk.chunkZ - this.centerChunkZ) <= this.radius
      ) count += 1;
    }
    return count;
  }

  private radiusForHeight(heightAboveGround: number): number {
    if (heightAboveGround < 180) return 8;
    if (heightAboveGround < 1_200) return 9;
    return 10;
  }

  private removeDistantChunks(): void {
    let removed = false;
    for (const chunk of this.chunks.values()) {
      if (
        Math.abs(chunk.chunkX - this.centerChunkX) > this.radius + 1 ||
        Math.abs(chunk.chunkZ - this.centerChunkZ) > this.radius + 1
      ) {
        this.removeChunk(chunk);
        this.chunks.delete(chunk.key);
        removed = true;
      }
    }
    if (removed) this.forestDirty = true;
  }

  private removeChunk(chunk: LoadedChunk): void {
    this.scene.remove(chunk.mesh);
    chunk.mesh.geometry.dispose();
    chunk.mesh.material.dispose();
    if (chunk.body) this.physics.removeRigidBody(chunk.body);
  }

  private spatialKey(x: number, z: number): string {
    return `${x}:${z}`;
  }

  private requestKey(scale: number, x: number, z: number): string {
    return `${scale}:${x}:${z}`;
  }
}
