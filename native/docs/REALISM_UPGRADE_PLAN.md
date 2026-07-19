# Native realism upgrade: tracked implementation plan

Status values: `[ ]` pending, `[~]` active, `[x]` implemented, `[M]` awaiting
manual visual/hardware validation, `[W]` waived by explicit user direction,
`[D]` deferred with a measured reason recorded below.

The definition of done remains the one supplied by the user, adapted to the
native SDL_GPU renderer. This ledger must be updated as implementation proceeds.

## Milestone 0 — audit and baseline

- [x] Map renderer, data contract, tile lifecycle, shaders, vegetation,
  atmosphere, shadows, floating origin, UI and diagnostics.
- [x] Verify current source resolutions and world/sample conversion.
- [x] Identify screen-space cloud defect and research native cloud references.
- [x] Record implementation deviations and validation constraints.
- [x] Capture build/runtime baseline counters for the current native client.

## Milestone 1 — metric, data and seam correctness

- [x] Add typed central physical/visual/streaming configuration with units.
- [x] Load and serialize tuned configuration as JSON.
- [x] Require exact elevation/climate binary payload sizes.
- [x] Request derivative-only border samples without changing visible extents.
- [x] Use physical central differences across tile boundaries.
- [x] Pack derived and biome fields without excessive per-tile GPU memory.
- [x] Make parent exclusion depend on finer GPU-ready coverage.
- [x] Add refine/coarsen hysteresis and remove parent/child intersections.
- [x] Add tile, source elevation, normal and derivative debug views.
- [M] Verify no same-LOD lighting seams, holes or terrain above the player.

## Milestone 2 — terrain classification and material

- [x] Derive physical slope and two useful curvature scales.
- [x] Derive climate/form-based wetness and ridge exposure.
- [x] Derive bounded forest, rock, scree and snow suitability.
- [x] Preserve fields continuously in global coordinates and across borders.
- [x] Implement grass/ground, forest floor, rock, scree, snow and wet soil.
- [x] Add slope-aware/triplanar-style rock and scree breakup.
- [x] Blend metre, local, macro and continental scales by projected detail.
- [x] Integrate distant forest coverage into terrain without tree-sized noise.
- [x] Use consistent linear shader math, an FP16 HDR target and an ACES filmic
  output pass.
- [M] Verify alpine terrain no longer reads as uniformly green.

## Milestone 3 — billboard forest overhaul

- [x] Reuse the same ecology masks for terrain and placement.
- [x] Add coherent stands, openings, exposed ridges and tree-line response.
- [x] Keep global deterministic candidates and cross-tile agreement.
- [x] Ground vertical cards using the final detailed interpolation and sink.
- [x] Improve atlas alpha cutoff/mips and foliage edge response.
- [x] Add wrapped diffuse, sky light and restrained transmission.
- [x] Add projected-pixel LOD/culling with stable deterministic thinning.
- [x] Add near, mid, sparse representative and terrain-only tiers.
- [x] Cull/batch spatial tile groups before submitting instance draws.
- [x] Restrict individual tree shadows to useful projected ranges.
- [M] Verify forest structure, brightness and distant noise manually.

## Milestone 4 — unified lighting and atmosphere

- [x] Introduce one environment state shared by every material.
- [x] Use common sun, sky radiance and exposure.
- [x] Add physical-scale terrain form/horizon lighting.
- [x] Keep expensive shadows near and macro occlusion far.
- [x] Replace duplicated fog with one analytic height-aware model.
- [x] Blend haze toward view-direction sky radiance.
- [x] Add valley-weighted aerial perspective and clearer high peaks.
- [x] Improve sun disk, horizon brightening, turbidity and orbital transition.
- [M] Verify progressive ridge depth and absence of a cyan cutoff.

## Milestone 5 — clouds and far-range quality

- [x] Replace camera-bound sky noise with world-anchored cloud volumes.
- [x] Add global weather coverage, base/detail erosion and wind advection.
- [x] Bound raymarch work and exit early on transmittance.
- [x] Evaluate the expensive above-cloud volume at quarter linear resolution and
  premultiplied-composite it into the FP16 scene target.
- [D] Temporal reprojection; the native renderer has no motion-vector/history
  path, and adding one now would introduce persistent ghosting and substantial
  cross-backend state for a pass already reduced to one-sixteenth pixel count.
- [x] Add broad global cloud shadows; the flat cirrus sheet was deliberately
  removed after fullscreen inspection exposed repeated height layers.
- [x] Preserve ridge/coast extrema through feature-aware server-side far LOD
  reduction without inventing new terrain.
- [x] Use source-scale far derivatives and macro-only distant material.
- [x] Keep exact spherical projection and circular planet-scale coverage.
- [M] Validate clouds on Apple Silicon and inspect high-altitude silhouettes.

## Milestone 6 — streaming, debug and polish

- [x] Split request/decode/derivation/vegetation/upload accounting.
- [x] Add request, job, upload-byte, terrain-memory and vegetation budgets.
- [x] Prioritize visible terrain, replacement children, camera view direction
  and travel direction.
- [x] Version derived products and reject stale generation/config results.
- [x] Release uploaded climate/vertex/index/instance payloads while retaining
  the elevation/range metadata needed for grounding and culling.
- [x] Retain uploaded tiles in a bounded 512-tile resident cache so a revisit
  does not repeat network, derivation and upload work.
- [x] Add mask, LOD, fog, shadow and vegetation debug modes.
- [x] Add live tuning controls with visible values and JSON persistence.
- [x] Add deterministic camera bookmarks for required visual scenarios.
- [x] Report frame time, 1% low estimate, draws, submitted vegetation,
  memory, request/decode/derive/upload timings and queue state.
- [x] Record fixed-seed screenshots and measurements on the user's M4 using
  native fullscreen capture at eye/canopy, aircraft and orbital viewpoints.
- [x] Update native setup, controls, architecture and assumptions docs.

## Validation tasks explicitly waived

- [W] Unit tests.
- [W] Automated seam tests.
- [W] Automated determinism tests.
- [W] Automated screenshot regression harness.

The corresponding properties still require runtime/manual evidence before the
goal can be completed.

## Deferred items and reasons

Temporal cloud reprojection is deferred because SDL_GPU exposes the necessary
textures but the renderer has no motion-vector/history validity system. A
quarter-linear-resolution FP16 volume pass gives one-sixteenth fragment count without the
camera-motion ghosting and history rejection complexity of temporal reuse.

## Current manual validation gate

The Release build and Metal runtime startup succeed, shaders/pipelines compile,
the spawn is grounded, and detailed/latent/orbital requests return valid
coordinates and HTTP 200 responses. Fixed-bookmark fullscreen inspection on the
user's M4 now covers canopy, 3 km, 30 km, orbital nadir and orbital horizon
views; evidence and measured counters are in
`VISUAL_AUDIT_2026-07-18.md`. The remaining `[M]` items require deliberately
selecting alpine and dense-forest seeds because the fixed audit seed starts in
open lowland grassland.
