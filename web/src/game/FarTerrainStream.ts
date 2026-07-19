import * as THREE from 'three';

import { TerrainApi } from './TerrainApi';
import type { LodTerrainData } from './types';

const EARTH_DIAMETER = 12_742_000;
const SEGMENTS = 32;
const TILE_RADIUS = 2;
const STRIDES = [1, 2, 4, 8, 16, 32, 64] as const;

type LodSpec = {
  level: number;
  stride: number;
  spacing: number;
  tileSize: number;
  innerRadius: number;
  outerRadius: number;
  fadeWidth: number;
  material: THREE.MeshStandardMaterial;
};

type LoadedLodTile = {
  key: string;
  tileX: number;
  tileZ: number;
  spec: LodSpec;
  mesh: THREE.Mesh<THREE.BufferGeometry, THREE.MeshStandardMaterial>;
};

type QueuedLodTile = {
  key: string;
  tileX: number;
  tileZ: number;
  spec: LodSpec;
  priority: number;
};

export class FarTerrainStream {
  private readonly specs: LodSpec[];
  private readonly tiles = new Map<string, LoadedLodTile>();
  private readonly requested = new Set<string>();
  private readonly retryAt = new Map<string, number>();
  private readonly queue: QueuedLodTile[] = [];
  private readonly viewer = new THREE.Vector2();
  private readonly floatingOrigin = new THREE.Vector2();
  private activeRequests = 0;
  private originX = 0;
  private originZ = 0;
  private targetLevel = 0;
  private failed = false;
  private disposed = false;

  constructor(
    private readonly scene: THREE.Scene,
    private readonly api: TerrainApi,
    latentResolution: number,
    terrainNoiseTexture: THREE.DataTexture,
  ) {
    this.specs = STRIDES.map((stride, level) => {
      const spacing = latentResolution * stride;
      const tileSize = SEGMENTS * spacing;
      const outerRadius = tileSize * (TILE_RADIUS + 0.5);
      const previousOuter = level === 0
        ? 2_400
        : SEGMENTS * latentResolution * STRIDES[level - 1] * (TILE_RADIUS + 0.5);
      const innerRadius = level === 0 ? 2_400 : previousOuter * 0.72;
      const fadeWidth = Math.max(spacing * 3, (outerRadius - innerRadius) * 0.18);
      return {
        level,
        stride,
        spacing,
        tileSize,
        innerRadius,
        outerRadius,
        fadeWidth,
        material: this.createMaterial(
          terrainNoiseTexture,
          innerRadius,
          outerRadius,
          fadeWidth,
          level,
        ),
      };
    });
  }

  update(
    absoluteX: number,
    absoluteZ: number,
    originX: number,
    originZ: number,
    requiredRadius: number,
  ): void {
    this.viewer.set(absoluteX, absoluteZ);
    this.floatingOrigin.set(originX, originZ);
    this.originX = originX;
    this.originZ = originZ;
    this.targetLevel = this.specs.findIndex((spec) => spec.outerRadius >= requiredRadius);
    if (this.targetLevel < 0) this.targetLevel = this.specs.length - 1;

    for (const tile of this.tiles.values()) this.placeTile(tile);
    this.pruneQueue(absoluteX, absoluteZ);
    this.enqueueVisibleTiles(absoluteX, absoluteZ);
    this.removeDistantTiles(absoluteX, absoluteZ);
    this.pumpQueue();
  }

  getCoverageStatus(): { installedKm: number; requestedKm: number; loading: boolean; failed: boolean } {
    let installedRadius = 0;
    for (const spec of this.specs.slice(0, this.targetLevel + 1)) {
      const centerX = Math.floor(this.viewer.x / spec.tileSize);
      const centerZ = Math.floor(this.viewer.y / spec.tileSize);
      if (this.tiles.has(this.key(spec.level, centerX, centerZ))) installedRadius = spec.outerRadius;
      else break;
    }
    return {
      installedKm: Math.round(installedRadius / 1_000),
      requestedKm: Math.round(this.specs[this.targetLevel].outerRadius / 1_000),
      loading: this.activeRequests > 0 || this.queue.length > 0,
      failed: this.failed,
    };
  }

  dispose(): void {
    this.disposed = true;
    this.queue.length = 0;
    for (const tile of this.tiles.values()) {
      this.scene.remove(tile.mesh);
      tile.mesh.geometry.dispose();
    }
    this.tiles.clear();
    for (const spec of this.specs) spec.material.dispose();
  }

  private enqueueVisibleTiles(absoluteX: number, absoluteZ: number): void {
    const now = performance.now();
    for (const spec of this.specs.slice(0, this.targetLevel + 1)) {
      const centerX = Math.floor(absoluteX / spec.tileSize);
      const centerZ = Math.floor(absoluteZ / spec.tileSize);
      for (let dz = -TILE_RADIUS; dz <= TILE_RADIUS; dz += 1) {
        for (let dx = -TILE_RADIUS; dx <= TILE_RADIUS; dx += 1) {
          const tileX = centerX + dx;
          const tileZ = centerZ + dz;
          const key = this.key(spec.level, tileX, tileZ);
          if (this.tiles.has(key) || this.requested.has(key) || (this.retryAt.get(key) ?? 0) > now) continue;
          this.requested.add(key);
          this.queue.push({
            key,
            tileX,
            tileZ,
            spec,
            priority: spec.level * 10_000 + dx * dx + dz * dz,
          });
        }
      }
    }
    this.queue.sort((a, b) => a.priority - b.priority);
  }

  private pruneQueue(absoluteX: number, absoluteZ: number): void {
    for (let index = this.queue.length - 1; index >= 0; index -= 1) {
      const queued = this.queue[index];
      const centerX = Math.floor(absoluteX / queued.spec.tileSize);
      const centerZ = Math.floor(absoluteZ / queued.spec.tileSize);
      if (
        queued.spec.level > this.targetLevel
        || Math.abs(queued.tileX - centerX) > TILE_RADIUS + 1
        || Math.abs(queued.tileZ - centerZ) > TILE_RADIUS + 1
      ) {
        this.requested.delete(queued.key);
        this.queue.splice(index, 1);
      }
    }
  }

  private pumpQueue(): void {
    if (this.disposed || this.activeRequests >= 1) return;
    const next = this.queue.shift();
    if (!next) return;
    this.activeRequests += 1;
    void this.api.getLodChunk(
      next.spec.level,
      next.tileX,
      next.tileZ,
      SEGMENTS,
      next.spec.stride,
    ).then((data) => {
      if (!this.disposed) this.installTile(next, data);
    }).catch((error: unknown) => {
      if (!this.disposed) {
        this.failed = true;
        this.retryAt.set(next.key, performance.now() + 10_000);
        console.warn('Detailed far terrain stream failed', error);
      }
    }).finally(() => {
      this.requested.delete(next.key);
      this.activeRequests -= 1;
      this.pumpQueue();
    });
  }

  private installTile(request: QueuedLodTile, data: LodTerrainData): void {
    const geometry = new THREE.BufferGeometry();
    const vertexCount = data.width * data.height;
    const positions = new Float32Array(vertexCount * 3);
    const colors = new Float32Array(vertexCount * 3);
    const indices = new Uint32Array((data.width - 1) * (data.height - 1) * 6);
    const beach = new THREE.Color(0xa89a78);
    const lowland = new THREE.Color(0x738d5f);
    const upland = new THREE.Color(0x66785a);
    const rock = new THREE.Color(0x77756d);
    const snow = new THREE.Color(0xd8dcda);
    const color = new THREE.Color();

    for (let z = 0; z < data.height; z += 1) {
      for (let x = 0; x < data.width; x += 1) {
        const index = z * data.width + x;
        const elevation = data.elevations[index];
        positions[index * 3] = x * request.spec.spacing;
        positions[index * 3 + 1] = elevation;
        positions[index * 3 + 2] = z * request.spec.spacing;
        color.copy(lowland).lerp(upland, THREE.MathUtils.smoothstep(elevation, 500, 2_000));
        color.lerp(rock, THREE.MathUtils.smoothstep(elevation, 1_700, 3_200) * 0.62);
        if (elevation > 2_900) color.lerp(snow, THREE.MathUtils.smoothstep(elevation, 2_900, 3_800));
        if (elevation < 4) color.copy(beach);
        colors[index * 3] = color.r;
        colors[index * 3 + 1] = color.g;
        colors[index * 3 + 2] = color.b;
      }
    }
    let write = 0;
    for (let z = 0; z < data.height - 1; z += 1) {
      for (let x = 0; x < data.width - 1; x += 1) {
        const a = z * data.width + x;
        const b = a + 1;
        const c = a + data.width;
        const d = c + 1;
        indices[write++] = a; indices[write++] = c; indices[write++] = b;
        indices[write++] = b; indices[write++] = c; indices[write++] = d;
      }
    }
    geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
    geometry.setIndex(new THREE.BufferAttribute(indices, 1));
    geometry.computeVertexNormals();
    geometry.computeBoundingSphere();
    const mesh = new THREE.Mesh(geometry, request.spec.material);
    mesh.castShadow = false;
    mesh.receiveShadow = false;
    // Vertex curvature is applied in the shader and can move distant tiles
    // outside their CPU-side bounds. Keep those tiles from being falsely
    // rejected before the GPU applies the curved-earth transform.
    mesh.frustumCulled = false;
    mesh.renderOrder = -2 - request.spec.level;
    const tile: LoadedLodTile = {
      key: request.key,
      tileX: request.tileX,
      tileZ: request.tileZ,
      spec: request.spec,
      mesh,
    };
    this.tiles.set(tile.key, tile);
    this.placeTile(tile);
    this.scene.add(mesh);
    this.stitchNormals(tile);
    this.failed = false;
  }

  private createMaterial(
    noise: THREE.DataTexture,
    innerRadius: number,
    outerRadius: number,
    fadeWidth: number,
    level: number,
  ): THREE.MeshStandardMaterial {
    const material = new THREE.MeshStandardMaterial({
      vertexColors: true,
      roughness: 1,
      metalness: 0,
      alphaHash: true,
      polygonOffset: true,
      polygonOffsetFactor: 1 + level,
      polygonOffsetUnits: 1 + level,
    });
    material.onBeforeCompile = (shader) => {
      shader.uniforms.uFarViewer = { value: this.viewer };
      shader.uniforms.uFarFloatingOrigin = { value: this.floatingOrigin };
      shader.uniforms.uFarNoise = { value: noise };
      shader.vertexShader = `uniform vec2 uFarViewer;
      uniform vec2 uFarFloatingOrigin;
      varying vec2 vFarWorldPosition;
      ${shader.vertexShader}`;
      shader.vertexShader = shader.vertexShader.replace(
        '#include <begin_vertex>',
        `#include <begin_vertex>
        vec3 farLocalWorld = (modelMatrix * vec4(position, 1.0)).xyz;
        vFarWorldPosition = farLocalWorld.xz + uFarFloatingOrigin;
        vec2 farViewerDelta = vFarWorldPosition - uFarViewer;
        transformed.y -= dot(farViewerDelta, farViewerDelta) / ${EARTH_DIAMETER.toFixed(1)};`,
      );
      shader.fragmentShader = `uniform sampler2D uFarNoise;
      uniform vec2 uFarViewer;
      varying vec2 vFarWorldPosition;
      ${shader.fragmentShader}`;
      shader.fragmentShader = shader.fragmentShader.replace(
        '#include <color_fragment>',
        `#include <color_fragment>
        vec3 farNoise = texture2D(uFarNoise, vFarWorldPosition * 0.00035).rgb;
        diffuseColor.rgb *= 0.91 + farNoise.r * 0.18;
        float farForest = smoothstep(0.56, 0.78, farNoise.g * 0.72 + farNoise.b * 0.28);
        diffuseColor.rgb = mix(diffuseColor.rgb, diffuseColor.rgb * vec3(0.58, 0.72, 0.52), farForest * 0.42);
        float farDistance = length(vFarWorldPosition - uFarViewer);
        float farInner = smoothstep(${innerRadius.toFixed(1)}, ${(innerRadius + fadeWidth).toFixed(1)}, farDistance);
        float farOuter = 1.0 - smoothstep(${(outerRadius - fadeWidth).toFixed(1)}, ${outerRadius.toFixed(1)}, farDistance);
        diffuseColor.a *= min(farInner, farOuter);`,
      );
      shader.fragmentShader = shader.fragmentShader.replace(
        '#include <opaque_fragment>',
        `outgoingLight = max(outgoingLight, diffuseColor.rgb * 0.48);
        #include <opaque_fragment>`,
      );
    };
    material.customProgramCacheKey = () => `detailed-far-lod-v1-${level}`;
    return material;
  }

  private placeTile(tile: LoadedLodTile): void {
    tile.mesh.position.set(
      tile.tileX * tile.spec.tileSize - this.originX,
      0,
      tile.tileZ * tile.spec.tileSize - this.originZ,
    );
  }

  private removeDistantTiles(absoluteX: number, absoluteZ: number): void {
    for (const [key, tile] of this.tiles) {
      if (tile.spec.level > this.targetLevel) {
        this.scene.remove(tile.mesh);
        tile.mesh.geometry.dispose();
        this.tiles.delete(key);
        continue;
      }
      const centerX = Math.floor(absoluteX / tile.spec.tileSize);
      const centerZ = Math.floor(absoluteZ / tile.spec.tileSize);
      if (
        Math.abs(tile.tileX - centerX) > TILE_RADIUS + 1
        || Math.abs(tile.tileZ - centerZ) > TILE_RADIUS + 1
      ) {
        this.scene.remove(tile.mesh);
        tile.mesh.geometry.dispose();
        this.tiles.delete(key);
      }
    }
  }

  private stitchNormals(tile: LoadedLodTile): void {
    const neighbors = [
      { dx: -1, dz: 0 }, { dx: 1, dz: 0 }, { dx: 0, dz: -1 }, { dx: 0, dz: 1 },
    ];
    for (const { dx, dz } of neighbors) {
      const neighbor = this.tiles.get(this.key(tile.spec.level, tile.tileX + dx, tile.tileZ + dz));
      if (!neighbor) continue;
      const normals = tile.mesh.geometry.getAttribute('normal');
      const neighborNormals = neighbor.mesh.geometry.getAttribute('normal');
      for (let sample = 0; sample <= SEGMENTS; sample += 1) {
        const tileIndex = dx === -1
          ? sample * (SEGMENTS + 1)
          : dx === 1
            ? sample * (SEGMENTS + 1) + SEGMENTS
            : dz === -1
              ? sample
              : SEGMENTS * (SEGMENTS + 1) + sample;
        const neighborIndex = dx === -1
          ? sample * (SEGMENTS + 1) + SEGMENTS
          : dx === 1
            ? sample * (SEGMENTS + 1)
            : dz === -1
              ? SEGMENTS * (SEGMENTS + 1) + sample
              : sample;
        const x = normals.getX(tileIndex) + neighborNormals.getX(neighborIndex);
        const y = normals.getY(tileIndex) + neighborNormals.getY(neighborIndex);
        const z = normals.getZ(tileIndex) + neighborNormals.getZ(neighborIndex);
        const inverseLength = 1 / Math.max(0.0001, Math.hypot(x, y, z));
        normals.setXYZ(tileIndex, x * inverseLength, y * inverseLength, z * inverseLength);
        neighborNormals.setXYZ(neighborIndex, x * inverseLength, y * inverseLength, z * inverseLength);
      }
      normals.needsUpdate = true;
      neighborNormals.needsUpdate = true;
    }
  }

  private key(level: number, tileX: number, tileZ: number): string {
    return `${level}:${tileX}:${tileZ}`;
  }
}
