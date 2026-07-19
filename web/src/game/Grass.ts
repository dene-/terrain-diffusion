import * as THREE from 'three';
import type { CSM } from 'three/addons/csm/CSM.js';

export type SurfaceSample = {
  elevation: number;
  slope: number;
  temperature: number;
  precipitation: number;
  precipitationCv: number;
};

const MAX_PATCHES = 22_000;
const BLADES_PER_PATCH = 36;
const RADIUS = 100;
const SPACING = 1.2;
const REBUILD_DISTANCE = 12;

export class Grass {
  private readonly geometry = this.createTuftGeometry();
  private readonly material: THREE.MeshStandardMaterial;
  private readonly mesh: THREE.InstancedMesh;
  private readonly transform = new THREE.Object3D();
  private readonly color = new THREE.Color();
  private readonly dry = new THREE.Color(0xa99a52);
  private readonly lush = new THREE.Color(0x4d7f38);
  private readonly cold = new THREE.Color(0x6c8050);
  private lastX = Number.NaN;
  private lastZ = Number.NaN;
  private lastOriginX = Number.NaN;
  private lastOriginZ = Number.NaN;
  private invalidated = true;
  private windUniform?: { value: number };

  constructor(
    scene: THREE.Scene,
    csm: CSM,
    private readonly seed: number,
    private readonly sampleSurface: (absoluteX: number, absoluteZ: number) => SurfaceSample | undefined,
  ) {
    this.material = new THREE.MeshStandardMaterial({
      color: 0xffffff,
      roughness: 1,
      metalness: 0,
      side: THREE.DoubleSide,
      vertexColors: true,
      dithering: true,
    });
    csm.setupMaterial(this.material);
    const setupCsmShader = this.material.onBeforeCompile;
    this.material.onBeforeCompile = (shader, renderer) => {
      setupCsmShader(shader, renderer);
      this.windUniform = { value: 0 };
      shader.uniforms.uGrassTime = this.windUniform;
      shader.vertexShader = `uniform float uGrassTime;\n${shader.vertexShader}`;
      shader.vertexShader = shader.vertexShader.replace(
        '#include <begin_vertex>',
        `#include <begin_vertex>
        float grassTip = smoothstep(0.05, 1.0, position.y);
        float grassPhase = instanceMatrix[3].x * 0.075 + instanceMatrix[3].z * 0.052 + uGrassTime * 1.65;
        float grassGust = sin(grassPhase) + sin(grassPhase * 0.37 + uGrassTime * 0.72) * 0.34;
        transformed.x += grassGust * grassTip * 0.16;
        transformed.z += cos(grassPhase * 0.79) * grassTip * 0.11;`,
      );
      shader.fragmentShader = shader.fragmentShader.replace(
        '#include <opaque_fragment>',
        `outgoingLight = max(outgoingLight, diffuseColor.rgb * 0.36);
        #include <opaque_fragment>`,
      );
    };
    this.material.customProgramCacheKey = () => 'terrain-grass-wind-v3';

    this.mesh = new THREE.InstancedMesh(this.geometry, this.material, MAX_PATCHES);
    this.mesh.count = 0;
    this.mesh.castShadow = false;
    this.mesh.receiveShadow = true;
    this.mesh.frustumCulled = true;
    this.mesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
    scene.add(this.mesh);
  }

  update(absoluteX: number, absoluteZ: number, originX: number, originZ: number, elapsedTime: number): void {
    if (this.windUniform) this.windUniform.value = elapsedTime;
    const moved = Math.hypot(absoluteX - this.lastX, absoluteZ - this.lastZ) >= REBUILD_DISTANCE;
    const originChanged = originX !== this.lastOriginX || originZ !== this.lastOriginZ;
    if (!this.invalidated && !moved && !originChanged) return;

    this.lastX = absoluteX;
    this.lastZ = absoluteZ;
    this.lastOriginX = originX;
    this.lastOriginZ = originZ;
    this.invalidated = false;
    const centreX = Math.round(absoluteX / SPACING) * SPACING;
    const centreZ = Math.round(absoluteZ / SPACING) * SPACING;
    const cells = Math.ceil(RADIUS / SPACING);
    let count = 0;

    for (let z = -cells; z <= cells && count < MAX_PATCHES; z += 1) {
      for (let x = -cells; x <= cells && count < MAX_PATCHES; x += 1) {
        const gridX = Math.round(centreX / SPACING) + x;
        const gridZ = Math.round(centreZ / SPACING) + z;
        const jitterX = (this.hash(gridX + 31, gridZ - 17) - 0.5) * SPACING * 0.9;
        const jitterZ = (this.hash(gridX - 47, gridZ + 53) - 0.5) * SPACING * 0.9;
        const worldX = centreX + x * SPACING + jitterX;
        const worldZ = centreZ + z * SPACING + jitterZ;
        const distance = Math.hypot(worldX - absoluteX, worldZ - absoluteZ);
        if (distance > RADIUS) continue;

        const surface = this.sampleSurface(worldX, worldZ);
        if (!surface || surface.elevation < 3 || surface.slope > 0.52) continue;
        const warmEnough = THREE.MathUtils.smoothstep(surface.temperature, -10, 4);
        const notScorched = 1 - THREE.MathUtils.smoothstep(surface.temperature, 34, 47);
        const moisture = THREE.MathUtils.smoothstep(surface.precipitation, 70, 900)
          * (1 - 0.28 * THREE.MathUtils.clamp(surface.precipitationCv / 100, 0, 1));
        const growingClimate = warmEnough * notScorched;
        const desertFade = THREE.MathUtils.smoothstep(surface.precipitation, 35, 150);
        if (growingClimate * desertFade < 0.12) continue;
        const patch = this.valueNoise(worldX, worldZ, 68);
        const probability = THREE.MathUtils.clamp(
          growingClimate * desertFade * THREE.MathUtils.lerp(0.78, 0.995, moisture)
            + (patch - 0.5) * 0.16,
          0,
          0.995,
        );
        if (this.hash(gridX + 191, gridZ - 223) > probability) continue;

        const variation = this.hash(gridX - 83, gridZ + 109);
        const heightPatch = this.valueNoise(worldX, worldZ, 38) * 0.68
          + this.valueNoise(worldX, worldZ, 13) * 0.32;
        const height = THREE.MathUtils.lerp(0.28, 0.72, heightPatch)
          * THREE.MathUtils.lerp(0.82, 1.06, moisture)
          * THREE.MathUtils.lerp(0.9, 1.1, variation);
        this.transform.position.set(worldX - originX, surface.elevation - 0.025, worldZ - originZ);
        this.transform.rotation.set(0, this.hash(gridX + 7, gridZ + 13) * Math.PI, 0);
        const spread = THREE.MathUtils.lerp(0.88, 1.16, this.hash(gridX - 29, gridZ + 67));
        this.transform.scale.set(spread, height, spread);
        this.transform.updateMatrix();
        this.mesh.setMatrixAt(count, this.transform.matrix);

        this.color.copy(this.dry).lerp(this.lush, moisture);
        if (surface.temperature < 5) this.color.lerp(this.cold, 0.58);
        this.color.multiplyScalar(THREE.MathUtils.lerp(0.82, 1.08, this.hash(gridX + 71, gridZ - 37)));
        this.mesh.setColorAt(count, this.color);
        count += 1;
      }
    }

    this.mesh.count = count;
    this.mesh.instanceMatrix.needsUpdate = true;
    if (this.mesh.instanceColor) this.mesh.instanceColor.needsUpdate = true;
    this.mesh.computeBoundingSphere();
  }

  invalidate(): void {
    this.invalidated = true;
  }

  dispose(scene: THREE.Scene): void {
    scene.remove(this.mesh);
    this.geometry.dispose();
    this.material.dispose();
  }

  private createTuftGeometry(): THREE.BufferGeometry {
    const positions: number[] = [];
    const indices: number[] = [];
    const colors: number[] = [];
    const random = (value: number): number => {
      const result = Math.sin(value * 91.731 + 17.137) * 43_758.5453;
      return result - Math.floor(result);
    };
    for (let blade = 0; blade < BLADES_PER_PATCH; blade += 1) {
      const distributionAngle = blade * 2.399963 + random(blade + 1) * 0.7;
      const distributionRadius = Math.sqrt((blade + 0.35) / BLADES_PER_PATCH) * 0.72;
      const centreX = Math.cos(distributionAngle) * distributionRadius;
      const centreZ = Math.sin(distributionAngle) * distributionRadius;
      const facing = random(blade + 23) * Math.PI;
      const halfWidth = THREE.MathUtils.lerp(0.028, 0.062, random(blade + 47));
      const bladeHeight = THREE.MathUtils.lerp(0.58, 1, random(blade + 79));
      const sideX = Math.cos(facing) * halfWidth;
      const sideZ = Math.sin(facing) * halfWidth;
      const bend = THREE.MathUtils.lerp(-0.17, 0.17, random(blade + 113));
      const bendX = Math.cos(facing + Math.PI / 2) * bend;
      const bendZ = Math.sin(facing + Math.PI / 2) * bend;
      const start = positions.length / 3;
      positions.push(
        centreX - sideX, 0, centreZ - sideZ,
        centreX + sideX, 0, centreZ + sideZ,
        centreX + bendX, bladeHeight, centreZ + bendZ,
      );
      indices.push(start, start + 1, start + 2);
      const shade = THREE.MathUtils.lerp(0.78, 1.08, random(blade + 149));
      colors.push(
        shade * 0.7, shade * 0.78, shade * 0.66,
        shade * 0.7, shade * 0.78, shade * 0.66,
        shade, shade, shade * 0.9,
      );
    }
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    geometry.setAttribute('color', new THREE.Float32BufferAttribute(colors, 3));
    geometry.setIndex(indices);
    geometry.computeVertexNormals();
    geometry.computeBoundingSphere();
    return geometry;
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
    let value = Math.imul((x | 0) ^ this.seed, 374761393) + Math.imul(z | 0, 668265263);
    value = Math.imul(value ^ (value >>> 13), 1274126177);
    return ((value ^ (value >>> 16)) >>> 0) / 0xffffffff;
  }
}
