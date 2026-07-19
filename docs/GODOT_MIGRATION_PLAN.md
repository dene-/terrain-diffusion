# Terrain Stream Godot migration plan

Last updated: 2026-07-19

This is the durable implementation ledger for moving the native Terrain Stream
client from its custom SDL3/SDL_GPU renderer to Godot while retaining Terrain
Diffusion and the proven C++ streaming foundation.

## Resume protocol

1. Read this document in full at the start of every migration session and
   immediately after context compaction.
2. Read `native/docs/REALISM_UPGRADE_AUDIT.md` before changing an already
   implemented native subsystem.
3. Continue from **Current next action** unless the user's latest request
   changes priority.
4. Update checkboxes and the decision log in the same change that completes or
   defers work.

Status legend:

- `[ ]` pending
- `[~]` in progress
- `[x]` implemented or completed
- `[M]` awaiting manual visual/hardware validation
- `[W]` waived by explicit user direction
- `[D]` deliberately deferred; the reason must be recorded
- `[B]` blocked; the blocker and recovery step must be recorded

## Fixed scope and constraints

- Terrain Diffusion remains the sole source of large-scale elevation and
  climate. Do not replace it with procedural-noise terrain.
- Keep the Python Terrain Diffusion service and its random-access API.
- Port the playable native client to Godot Forward+ with a C++ GDExtension.
- Reuse renderer-independent C++ streaming, decoding, mesh, ecology, cache,
  and configuration logic where practical.
- Replace SDL window/input/render orchestration, custom GPU pipelines, custom
  HUD rendering, and engine-level sky/fog/shadow plumbing with Godot systems.
- Keep 1 Godot world unit = 1 physical metre near the player.
- Keep absolute world coordinates in C++ doubles and expose camera-relative
  float coordinates to Godot nodes. Do not require a custom double-precision
  Godot build for the first playable migration.
- Use detailed scale-8 source pages as the backing cache currently selected by
  the native client. A 4096 x 4096 source page is cache data, not one render
  mesh; render tiles remain segmented and independently streamed.
- Preserve deterministic world generation, ecological placement, LOD
  readiness rules, cache behavior, player scale, and floating origin.
- Trees remain billboard/impostor vegetation and are submitted in chunked
  `MultiMeshInstance3D` batches. Do not create a Godot node per tree.
- Automated and unit tests are out of scope by explicit user direction.
  Verification uses Release builds, Godot diagnostics, runtime logs,
  screenshots, performance counters, and user manual checks.
- Do not broaden or rewrite the current native renderer while migrating. Freeze
  it as the behavioral and visual reference except for renderer-independent
  extraction fixes required by the port.
- Another contributor may be changing clouds. Do not overwrite unrelated cloud
  work; integrate it only after terrain, atmosphere, and camera foundations are
  stable.

## Assumptions requiring confirmation during implementation

- Godot 4.7.1 is the initial target because it is the version installed and
  launched successfully on the development Mac. Pin `godot-cpp` and extension
  compatibility to 4.7 rather than silently building against another release.
- The Godot project will live in `godot/`; shared C++ will move toward
  `terrain_core/` only after dependency boundaries are mapped.
- Godot Forward+ is the primary renderer on macOS, Windows, and Linux.
- The Terrain Diffusion API continues to run on `127.0.0.1:8000` unless a
  documented port migration is made. This intentionally prevents enabling the
  Godot AI MCP bridge on its current hardcoded port 8000.
- Godot owns presentation and scene composition. C++ owns authoritative terrain
  data, streaming policy, cache identity, and high-volume mesh/instance packing.

## Tooling and research gate

- [x] Inspect existing project-local agent and MCP configuration.
- [x] Search the Skills registry for Godot project structure, GDScript,
  performance, GDExtension, and desktop-platform guidance.
- [x] Verify selected skill install count, source reputation, repository
  popularity, and published security audits.
- [x] Install `wshobson/agents@godot-gdscript-patterns` project-locally at
  `.agents/skills/godot-gdscript-patterns`.
- [x] Reject `zate/cc-godot@godot-development` because its published Gen Agent
  Trust Hub audit failed.
- [x] Record that no GDExtension/C++ skill found in the registry had comparable
  adoption and trust evidence. Use official Godot GDExtension documentation
  and existing C++ conventions instead of installing an unverified skill.
- [x] Research Godot MCP implementations for API lookup, scene inspection,
  live editor control, runtime screenshots, and cross-platform use.
- [x] Select Godot Forge `0.1.4` as the initial project MCP: small tool surface,
  Godot 4 documentation/search, scene/script analysis, LSP diagnostics, no
  network port collision, MIT license.
- [x] Add a disabled project-local Codex MCP entry. Enable it when
  `godot/project.godot` exists so startup does not fail during the pre-scaffold
  phase.
- [D] Add the richer Godot AI live-editor plugin and screenshot MCP. It is the
  preferred later visual-iteration bridge, but its documented MCP server is
  fixed to `127.0.0.1:8000`, which conflicts with the Terrain Diffusion API.
  Revisit after deciding whether to patch/vendor the bridge or move the terrain
  service to a configurable non-conflicting port.

Selected external tooling:

| Tool | Pinned source/version | Intended use |
|---|---|---|
| Godot GDScript Patterns | `wshobson/agents@godot-gdscript-patterns` | Scene architecture, typed GDScript glue, signals, allocation discipline |
| Godot Forge MCP | npm `godot-forge@0.1.4`, MIT | Godot 4 docs, project/scene/script analysis, LSP diagnostics |
| Godot AI MCP | `hi-godot/godot-ai`, candidate only | Future live editor manipulation, runtime input, screenshots |

## Repository-specific implementation map

### Reuse behind renderer-independent boundaries

| Current source | Responsibility retained |
|---|---|
| `native/src/TerrainTypes.hpp` | Tile keys, payloads, mesh and vegetation transfer types |
| `native/src/TerrainClient.*` | API requests, strict binary decode, scale-8 source-page cache |
| `native/src/TerrainStreamer.*` | LOD selection, async scheduling, readiness, resident cache and cancellation |
| `native/src/TerrainMesh.*` | Mesh construction, physical derivatives, biome masks and deterministic vegetation |
| `native/src/VisualConfig.*` | Typed physical/visual/streaming configuration and persistence |
| `terrain_diffusion/inference/api.py` | Authoritative terrain/climate service contract |

### Replace with Godot-owned systems

| Current source | Godot replacement |
|---|---|
| `native/src/Renderer.*` | `RenderingServer`, `ArrayMesh`, materials/shaders, `MultiMesh`, environment nodes |
| `native/src/Application.*` SDL lifecycle | Godot scene lifecycle plus thin GDExtension entry nodes |
| `native/src/CameraController.*` input shell | `CharacterBody3D`/`Camera3D` scene and typed input actions; C++ may retain absolute coordinates |
| `native/src/Hud.*` | Godot `Control` scene and theme |
| Native shadow/HDR/sky/fog/water orchestration | `DirectionalLight3D`, PSSM, `WorldEnvironment`, physical sky/fog, Godot shaders |
| Runtime HLSL compilation | Godot `.gdshader` resources or RenderingDevice shaders only where measured necessary |

### Target scene tree

```text
Main (Node)
|- WorldRoot (Node3D)
|  |- TerrainWorld3D (C++ GDExtension)
|  |  |- TerrainTiles (Node3D)
|  |  |- Vegetation (Node3D)
|  |  `- Ocean (MeshInstance3D)
|  |- Sun (DirectionalLight3D)
|  `- WorldEnvironment
|- Player (CharacterBody3D)
|  `- Camera3D
`- HUD (Control)
```

## Milestone 0 - freeze baseline and specify contracts

- [x] Record the exact current native build/run commands and fixed visual seed.
- [x] Capture baseline eye-level, canopy, 3 km, 30 km, nadir, and orbital
  screenshots using the existing bookmarks.
- [M] Record frame time, 1% low, draw counts, resident tiles, cache hits,
  request/decode/derive/upload timings, terrain memory, and vegetation memory.
- [x] Document current Terrain API request/response examples for `/world`,
  `/terrain`, `/terrain/lod`, and `/terrain/orbital` without changing them.
- [M] Confirm authoritative spacing and scale values from `/world` at runtime.
- [x] Define the minimal renderer-independent C++ interfaces required by Godot:
  terrain source, stream update, ready-tile extraction, cache stats, and
  absolute/local coordinate conversion.
- [x] Record ownership/lifetime rules for every buffer crossing into Godot.

Exit criteria:

- Native behavior and performance can be compared against the port.
- No Godot implementation depends directly on SDL or SDL_GPU types.

## Milestone 1 - Godot and GDExtension scaffold

- [x] Create `godot/project.godot` targeting Godot 4.7.1 Forward+.
- [x] Enable the pinned Godot Forge MCP entry in `.codex/config.toml`.
- [x] Add the official `godot-cpp` dependency pinned to an exact commit with
  explicit Godot 4.7 API generation because no 4.7 branch or tag exists yet.
- [x] Create a cross-platform CMake GDExtension build producing macOS,
  Windows, and Linux library names expected by one `.gdextension` resource.
- [x] Create the target scene tree with empty terrain, player, environment, and
  HUD nodes.
- [x] Register a minimal `TerrainWorld3D` C++ class and expose typed properties,
  signals, and read-only diagnostics.
- [x] Build the extension and launch the empty scene on Apple Silicon.
- [M] Confirm editor reloads the extension without stale-library crashes.

Exit criteria:

- Godot opens the project without errors.
- The native GDExtension loads on Apple Silicon and creates `TerrainWorld3D`.

## Milestone 2 - extract the renderer-independent terrain core

- [x] Introduce a small `terrain_core` target without SDL, SDL_GPU, or Godot
  dependencies.
- [x] Move metric configuration and tile/source types first.
- [x] Move `TerrainClient` network/decode/page-cache behavior.
- [x] Move `TerrainMesh` CPU generation and ecological placement.
- [x] Move `TerrainStreamer` scheduling/readiness/cache behavior.
- [x] Keep the native application building against the same extracted core as
  a regression reference.
- [x] Add an adapter that translates ready core mesh buffers to Godot arrays
  without per-vertex object allocation.
- [x] Add an adapter that bulk-packs vegetation transforms/custom data for
  `MultiMesh`.
- [x] Log tile state transitions and slow operations only on events, never per
  frame.

Exit criteria:

- Native and Godot clients consume the same source/decode/streaming logic.
- Core code has no engine-renderer dependency.

## Milestone 3 - one-tile vertical slice

- [x] Request one deterministic detailed tile from the existing Terrain API.
- [x] Decode it through `terrain_core` and verify physical extents in logs.
- [x] Create one `ArrayMesh` surface with indexed geometry and smooth physical
  normals.
- [x] Pack slope/curvature/wetness/forest/rock/scree/snow fields into Godot
  custom vertex channels or compact textures.
- [x] Upload the mesh on the Godot main thread through a bounded queue.
- [x] Render a minimal terrain shader with linear color handling.
- [x] Spawn the player at sampled terrain height plus 1.68 m eye height.
- [x] Add mouse-look, walk, run, jump, fly, wheel fly-speed, and aim-distance
  controls matching the native client.
- [M] Inspect scale, orientation, normals, grounding, and camera behavior.

Exit criteria:

- One Terrain Diffusion tile is playable at correct physical scale.
- No renderer-generated terrain substitute is present.

## Milestone 4 - streaming, LOD, floating origin and cache

- [ ] Connect `TerrainStreamer` to `TerrainWorld3D` update ticks.
- [ ] Create one Godot mesh node/resource per resident render tile, not per
  source page.
- [ ] Preserve parent visibility until the complete required child coverage is
  GPU-ready.
- [ ] Prevent simultaneous parent/child depth overlap.
- [ ] Preserve refine/coarsen hysteresis and camera/travel-direction priority.
- [ ] Treat the 4096 x 4096 scale-8 page as a CPU source cache and extract
  segment meshes on demand.
- [ ] Make cache-hit restoration avoid network and derivative recomputation.
- [ ] Apply absolute-double to camera-relative-float transforms consistently to
  terrain, player, vegetation, water, clouds, shadows, and debug overlays.
- [ ] Rebase the Godot scene origin without changing authoritative C++ world
  coordinates.
- [ ] Add bounded upload count and byte budgets per frame.
- [ ] Add event-driven diagnostics for gaps, overlaps, stale work, and slow
  cache restores.
- [M] Validate eye-level travel, high-speed flight, revisits, and seed changes.

Exit criteria:

- No cracks, holes, terrain above the player, parent/child z-fighting, or long
  cache stalls during ordinary traversal.

## Milestone 5 - terrain material and engine lighting

- [ ] Recreate grass/ground, forest floor, exposed rock, scree, snow, and wet
  soil masks from the shared core fields.
- [ ] Implement slope-aware triplanar rock/scree mapping.
- [ ] Use metre-scale, local, macro, and continental detail with distance-based
  frequency suppression.
- [ ] Integrate far-forest canopy color into the terrain material.
- [ ] Configure sRGB/linear texture usage, Godot tonemapping, exposure, and
  physically coherent sun/sky colors centrally.
- [ ] Add `DirectionalLight3D` with stabilized four-split PSSM tuned for nearby
  terrain and vegetation.
- [ ] Ensure terrain receives and casts shadows.
- [ ] Replace distant realtime shadows with shared macro occlusion/form
  lighting rather than extending PSSM over tens of kilometres.
- [ ] Configure exponential height fog/aerial perspective in
  `WorldEnvironment` and use matching fog behavior for custom shaders.
- [ ] Add debug modes for elevation, normals, derived masks, LOD, fog, and
  shadow cascades.
- [M] Inspect seams, HDR range, shadow stability, terrain form, and horizon
  layering at fixed bookmarks.

Exit criteria:

- Terrain no longer reads as uniformly green.
- Godot lighting/shadows/atmosphere replace the broken custom presentation
  paths without exposing tile boundaries.

## Milestone 6 - billboard forests and grass

- [ ] Create chunked spatial `MultiMeshInstance3D` groups per tile/LOD/material.
- [ ] Upload tree transforms, species, tint, size, and wind phase in bulk.
- [ ] Use alpha-tested foliage with depth writes and alpha-safe mip handling.
- [ ] Implement wrapped sun, sky contribution, restrained transmission, and
  shared atmospheric fading so trees do not become black silhouettes.
- [ ] Preserve deterministic ecology-driven stands, openings, tree line,
  exposed ridges, and cross-tile agreement.
- [ ] Preserve near, mid, sparse representative, and terrain-only forest tiers.
- [ ] Cull individual trees by projected pixel height with hysteresis.
- [ ] Limit tree shadow casting to useful near ranges; preserve terrain forest
  shading at distance.
- [ ] Add performant grass in nearby suitable areas only, with distance and
  density LODs that cannot reach eye height unexpectedly.
- [ ] Add debug visualization for vegetation chunk bounds, suitability, LOD,
  submitted counts, and shadow-casting counts.
- [M] Inspect dense forest at eye, canopy, valley, and aircraft views.

Exit criteria:

- Forests are coherent, grounded, lit, and dense without far-distance pin
  noise or unbounded instance cost.

## Milestone 7 - stable water and coastlines

- [ ] Start with one stable spherical/large-scale ocean surface and no wave
  displacement.
- [ ] Use reversed-Z-compatible depth handling and eliminate terrain/water
  z-fighting at coastlines.
- [ ] Add subtle normal-only multi-scale waves bounded by projected detail.
- [ ] Prevent waves from consuming terrain or changing the physical shoreline.
- [ ] Share sun, sky, fog, origin, and curvature transforms with terrain.
- [ ] Add distance-aware specular/roughness behavior without subpixel shimmer.
- [M] Inspect shoreline, low flight, 3 km, 30 km, and orbital views.

Exit criteria:

- Water reads as water at eye level and remains stable at extreme range.

## Milestone 8 - far terrain and orbital representation

- [ ] Preserve source ridges/coasts through feature-aware render LOD reduction.
- [ ] Select LOD by measured geometric error projected to screen pixels.
- [ ] Keep far normals and materials matched to the visible mesh frequency.
- [ ] Render macro biome/forest/rock/snow shading without individual tree noise.
- [ ] Preserve readiness-aware coverage in every direction while prioritizing
  the camera frustum and travel direction.
- [ ] Maintain a dedicated spherical far/orbital representation backed by
  Terrain Diffusion data; do not stretch a local flat clipmap to orbital scale.
- [ ] Transition between local floating-origin terrain and spherical far
  geometry without gaps, walls, double surfaces, or rectangular coverage.
- [ ] Confirm horizon distance/curvature from physical camera altitude.
- [M] Inspect at 1 km, aircraft altitude, 30 km, 160 km, and 800 km.

Exit criteria:

- Far geometry remains real Terrain Diffusion geometry and the world reads as a
  planet rather than a finite square.

## Milestone 9 - HUD, menus, input and tuning

- [ ] Recreate the native visual HUD as responsive Godot `Control` scenes.
- [ ] Keep title/seed, random terrain, entry/loading state, controls, human
  scale, coordinates, altitude, aim distance, LOD, loading, FPS, tree, and grass
  diagnostics.
- [ ] Add safe margins and scalable UI so text is never clipped.
- [ ] Preserve random-terrain teleport with safe non-water grounded spawning.
- [ ] Add a typed settings resource for terrain, forest, snow, atmosphere,
  lighting, streaming, water, and debug tuning.
- [ ] Serialize tuned values and expose only useful live controls.
- [ ] Preserve deterministic camera bookmarks and screenshot viewpoints.
- [M] Confirm keyboard/mouse behavior on macOS, Windows, and Linux mappings.

Exit criteria:

- The Godot client has feature parity with the useful native UI and controls.

## Milestone 10 - clouds and weather integration

- [ ] Audit the concurrently developed cloud work before importing or
  reimplementing anything.
- [ ] Prefer Godot-supported volumetric fog/compute or a measured low-resolution
  cloud pass rather than copying the custom renderer blindly.
- [ ] Anchor coverage and shadows in global coordinates, not camera space.
- [ ] Avoid repeated flat sheets at different heights.
- [ ] Use bounded raymarch cost, early exit, low-resolution rendering, and
  stable reconstruction only when visually justified.
- [ ] Share sun, sky, exposure, atmosphere, origin, and planet curvature.
- [ ] Add broad cloud shadows that correspond to visible cloud systems.
- [M] Inspect below, within, above, and orbital views at several sun angles.

Exit criteria:

- Clouds have coherent volume and scale, do not follow the camera, and stay
  within the measured performance budget.

## Milestone 11 - packaging, performance and handoff

- [ ] Produce reproducible Apple Silicon, Windows, and Linux build instructions.
- [ ] Package GDExtension libraries and Godot import assets correctly for each
  platform.
- [ ] Document the uv environment and Terrain API startup alongside Godot.
- [ ] Record final fixed-seed screenshots matching the native baseline.
- [ ] Record average frame time, 1% low, draw calls, visible instances, memory,
  network, decode, derive, vegetation, and upload measurements.
- [ ] Compare Godot results with the native baseline and record regressions.
- [ ] Remove dead migration adapters only after the Godot path has parity.
- [ ] Keep the native client available until the user accepts the Godot client.
- [M] Complete user manual acceptance on the M4 and at least one non-macOS
  platform.

Exit criteria:

- The Godot client is the documented default, preserves Terrain Diffusion
  fidelity, and is measurably stable and performant.

## Validation explicitly waived

- [W] Unit tests.
- [W] Automated seam tests.
- [W] Automated determinism tests.
- [W] Automated screenshot regression tests.

These properties still require runtime evidence; waiver does not mean they are
assumed correct.

## Current next action

**Milestone 3:** request and render one deterministic detailed Terrain
Diffusion tile through `terrain_core`, then inspect the grounded playable
camera against that authoritative mesh. The end-to-end data path is complete;
visual scale, grounding, and controls remain a manual checkpoint.

## Decision log

### 2026-07-19 - migrate presentation, retain terrain core

Godot replaces the custom rendering/window/input/presentation layer. The
Terrain Diffusion service and renderer-independent native C++ terrain pipeline
remain authoritative. This constrains the migration and avoids rewriting the
hardest working subsystem.

### 2026-07-19 - Forward+ and GDExtension

Use Godot Forward+ and C++ GDExtension. Built-in environment, PSSM shadows,
resource/scene management, editor tooling, and cross-platform packaging are the
reason for moving. RenderingDevice compute remains optional and must be
justified by profiling.

### 2026-07-19 - Godot 4.7.1 baseline

Use the installed Godot 4.7.1 editor and pin `godot-cpp` to compatible 4.7
bindings. The editor has been launched once successfully on the development
Mac.

### 2026-07-19 - exact godot-cpp commit with API 4.7

The official `godot-cpp` repository did not yet publish a `4.7` branch or tag.
Pin commit `ba0edfed90512ec64aba51d4295a3e7e30112f86`, whose CMake configuration
explicitly supports `GODOTCPP_API_VERSION=4.7`, rather than following an
unversioned moving branch.

### 2026-07-19 - baseline evidence gaps remain manual

Existing native screenshots and persisted HUD measurements establish the
visual baseline, but they do not contain 1% low, cache-hit, or complete timing
and memory dumps. The Terrain API was offline during the scaffold checkpoint,
so `/world` spacing confirmation and the missing counters remain marked for
manual capture instead of being guessed.

### 2026-07-19 - shared terrain_core target

Move the renderer-independent types, client, mesh derivation, visual
configuration, and streamer intact into `terrain_core`. Both the native SDL
application and Godot GDExtension link the same static library. The Godot
adapter converts ready mesh data through packed arrays and vegetation through
one `MultiMesh` float buffer per batch, preserving the no-object-per-tree
architecture.

### 2026-07-19 - one-tile Godot vertical slice

The first playable slice requests one deterministic scale-8 terrain tile on a
background C++ worker, transfers only plain core data across the thread
boundary, and creates Godot mesh, collision, material, and player grounding on
the main thread. `PlayerController` supplies metre-scaled walking, running,
jumping, flying, wheel speed adjustment, and aim-distance reporting. Godot
4.7.1 loads the extension, scene, shader, and custom classes without errors.
With seed `7851568387153385500`, the live slice produced a 480 m tile at 3.75 m
sample spacing with 17,153 vertices; the request took 54.3 ms and detailed-page
decode/cache preparation took 222.8 ms. Visual grounding remains marked for
manual validation.

### 2026-07-19 - editor/runtime lifecycle separation

Godot instantiates GDExtension scene nodes while editing the scene. Runtime
player polling consequently queried the editor InputMap and emitted repeated
missing-action errors, while the player's `_ready()` captured the editor
cursor. `PlayerController` and `TerrainWorld3D` now explicitly disable runtime
processing, input capture, and terrain requests under `Engine::is_editor_hint()`.
Editor-mode diagnostics are clean, while a separate game-mode run still loads
the deterministic scale-8 tile normally.

### 2026-07-19 - skill selection

Install the widely adopted, fully audited `godot-gdscript-patterns` skill for
Godot scene and glue code. No registry GDExtension skill had comparable trust
evidence, so C++ work follows official Godot documentation plus existing
project C++ practices.

### 2026-07-19 - MCP staging

Configure Godot Forge 0.1.4 first because it is project-aware, cross-platform,
small, and conflict-free. Defer the more capable Godot AI live bridge until its
port 8000 collision with the Terrain API has a deliberate resolution.

## Blockers and deferrals

- Godot AI MCP cannot coexist with the current Terrain API on the documented
  default loopback port. Recovery: either vendor a configurable-port revision
  or move the Terrain API behind a configurable port and update every client.
- Live Godot MCP tools cannot operate until `godot/project.godot` exists and the
  editor is installed/open. Godot Forge stays disabled until Milestone 1.
