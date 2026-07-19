import RAPIER from '@dimforge/rapier3d-compat';
import * as THREE from 'three';

const PLAYER_HEIGHT = 1.8;
const PLAYER_RADIUS = 0.32;
const CAPSULE_HALF_SEGMENT = (PLAYER_HEIGHT - PLAYER_RADIUS * 2) / 2;
const EYE_HEIGHT = 1.68;
const CAMERA_FROM_BODY_CENTER = EYE_HEIGHT - PLAYER_HEIGHT / 2;
const WALK_SPEED = 1.5;
const RUN_SPEED = 5.2;
const DEFAULT_FLY_SPEED = 24;
const GRAVITY = 9.81;
const JUMP_VELOCITY = 3.6;

export class PlayerController {
  private readonly body: RAPIER.RigidBody;
  private readonly collider: RAPIER.Collider;
  private readonly character: RAPIER.KinematicCharacterController;
  private readonly keys = new Set<string>();
  private readonly horizontalVelocity = new THREE.Vector2();
  private yaw = 0;
  private pitch = -0.12;
  private verticalVelocity = 0;
  private bobPhase = 0;
  private cameraBob = 0;
  private jumpQueued = false;
  private flying = false;
  private flySpeed = DEFAULT_FLY_SPEED;
  private spawned = false;

  constructor(
    private readonly physics: RAPIER.World,
    private readonly camera: THREE.PerspectiveCamera,
    private readonly canvas: HTMLCanvasElement,
    private readonly onFlyChange: (flying: boolean, flySpeed: number) => void,
    private readonly onLockChange: (locked: boolean) => void,
  ) {
    this.body = physics.createRigidBody(
      RAPIER.RigidBodyDesc.kinematicPositionBased().setTranslation(0, 4200, 0),
    );
    this.collider = physics.createCollider(
      RAPIER.ColliderDesc.capsule(CAPSULE_HALF_SEGMENT, PLAYER_RADIUS).setFriction(0),
      this.body,
    );
    this.character = physics.createCharacterController(0.04);
    this.character.setMaxSlopeClimbAngle(THREE.MathUtils.degToRad(42));
    this.character.setMinSlopeSlideAngle(THREE.MathUtils.degToRad(48));
    this.character.enableAutostep(0.42, 0.14, true);
    this.character.enableSnapToGround(0.28);

    window.addEventListener('keydown', this.handleKeyDown);
    window.addEventListener('keyup', this.handleKeyUp);
    window.addEventListener('mousemove', this.handleMouseMove);
    window.addEventListener('wheel', this.handleWheel, { passive: false });
    document.addEventListener('pointerlockchange', this.handleLockChange);
    canvas.addEventListener('click', this.requestPointerLock);
  }

  requestPointerLock = (): void => {
    void this.canvas.requestPointerLock();
  };

  spawn(localPosition: THREE.Vector3): void {
    this.body.setTranslation(localPosition, true);
    this.body.setNextKinematicTranslation(localPosition);
    this.verticalVelocity = 0;
    this.spawned = true;
    this.updateCamera();
  }

  hold(localPosition: THREE.Vector3): void {
    this.body.setTranslation(localPosition, true);
    this.body.setNextKinematicTranslation(localPosition);
    this.horizontalVelocity.set(0, 0);
    this.verticalVelocity = 0;
    this.spawned = false;
    this.updateCamera();
  }

  update(delta: number): void {
    if (!this.spawned) {
      this.physics.step();
      return;
    }

    const forward = Number(this.keys.has('KeyW')) - Number(this.keys.has('KeyS'));
    const strafe = Number(this.keys.has('KeyD')) - Number(this.keys.has('KeyA'));
    const length = Math.hypot(forward, strafe) || 1;
    const speed = this.flying ? this.flySpeed : this.keys.has('ShiftLeft') ? RUN_SPEED : WALK_SPEED;
    const sin = Math.sin(this.yaw);
    const cos = Math.cos(this.yaw);
    const targetX = ((-sin * forward + cos * strafe) / length) * speed;
    const targetZ = ((-cos * forward - sin * strafe) / length) * speed;
    const response = this.flying ? 7 : 11;
    this.horizontalVelocity.x = THREE.MathUtils.damp(this.horizontalVelocity.x, targetX, response, delta);
    this.horizontalVelocity.y = THREE.MathUtils.damp(this.horizontalVelocity.y, targetZ, response, delta);
    const dx = this.horizontalVelocity.x * delta;
    const dz = this.horizontalVelocity.y * delta;

    if (this.flying) {
      const up = Number(this.keys.has('Space')) - Number(this.keys.has('KeyC'));
      const movement = { x: dx, y: up * speed * delta, z: dz };
      const position = this.body.translation();
      this.body.setNextKinematicTranslation({
        x: position.x + movement.x,
        y: position.y + movement.y,
        z: position.z + movement.z,
      });
    } else {
      const grounded = this.character.computedGrounded();
      if (grounded && this.verticalVelocity < 0) this.verticalVelocity = -0.5;
      if (grounded && this.jumpQueued) this.verticalVelocity = JUMP_VELOCITY;
      this.jumpQueued = false;
      this.verticalVelocity -= GRAVITY * delta;

      this.character.computeColliderMovement(this.collider, {
        x: dx,
        y: this.verticalVelocity * delta,
        z: dz,
      });
      const movement = this.character.computedMovement();
      const position = this.body.translation();
      this.body.setNextKinematicTranslation({
        x: position.x + movement.x,
        y: position.y + movement.y,
        z: position.z + movement.z,
      });
    }

    this.physics.timestep = delta;
    this.physics.step();
    const moving = this.horizontalVelocity.length() > 0.25;
    const grounded = !this.flying && this.character.computedGrounded();
    if (moving && grounded) this.bobPhase += delta * (5.8 + this.horizontalVelocity.length() * 0.42);
    const targetBob = moving && grounded ? Math.sin(this.bobPhase) * 0.022 : 0;
    this.cameraBob = THREE.MathUtils.damp(this.cameraBob, targetBob, 14, delta);
    this.updateCamera();
  }

  getPosition(target = new THREE.Vector3()): THREE.Vector3 {
    const position = this.body.translation();
    return target.set(position.x, position.y, position.z);
  }

  isFlying(): boolean {
    return this.flying;
  }

  getFlySpeed(): number {
    return this.flySpeed;
  }

  shift(x: number, z: number): void {
    const position = this.body.translation();
    const next = { x: position.x + x, y: position.y, z: position.z + z };
    this.body.setTranslation(next, true);
    this.body.setNextKinematicTranslation(next);
  }

  dispose(): void {
    window.removeEventListener('keydown', this.handleKeyDown);
    window.removeEventListener('keyup', this.handleKeyUp);
    window.removeEventListener('mousemove', this.handleMouseMove);
    window.removeEventListener('wheel', this.handleWheel);
    document.removeEventListener('pointerlockchange', this.handleLockChange);
    this.canvas.removeEventListener('click', this.requestPointerLock);
    this.physics.removeCharacterController(this.character);
    this.physics.removeRigidBody(this.body);
  }

  private updateCamera(): void {
    const position = this.body.translation();
    this.camera.position.set(position.x, position.y + CAMERA_FROM_BODY_CENTER + this.cameraBob, position.z);
    this.camera.rotation.set(this.pitch, this.yaw, 0, 'YXZ');
  }

  private handleKeyDown = (event: KeyboardEvent): void => {
    this.keys.add(event.code);
    if (event.code === 'Space' && !event.repeat) this.jumpQueued = true;
    if (event.code === 'KeyF' && !event.repeat) {
      this.flying = !this.flying;
      this.verticalVelocity = 0;
      this.onFlyChange(this.flying, this.flySpeed);
    }
    if (document.pointerLockElement === this.canvas && ['Space', 'ArrowUp', 'ArrowDown'].includes(event.code)) {
      event.preventDefault();
    }
  };

  private handleKeyUp = (event: KeyboardEvent): void => {
    this.keys.delete(event.code);
  };

  private handleMouseMove = (event: MouseEvent): void => {
    if (document.pointerLockElement !== this.canvas) return;
    this.yaw -= event.movementX * 0.0017;
    this.pitch = THREE.MathUtils.clamp(this.pitch - event.movementY * 0.0017, -1.48, 1.48);
  };

  private handleLockChange = (): void => {
    this.onLockChange(document.pointerLockElement === this.canvas);
  };

  private handleWheel = (event: WheelEvent): void => {
    if (!this.flying) return;
    event.preventDefault();
    const unit = event.deltaMode === 1
      ? 16
      : event.deltaMode === 2
        ? window.innerHeight
        : 1;
    const delta = THREE.MathUtils.clamp(event.deltaY * unit, -240, 240);
    this.flySpeed = THREE.MathUtils.clamp(this.flySpeed * Math.exp(-delta * 0.003), 6, 240);
    this.onFlyChange(this.flying, this.flySpeed);
  };
}
