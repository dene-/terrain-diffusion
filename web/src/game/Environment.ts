import * as THREE from 'three';
import { CSM } from 'three/addons/csm/CSM.js';
import { Sky } from 'three/addons/objects/Sky.js';

const EARTH_RADIUS = 6_371_000;

function makeCloudTexture(): THREE.CanvasTexture {
  const canvas = document.createElement('canvas');
  canvas.width = 256;
  canvas.height = 128;
  const context = canvas.getContext('2d')!;
  context.clearRect(0, 0, canvas.width, canvas.height);
  const puffs = [
    [62, 75, 42], [102, 58, 54], [148, 70, 48], [190, 82, 34], [126, 84, 58],
  ];
  for (const [x, y, radius] of puffs) {
    const gradient = context.createRadialGradient(x, y, 0, x, y, radius);
    gradient.addColorStop(0, 'rgba(255,255,255,.9)');
    gradient.addColorStop(0.55, 'rgba(255,255,255,.56)');
    gradient.addColorStop(1, 'rgba(255,255,255,0)');
    context.fillStyle = gradient;
    context.fillRect(x - radius, y - radius, radius * 2, radius * 2);
  }
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  return texture;
}

export class WorldEnvironment {
  readonly csm: CSM;
  private readonly sky: Sky;
  private readonly water: THREE.Mesh;
  private readonly clouds = new THREE.Group();
  private readonly cloudTexture: THREE.CanvasTexture;
  private readonly fog: THREE.FogExp2;
  private readonly haze = new THREE.Color(0xdfe5e4);
  private readonly highAltitudeHaze = new THREE.Color(0xdbe4ea);
  private readonly measurementDirection = new THREE.Vector3();

  constructor(
    private readonly scene: THREE.Scene,
    camera: THREE.PerspectiveCamera,
    renderer: THREE.WebGLRenderer,
  ) {
    scene.background = this.haze;
    this.fog = new THREE.FogExp2(this.haze, 0.00018);
    scene.fog = this.fog;

    renderer.shadowMap.enabled = true;
    renderer.shadowMap.type = THREE.PCFSoftShadowMap;
    renderer.toneMapping = THREE.ACESFilmicToneMapping;
    renderer.toneMappingExposure = 1.05;
    renderer.outputColorSpace = THREE.SRGBColorSpace;

    this.sky = new Sky();
    this.sky.scale.setScalar(15_000);
    scene.add(this.sky);
    const uniforms = this.sky.material.uniforms;
    uniforms.turbidity.value = 4.2;
    uniforms.rayleigh.value = 1.45;
    uniforms.mieCoefficient.value = 0.004;
    uniforms.mieDirectionalG.value = 0.82;

    const sun = new THREE.Vector3().setFromSphericalCoords(
      1,
      THREE.MathUtils.degToRad(57),
      THREE.MathUtils.degToRad(224),
    );
    uniforms.sunPosition.value.copy(sun);

    this.csm = new CSM({
      camera,
      parent: scene,
      cascades: 4,
      maxFar: 6000,
      mode: 'practical',
      shadowMapSize: 2048,
      shadowBias: -0.00001,
      lightDirection: sun.clone().multiplyScalar(-1).normalize(),
      lightIntensity: 2.8,
      lightNear: 1,
      lightFar: 10_000,
      lightMargin: 240,
    });
    this.csm.fade = true;

    // A forest floor is lit mostly by diffuse sky and canopy bounce. Keep this
    // deliberately broad so CSM shadows retain shape without turning unlit
    // terrain and vegetation into black silhouettes.
    scene.add(new THREE.HemisphereLight(0xd8e9ff, 0x80745e, 2.05));

    const waterGeometry = new THREE.PlaneGeometry(1_700_000, 1_700_000, 192, 192);
    waterGeometry.rotateX(-Math.PI / 2);
    const waterPositions = waterGeometry.getAttribute('position');
    for (let index = 0; index < waterPositions.count; index += 1) {
      const x = waterPositions.getX(index);
      const z = waterPositions.getZ(index);
      waterPositions.setY(index, -(x * x + z * z) / (2 * EARTH_RADIUS));
    }
    waterPositions.needsUpdate = true;
    waterGeometry.computeVertexNormals();
    waterGeometry.computeBoundingSphere();
    this.water = new THREE.Mesh(
      waterGeometry,
      new THREE.MeshPhysicalMaterial({
        color: 0x315d72,
        roughness: 0.24,
        metalness: 0.05,
        transparent: true,
        opacity: 0.9,
        depthWrite: false,
      }),
    );
    this.water.position.y = 0.15;
    this.water.receiveShadow = false;
    scene.add(this.water);

    const cloudTexture = makeCloudTexture();
    this.cloudTexture = cloudTexture;
    const cloudMaterial = new THREE.SpriteMaterial({
      map: cloudTexture,
      transparent: true,
      opacity: 0.72,
      depthWrite: false,
      fog: true,
    });
    const random = this.seededRandom(34891);
    for (let index = 0; index < 72; index += 1) {
      const cloud = new THREE.Sprite(cloudMaterial.clone());
      const angle = random() * Math.PI * 2;
      const radius = 1600 + random() * 8200;
      cloud.position.set(Math.cos(angle) * radius, 900 + random() * 950, Math.sin(angle) * radius);
      const width = 260 + random() * 540;
      cloud.scale.set(width, width * (0.28 + random() * 0.12), 1);
      cloud.material.opacity = 0.42 + random() * 0.36;
      this.clouds.add(cloud);
    }
    scene.add(this.clouds);
  }

  setupTerrainMaterial(material: THREE.Material): void {
    this.csm.setupMaterial(material);
  }

  measureWaterDistance(camera: THREE.PerspectiveCamera): number | null {
    camera.getWorldDirection(this.measurementDirection);
    const direction = this.measurementDirection;
    const horizontalLengthSquared = direction.x * direction.x + direction.z * direction.z;
    const a = horizontalLengthSquared / (2 * EARTH_RADIUS);
    const b = direction.y;
    const c = camera.position.y - this.water.position.y;
    const candidates: number[] = [];
    if (a < 1e-10) {
      if (Math.abs(b) > 1e-8) candidates.push(-c / b);
    } else {
      const discriminant = b * b - 4 * a * c;
      if (discriminant >= 0) {
        const root = Math.sqrt(discriminant);
        candidates.push((-b - root) / (2 * a), (-b + root) / (2 * a));
      }
    }
    const maxWaterRadius = 850_000;
    let nearest = Number.POSITIVE_INFINITY;
    for (const distance of candidates) {
      if (
        distance > camera.near
        && distance <= camera.far
        && distance * Math.sqrt(horizontalLengthSquared) <= maxWaterRadius
      ) nearest = Math.min(nearest, distance);
    }
    return Number.isFinite(nearest) ? nearest : null;
  }

  update(camera: THREE.PerspectiveCamera, elapsed: number, heightAboveGround: number): void {
    const geometricHorizon = Math.sqrt(2 * EARTH_RADIUS * Math.max(1.68, heightAboveGround));
    const targetFar = THREE.MathUtils.clamp(geometricHorizon * 1.35, 8_000, 1_000_000);
    const nextFar = THREE.MathUtils.lerp(camera.far, targetFar, 0.045);
    if (Math.abs(camera.far - nextFar) > 20) {
      camera.far = nextFar;
      camera.updateProjectionMatrix();
    }
    const altitudeMix = THREE.MathUtils.smoothstep(heightAboveGround, 40, 5_000);
    const horizonHaze = THREE.MathUtils.smoothstep(heightAboveGround, 120, 1_200);
    this.haze.set(0xdfe5e4).lerp(this.highAltitudeHaze, altitudeMix);
    this.fog.color.copy(this.haze);
    this.fog.density = THREE.MathUtils.lerp(2, 3.4, horizonHaze) / Math.max(8_000, camera.far);
    this.sky.position.copy(camera.position);
    this.sky.scale.setScalar(Math.max(15_000, camera.far * 1.2));
    this.water.position.x = camera.position.x;
    this.water.position.z = camera.position.z;
    this.clouds.position.set(camera.position.x + elapsed * 1.3, 0, camera.position.z);
    this.csm.update();
  }

  dispose(): void {
    this.csm.dispose();
    this.scene.remove(this.sky, this.water, this.clouds);
    this.sky.geometry.dispose();
    this.sky.material.dispose();
    this.water.geometry.dispose();
    (this.water.material as THREE.Material).dispose();
    for (const cloud of this.clouds.children) {
      if (cloud instanceof THREE.Sprite) cloud.material.dispose();
    }
    this.cloudTexture.dispose();
  }

  private seededRandom(seed: number): () => number {
    let value = seed >>> 0;
    return () => {
      value = (value * 1664525 + 1013904223) >>> 0;
      return value / 0xffffffff;
    };
  }
}
