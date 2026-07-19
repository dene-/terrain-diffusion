import * as THREE from 'three';
import type { CSM } from 'three/addons/csm/CSM.js';

export type TreeSpecies = 'spruce' | 'birch' | 'oak' | 'acacia' | 'tropical';

export type TreePlacement = {
  x: number;
  y: number;
  z: number;
  height: number;
  width: number;
  rotation: number;
  color: THREE.Color;
  species: TreeSpecies;
  detail: 'near' | 'far';
};

type SpeciesMeshes = {
  texture: THREE.Texture;
  clusterTexture: THREE.CanvasTexture;
  material: THREE.MeshStandardMaterial;
  farMaterial: THREE.MeshBasicMaterial;
  near: [THREE.InstancedMesh, THREE.InstancedMesh];
  far: THREE.InstancedMesh;
};

const MAX_NEAR_TREES_PER_SPECIES = 50_000;
const MAX_FAR_TREES_PER_SPECIES = 45_000;
const SPECIES: readonly TreeSpecies[] = ['spruce', 'birch', 'oak', 'acacia', 'tropical'];
const TEXTURES: Record<TreeSpecies, string> = {
  spruce: '/assets/spruce-v3.png',
  birch: '/assets/birch.png',
  oak: '/assets/oak.png',
  acacia: '/assets/acacia.png',
  tropical: '/assets/tropical.png',
};

export class Forest {
  private readonly geometry = new THREE.PlaneGeometry(1, 1).translate(0, 0.5, 0);
  private readonly speciesMeshes = new Map<TreeSpecies, SpeciesMeshes>();
  private readonly transform = new THREE.Object3D();
  private readonly viewer = new THREE.Vector2();

  constructor(scene: THREE.Scene, csm: CSM) {
    const loader = new THREE.TextureLoader();
    for (const species of SPECIES) {
      const clusterCanvas = document.createElement('canvas');
      clusterCanvas.width = 1024;
      clusterCanvas.height = 512;
      const clusterTexture = new THREE.CanvasTexture(clusterCanvas);
      clusterTexture.colorSpace = THREE.SRGBColorSpace;
      clusterTexture.anisotropy = 2;
      const texture = loader.load(TEXTURES[species], (loaded) => {
        this.drawTreeCluster(clusterCanvas, loaded.image as HTMLImageElement);
        clusterTexture.needsUpdate = true;
      });
      texture.colorSpace = THREE.SRGBColorSpace;
      texture.anisotropy = 4;
      const material = new THREE.MeshStandardMaterial({
        map: texture,
        alphaTest: 0.42,
        side: THREE.DoubleSide,
        roughness: 1,
        metalness: 0,
        emissive: 0x73806b,
        emissiveMap: texture,
        emissiveIntensity: 0.46,
      });
      csm.setupMaterial(material);
      const setupCsmShader = material.onBeforeCompile;
      material.onBeforeCompile = (shader, renderer) => {
        setupCsmShader(shader, renderer);
        shader.uniforms.uForestViewer = { value: this.viewer };
        shader.vertexShader = `uniform vec2 uForestViewer;
        ${shader.vertexShader}`;
        shader.vertexShader = shader.vertexShader.replace(
          '#include <begin_vertex>',
          `#include <begin_vertex>
          vec2 forestViewerDelta = instanceMatrix[3].xz - uForestViewer;
          float forestScaleY = max(0.001, length(instanceMatrix[1].xyz));
          transformed.y -= dot(forestViewerDelta, forestViewerDelta) / (12742000.0 * forestScaleY);`,
        );
      };
      material.customProgramCacheKey = () => 'curved-forest-v1';

      const farMaterial = new THREE.MeshBasicMaterial({
        map: clusterTexture,
        alphaTest: 0.36,
        side: THREE.DoubleSide,
        vertexColors: true,
        fog: true,
      });
      farMaterial.onBeforeCompile = (shader) => {
        shader.vertexShader = shader.vertexShader.replace(
          '#include <project_vertex>',
          `vec3 farOrigin = instanceMatrix[3].xyz;
          float farScaleX = length(instanceMatrix[0].xyz);
          float farScaleY = length(instanceMatrix[1].xyz);
          vec3 farDirection = normalize(vec3(cameraPosition.x - farOrigin.x, 0.0, cameraPosition.z - farOrigin.z));
          vec3 farRight = normalize(cross(vec3(0.0, 1.0, 0.0), farDirection));
          vec3 farWorldPosition = farOrigin
            + farRight * transformed.x * farScaleX
            + vec3(0.0, transformed.y * farScaleY, 0.0);
          vec2 farViewerDelta = farOrigin.xz - cameraPosition.xz;
          farWorldPosition.y -= dot(farViewerDelta, farViewerDelta) / 12742000.0;
          vec4 mvPosition = viewMatrix * vec4(farWorldPosition, 1.0);
          gl_Position = projectionMatrix * mvPosition;`,
        );
        shader.fragmentShader = shader.fragmentShader.replace(
          '#include <color_fragment>',
          `#include <color_fragment>
          // Cluster cards overlap heavily when viewed from above. Lift their
          // dark linear tones so the canopy reads as foliage, not a black mass.
          diffuseColor.rgb = min(
            vec3(1.0),
            pow(max(diffuseColor.rgb, vec3(0.0001)), vec3(0.42)) * 0.82
          );`,
        );
      };
      farMaterial.customProgramCacheKey = () => 'billboard-forest-cluster-v5';

      const near: SpeciesMeshes['near'] = [
        new THREE.InstancedMesh(this.geometry, material, MAX_NEAR_TREES_PER_SPECIES),
        new THREE.InstancedMesh(this.geometry, material, MAX_NEAR_TREES_PER_SPECIES),
      ];
      for (const plane of near) {
        plane.count = 0;
        plane.castShadow = true;
        plane.receiveShadow = true;
        plane.frustumCulled = false;
        plane.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
        scene.add(plane);
      }
      near[1].castShadow = false;
      const far = new THREE.InstancedMesh(this.geometry, farMaterial, MAX_FAR_TREES_PER_SPECIES);
      far.count = 0;
      far.castShadow = false;
      far.receiveShadow = false;
      far.frustumCulled = false;
      far.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
      scene.add(far);
      this.speciesMeshes.set(species, { texture, clusterTexture, material, farMaterial, near, far });
    }
  }

  setTrees(trees: TreePlacement[], originX: number, originZ: number): void {
    const grouped = new Map<TreeSpecies, { near: TreePlacement[]; far: TreePlacement[] }>();
    for (const species of SPECIES) grouped.set(species, { near: [], far: [] });
    for (const tree of trees) grouped.get(tree.species)![tree.detail].push(tree);

    for (const species of SPECIES) {
      const meshes = this.speciesMeshes.get(species)!;
      const placements = grouped.get(species)!;
      const nearTrees = placements.near;
      const farTrees = placements.far;
      const nearCount = Math.min(nearTrees.length, MAX_NEAR_TREES_PER_SPECIES);
      for (let index = 0; index < nearCount; index += 1) {
        const tree = nearTrees[Math.floor(index * nearTrees.length / nearCount)];
        for (let planeIndex = 0; planeIndex < meshes.near.length; planeIndex += 1) {
          this.transform.position.set(tree.x - originX, tree.y, tree.z - originZ);
          this.transform.rotation.set(0, tree.rotation + planeIndex * Math.PI / 2, 0);
          this.transform.scale.set(tree.width, tree.height, 1);
          this.transform.updateMatrix();
          meshes.near[planeIndex].setMatrixAt(index, this.transform.matrix);
          meshes.near[planeIndex].setColorAt(index, tree.color);
        }
      }
      for (const plane of meshes.near) {
        plane.count = nearCount;
        plane.instanceMatrix.needsUpdate = true;
        if (plane.instanceColor) plane.instanceColor.needsUpdate = true;
      }

      const farCount = Math.min(farTrees.length, MAX_FAR_TREES_PER_SPECIES);
      for (let index = 0; index < farCount; index += 1) {
        const tree = farTrees[Math.floor(index * farTrees.length / farCount)];
        this.transform.position.set(tree.x - originX, tree.y, tree.z - originZ);
        this.transform.rotation.set(0, tree.rotation, 0);
        this.transform.scale.set(tree.width, tree.height, 1);
        this.transform.updateMatrix();
        meshes.far.setMatrixAt(index, this.transform.matrix);
        meshes.far.setColorAt(index, tree.color);
      }
      meshes.far.count = farCount;
      meshes.far.instanceMatrix.needsUpdate = true;
      if (meshes.far.instanceColor) meshes.far.instanceColor.needsUpdate = true;
    }
  }

  updateViewer(localX: number, localZ: number): void {
    this.viewer.set(localX, localZ);
  }

  dispose(scene: THREE.Scene): void {
    for (const meshes of this.speciesMeshes.values()) {
      for (const plane of meshes.near) scene.remove(plane);
      scene.remove(meshes.far);
      meshes.material.dispose();
      meshes.farMaterial.dispose();
      meshes.texture.dispose();
      meshes.clusterTexture.dispose();
    }
    this.geometry.dispose();
  }

  private drawTreeCluster(canvas: HTMLCanvasElement, image: HTMLImageElement): void {
    const context = canvas.getContext('2d')!;
    context.clearRect(0, 0, canvas.width, canvas.height);
    const aspect = image.naturalWidth / Math.max(1, image.naturalHeight);
    const trees = [
      [0.1, 0.72, 0.03],
      [0.27, 0.94, 0],
      [0.46, 0.8, 0.07],
      [0.65, 0.98, 0],
      [0.84, 0.74, 0.11],
    ];
    for (const [centre, height, top] of trees) {
      const drawHeight = height * canvas.height;
      const drawWidth = drawHeight * aspect;
      context.drawImage(
        image,
        centre * canvas.width - drawWidth / 2,
        top * canvas.height,
        drawWidth,
        drawHeight,
      );
    }
  }
}
