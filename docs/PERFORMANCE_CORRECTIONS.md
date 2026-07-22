# Godot performance corrections

Baseline captured on 2026-07-20 on an Apple M4 at 1920x1080: 43-46 FPS,
23.7-29.8 ms sampled frames, 67.9 ms sampled worst frame, 95 draw calls,
2,964,708 primitives, 121 resident tiles, 70,756 resident tree instances, and
491,063 resident grass instances.

Status legend: `[ ]` pending, `[~]` implemented but not runtime-validated,
`[x]` implemented and validated with the evidence recorded below.

## Rendering

- [x] PERF-01 Replace the 800x800 ocean grid (about 1.28M triangles) with a
  bounded-cost grid and retain shader detail for nearby waves.
- [x] PERF-02 Stop forcing realtime sky radiance updates, render the procedural
  sky at half resolution, reduce cloud raymarch work, and throttle cloud motion
  uniform updates.
- [x] PERF-03 Configure the active Sky3D sun as a bounded two-split PSSM shadow
  map; retain realtime terrain shadows through 3 km without submitting coarse
  horizon rings to the shadow pass.
- [x] PERF-04 Remove the full-screen custom atmospheric fog pass.
- [x] PERF-05 Use bounded SSAO for near-field contact and concavity instead of
  flattening the entire terrain to save a post effect.
- [x] PERF-06 Remove water's screen-texture refraction/backbuffer dependency and
  skip foam noise outside foam-producing regions.
- [x] PERF-07 Stop requesting HDR output by default.
- [x] PERF-08 Replace fragment-discard terrain transitions with geometry-level
  LOD coverage that does not submit overlapping parent/child surfaces.
- [x] PERF-09 Reduce terrain shader ALU where it has no visible contribution,
  and replace aliased coarse vertex noise with one compressed mipmapped detail
  texture restricted to the near field.

## Streaming and simulation

- [x] PERF-10 Replace synchronous full-resolution concave terrain collision
  builds with bounded-cost local collision.
- [x] PERF-11 Make main-thread tile installation genuinely incremental and
  account for the full Godot-side upload cost before accepting work.
- [x] PERF-12 Remove redundant manual per-frame frustum work and rely on bounded
  Godot visibility ranges/culling unless a measured case requires otherwise.
- [x] PERF-13 Add indexed height lookup instead of scanning all resident tiles.
- [x] PERF-14 Avoid stable-state wanted-set scans and synchronization.
- [x] PERF-15 Throttle the long-distance aiming physics ray and run it only when
  the camera/aim state changes materially.

## Vegetation, memory, and loading

- [x] PERF-16 Use geometry-level vegetation LOD and compact visible MultiMesh
  batches; shader discard must not be treated as instance culling.
- [x] PERF-17 Reduce grass geometry and per-vertex shader cost.
- [x] PERF-18 Budget the resident cache using actual render/physics bytes rather
  than tile count and released CPU payload bytes.
- [x] PERF-19 Import the tree atlas as a compressed resource rather than building
  an uncompressed RGBA8 atlas at runtime.
- [x] PERF-20 Reuse HTTP connections across terrain tile requests.
- [x] PERF-21 Cache stable scene nodes used by the per-frame streaming and
  vegetation paths instead of repeatedly traversing NodePaths.

## Measurement

- [x] PERF-22 Add settled-window frame statistics, 1% low/worst-frame data,
  accurate submitted/resident counters, and resource-byte accounting.
- [x] PERF-23 Capture an optimized 1080p runtime baseline after corrections and
  verify the corrected sky, shadow, ocean, fog, SSAO, and vegetation states.

## Evidence log

- 2026-07-20: Initial baseline and static audit recorded from the current Godot
  log and active Godot/GDExtension render path.
- 2026-07-20: PERF-01 through PERF-07 implemented. Build and runtime validation
  remain pending; their status must not be promoted to `[x]` until both pass.
- 2026-07-20: PERF-10 and PERF-12 through PERF-15 implemented. Collision now
  uses a 2,048-triangle local shape instead of converting the 32,768-triangle
  render mesh; stable scheduling is capped at 1 Hz; height lookup is indexed;
  and the aim ray is movement-sensitive at no more than 10 Hz.
- 2026-07-20: PERF-09, PERF-16, PERF-17, and PERF-20 implemented. Terrain rock
  and micro breakup now reuse interpolated vertex fields; vegetation LOD is
  bounded by near/far batch ranges rather than fragment probability discard;
  grass density, blades, and wind trigonometry are reduced; and each terrain
  worker now retains a reusable HTTP client.
- 2026-07-20: PERF-11 implemented after an intermediate Metal run still measured
  a 21.9 ms L1 install and 6.2 ms L2 install. Streamed vegetation now installs
  one spatial batch at a time inside a 4 ms frame slice, with at most four
  tiles retaining pending payloads; L1-L5 terrain grids use 64 segments.
- 2026-07-20: A later streaming capture found remaining 5.5-10.1 ms L0
  installs. Visible mesh, shadow mesh, occluder, collision shape, tree batches,
  and grass rows are now distinct budgeted work units; validation was reset.
- 2026-07-20: PERF-18 implemented with per-tile estimates for expanded Godot
  vertex streams, indices, shadow meshes, occluders, collision faces,
  MultiMesh buffers, and render objects. Cache eviction now enforces the
  combined configured byte budget in addition to the tile-count limit.
- 2026-07-20: PERF-22 implemented as a 10-second sample after two fully settled
  seconds with no tile or vegetation work. It reports average/worst frame,
  worst-1% frame time, 1% low FPS, draw/primitive counters, and estimated
  resident render/physics bytes. The HUD labels render bytes explicitly.
- 2026-07-20: PERF-19 implemented with a reproducible atlas generator and a
  2560x1024 WebP imported as a mipmapped high-quality VRAM texture. Runtime
  image loading, resizing, blitting, mip generation, and RGBA8 upload were
  removed from `TerrainWorld3D`.
- 2026-07-20: PERF-08 implemented as an exclusive resident quadtree. A parent
  remains selected until all four children are resident and wanted, then the
  children replace it as geometry. Terrain ring uniforms, stochastic coverage,
  and fragment discard were removed.
- 2026-07-20: PERF-21 implemented by caching the permanent player, camera,
  terrain, vegetation, ocean, and sky nodes during `_ready()`. Incremental
  install stages now also emit a watchdog entry whenever one unit exceeds the
  full 4 ms frame slice.
- 2026-07-20: The new stage watchdog exposed a remaining 5.78 ms L2 visible
  mesh upload. L2-L5 grids were reduced from 64 to 48 segments while L0 stays
  at 128 and L1 at 64. Two subsequent cold-start Metal captures emitted no
  install-stage, visible-mesh, collision, shader, or runtime errors.
- 2026-07-20: Final optimized 1920x1080 Metal captures both settled at 120 FPS.
  The final capture measured 8.334 ms average, 8.881 ms worst, 119.23 FPS 1%
  low, 59 draws, 1,074,918 primitives, 119 resident tiles, and 86.87 MiB
  estimated resident render/physics memory. Both template-debug (O2) and
  template-release GDExtension builds completed successfully.
- 2026-07-20: Visual recovery audit invalidated the preceding "final" capture
  as a quality baseline. It had only 48 segments on L2-L5, a 1.5 km single
  shadow map, no SSAO, effectively absent vegetation, and no representative
  visual acceptance gate. L1-L5 are restored to 64 segments; PSSM shadows,
  bounded SSAO, near/far tree presentation, and crack-free parent/child
  replacement are active again.
- 2026-07-20: The resident-quadtree selection was closed over complete sibling
  groups. Previously an annular child could partially replace a parent, which
  exposed skirts as large walls and holes. Coarse parents now load first and
  are replaced only by all four resident children.
- 2026-07-20: Detailed streaming no longer fetches 4096-square scale-8 pages
  for every L0-L5 ring. Every detailed LOD evaluates one bounded canonical
  native-resolution field; the representative initial request fell from
  69-800 ms (with multi-page cache churn) to 1.4-2.8 ms. Shared parent/child
  coordinates now use the same interpolation convention.
- 2026-07-20: Fine render heights retain sub-metre interpolated values rather
  than quantizing every 3.75 m vertex to int16. Coarse normal derivatives use
  a weighted wider baseline, removing the contour/terrace lighting artifact
  without reducing mesh density.
- 2026-07-20: Settled 1080p runs directly timed the GDExtension at
  0.06-0.08 ms average and 0.43-0.77 ms worst. Whole-frame samples varied from
  7.47-9.18 ms on the development M4 (110-145 instantaneous FPS), with
  132-138 draws and 2.34-2.44M submitted primitives after restoring visual
  quality. Isolated 133 ms application-focus/OS stalls remain visible in the
  worst-frame metric and are not attributed to the extension.
