import RAPIER from '@dimforge/rapier3d-compat';
import * as THREE from 'three';

import { WorldEnvironment } from './Environment';
import { PlayerController } from './PlayerController';
import { TerrainApi } from './TerrainApi';
import { TerrainStream } from './TerrainStream';
import type { GameStatus } from './types';

const INITIAL_STATUS: GameStatus = {
  seed: '—',
  loaded: 0,
  expected: 25,
  position: { x: 0, y: 0, z: 0 },
  flying: false,
  flySpeed: 24,
  aimDistance: null,
  farCoverage: { installedKm: 0, requestedKm: 0, loading: false, failed: false },
  ready: false,
  message: 'Connecting to terrain generator',
};

export class Game {
  private readonly scene = new THREE.Scene();
  private readonly camera = new THREE.PerspectiveCamera(70, 1, 0.15, 12_000);
  private readonly renderer: THREE.WebGLRenderer;
  private readonly clock = new THREE.Clock();
  private readonly api = new TerrainApi();
  private physics?: RAPIER.World;
  private environment?: WorldEnvironment;
  private terrain?: TerrainStream;
  private player?: PlayerController;
  private frame = 0;
  private running = false;
  private disposed = false;
  private originX = 0;
  private originZ = 0;
  private status = { ...INITIAL_STATUS };
  private pixelRatio = Math.min(window.devicePixelRatio, 1.5);
  private frameTimeAccumulator = 0;
  private frameTimeSamples = 0;
  private teleporting = false;

  constructor(
    private readonly mount: HTMLElement,
    private readonly onStatus: (status: GameStatus) => void,
    private readonly onPointerLock: (locked: boolean) => void,
  ) {
    this.renderer = new THREE.WebGLRenderer({
      antialias: true,
      logarithmicDepthBuffer: true,
      powerPreference: 'high-performance',
    });
    this.renderer.setPixelRatio(this.pixelRatio);
    this.renderer.setSize(mount.clientWidth, mount.clientHeight);
    mount.appendChild(this.renderer.domElement);
    window.addEventListener('resize', this.resize);
    window.addEventListener('keydown', this.handleGlobalKeyDown);
    this.resize();
  }

  async start(): Promise<void> {
    this.emitStatus({ message: 'Loading physics' });
    await RAPIER.init();
    if (this.disposed) return;
    const worldInfo = await this.api.getWorld();
    if (this.disposed) return;
    this.physics = new RAPIER.World({ x: 0, y: -9.81, z: 0 });
    this.environment = new WorldEnvironment(this.scene, this.camera, this.renderer);
    this.player = new PlayerController(
      this.physics,
      this.camera,
      this.renderer.domElement,
      (flying, flySpeed) => this.emitStatus({ flying, flySpeed }),
      this.onPointerLock,
    );
    this.terrain = new TerrainStream(
      this.scene,
      this.physics,
      this.api,
      this.environment.csm,
      worldInfo.native_resolution,
      worldInfo.latent_resolution,
      worldInfo.seed,
      {
        onProgress: (loaded, expected, message) => this.emitStatus({ loaded, expected, message }),
        onFirstTerrain: (absoluteSpawn) => {
          this.player?.spawn(new THREE.Vector3(
            absoluteSpawn.x - this.originX,
            absoluteSpawn.y,
            absoluteSpawn.z - this.originZ,
          ));
          this.emitStatus({ ready: true, message: 'Terrain streaming' });
        },
      },
    );
    this.emitStatus({ seed: worldInfo.seed, message: 'Generating first terrain chunk' });
    this.terrain.update(0, 0, this.originX, this.originZ);
    this.running = true;
    this.clock.start();
    this.renderer.setAnimationLoop(this.animate);
  }

  requestPointerLock(): void {
    this.player?.requestPointerLock();
  }

  async teleportRandom(): Promise<void> {
    if (!this.status.ready || this.teleporting || !this.player || !this.terrain) return;
    this.teleporting = true;
    const current = this.player.getPosition();
    const currentX = current.x + this.originX;
    const currentZ = current.z + this.originZ;
    const previousOriginX = this.originX;
    const previousOriginZ = this.originZ;
    const size = this.terrain.chunkWorldSize;

    if (document.pointerLockElement) document.exitPointerLock();
    this.emitStatus({ ready: false, loaded: 0, message: 'Searching a distant climate region' });

    try {
      let spawn: THREE.Vector3 | undefined;
      for (let attempt = 0; attempt < 6 && !spawn; attempt += 1) {
        const randomValues = new Uint32Array(2);
        crypto.getRandomValues(randomValues);
        const angle = (randomValues[0] / 0xffffffff) * Math.PI * 2;
        const distance = THREE.MathUtils.lerp(20_000, 220_000, randomValues[1] / 0xffffffff);
        const chunkX = Math.floor((currentX + Math.cos(angle) * distance) / size);
        const chunkZ = Math.floor((currentZ + Math.sin(angle) * distance) / size);
        const targetX = (chunkX + 0.5) * size;
        const targetZ = (chunkZ + 0.5) * size;
        this.originX = chunkX * size;
        this.originZ = chunkZ * size;
        this.player.hold(new THREE.Vector3(targetX - this.originX, 4_200, targetZ - this.originZ));
        this.terrain.shiftOrigin(this.originX, this.originZ);
        this.emitStatus({ message: `Searching a distant climate region · ${attempt + 1}/6` });
        const candidate = await this.terrain.requestSpawn(targetX, targetZ);
        if (candidate && candidate.y > 10) spawn = candidate;
      }
      if (!spawn) throw new Error('No walkable land found after six regions');
      if (this.disposed) return;
      this.player.spawn(new THREE.Vector3(
        spawn.x - this.originX,
        spawn.y,
        spawn.z - this.originZ,
      ));
      this.emitStatus({
        ready: true,
        position: { x: spawn.x, y: spawn.y, z: spawn.z },
        message: 'New terrain ready',
      });
    } catch (error: unknown) {
      if (!this.disposed) {
        const message = error instanceof Error ? error.message : String(error);
        this.originX = previousOriginX;
        this.originZ = previousOriginZ;
        this.player.spawn(current);
        this.terrain.shiftOrigin(this.originX, this.originZ);
        this.terrain.update(currentX, currentZ, this.originX, this.originZ);
        this.emitStatus({ ready: true, message: `Teleport failed; previous location restored: ${message}` });
      }
    } finally {
      this.teleporting = false;
    }
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.running = false;
    this.renderer.setAnimationLoop(null);
    window.removeEventListener('resize', this.resize);
    window.removeEventListener('keydown', this.handleGlobalKeyDown);
    this.player?.dispose();
    this.terrain?.dispose();
    this.environment?.dispose();
    this.renderer.dispose();
    this.renderer.domElement.remove();
  }

  private animate = (): void => {
    if (!this.running || !this.player || !this.terrain || !this.environment) return;
    const delta = Math.min(this.clock.getDelta(), 1 / 20);
    this.player.update(delta);
    let local = this.player.getPosition();
    this.maybeRebase(local);
    local = this.player.getPosition(local);
    const absoluteX = local.x + this.originX;
    const absoluteZ = local.z + this.originZ;
    const groundElevation = this.terrain.groundElevation(absoluteX, absoluteZ);
    const heightAboveGround = groundElevation === undefined
      ? this.player.isFlying() ? Math.max(1.68, this.camera.position.y) : 1.68
      : Math.max(1.68, this.camera.position.y - groundElevation);
    this.terrain.update(absoluteX, absoluteZ, this.originX, this.originZ, heightAboveGround);
    this.environment.update(this.camera, this.clock.elapsedTime, heightAboveGround);
    this.renderer.render(this.scene, this.camera);

    this.frame += 1;
    this.frameTimeAccumulator += delta;
    this.frameTimeSamples += 1;
    if (this.frame % 10 === 0) {
      const terrainDistance = this.terrain.measureDistance(this.camera);
      const waterDistance = this.environment.measureWaterDistance(this.camera);
      const aimDistance = terrainDistance === null
        ? waterDistance
        : waterDistance === null
          ? terrainDistance
          : Math.min(terrainDistance, waterDistance);
      this.emitStatus({
        position: { x: absoluteX, y: local.y, z: absoluteZ },
        flying: this.player.isFlying(),
        flySpeed: this.player.getFlySpeed(),
        aimDistance,
        farCoverage: this.terrain.farCoverageStatus(),
      });
    }
    if (this.frameTimeSamples >= 180) this.adjustResolution();
  };

  private maybeRebase(local: THREE.Vector3): void {
    if (!this.terrain) return;
    const threshold = 2800;
    if (Math.abs(local.x) < threshold && Math.abs(local.z) < threshold) return;
    const size = this.terrain.chunkWorldSize;
    const shiftX = Math.round(local.x / size) * size;
    const shiftZ = Math.round(local.z / size) * size;
    this.originX += shiftX;
    this.originZ += shiftZ;
    this.player?.shift(-shiftX, -shiftZ);
    this.terrain.shiftOrigin(this.originX, this.originZ);
  }

  private adjustResolution(): void {
    const averageFps = this.frameTimeSamples / this.frameTimeAccumulator;
    const maxRatio = Math.min(window.devicePixelRatio, 1.5);
    const nextRatio = averageFps < 44
      ? Math.max(0.8, this.pixelRatio - 0.15)
      : averageFps > 57
        ? Math.min(maxRatio, this.pixelRatio + 0.1)
        : this.pixelRatio;
    if (Math.abs(nextRatio - this.pixelRatio) > 0.01) {
      this.pixelRatio = nextRatio;
      this.renderer.setPixelRatio(nextRatio);
      this.resize();
    }
    this.frameTimeAccumulator = 0;
    this.frameTimeSamples = 0;
  }

  private resize = (): void => {
    const width = this.mount.clientWidth;
    const height = this.mount.clientHeight;
    this.camera.aspect = width / Math.max(1, height);
    this.camera.updateProjectionMatrix();
    this.renderer.setSize(width, height, false);
  };

  private handleGlobalKeyDown = (event: KeyboardEvent): void => {
    if (event.code === 'KeyR' && !event.repeat && !event.metaKey && !event.ctrlKey && !event.altKey) {
      void this.teleportRandom();
    }
  };

  private emitStatus(update: Partial<GameStatus>): void {
    this.status = { ...this.status, ...update };
    this.onStatus(this.status);
  }
}
