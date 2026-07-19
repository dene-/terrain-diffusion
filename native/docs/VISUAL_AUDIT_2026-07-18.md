# Native visual audit — 2026-07-18

The Release Metal build was inspected in fullscreen on Apple Silicon with the
fixed Terrain Diffusion seed `6966025870760447731`. Camera bookmarks make each
height repeatable; bookmark 5 is orbital nadir and bookmark 6 is orbital
horizon.

## Captures

- [35 m canopy/coast](screenshots/visual-audit-canopy-35m.jpeg)
- [3 km flight](screenshots/visual-audit-flight-3km.jpeg)
- [30 km flight](screenshots/visual-audit-flight-30km.jpeg)
- [800 km curved horizon](screenshots/visual-audit-orbit-horizon-800km.jpeg)

Fullscreen counters after widening quality-equivalent latent/orbital tiles:

| Bookmark | Resident terrain | Draws | FPS |
|---|---:|---:|---:|
| 35 m | 38 rendered terrain tiles | 216 | 120 |
| 3 km | 70 rendered terrain tiles | 236 | 91 |
| 30 km | 57 rendered terrain tiles | 130 | 102 |
| 800 km horizon | 38 rendered terrain tiles | 42 | 120 |

The pre-widening 3 km and 30 km fullscreen measurements were approximately
43–48 FPS. The change preserves source spacing and mesh density; it only batches
more adjacent samples per network/decode/upload/draw unit.

## Defects found and corrected

- HDR was washed out because display-authored terrain/environment palettes were
  entering the FP16 target as linear values. Palette boundaries now decode
  sRGB exactly once; the final ACES pass encodes linear output once.
- The original cloud cards formed camera-bound white splotches and repeated at
  multiple heights. Clouds now use a global weather field, vertically sheared
  3-D density, variable local base/cap, bounded jittered ray marching, world
  wind and premultiplied composition.
- Fixed-step cloud marching produced horizontal layers at grazing angles.
  Segment-length-adaptive steps and stable interleaved jitter remove the bands.
- High-altitude water waves collapsed into concentric moire. Vertex and albedo
  ripples now fade by camera height and projected distance.
- The orbital world initially appeared as one small loaded island. Wider
  quality-equivalent orbital tiles reduced it to 50 terrain draws and achieved
  complete 5,603 km installed coverage for the 3,403 km requested horizon.
- Orbital space lacked an atmosphere. An analytic 90 km spherical shell now
  produces the blue limb using the same Earth radius as terrain and water.
- Automated pointer motion could leave the audit camera looking into the
  ground or space. The six deterministic bookmarks now include separate nadir
  and horizon views.

## Remaining observations

- The 30 m source makes some low-relief coastlines visibly angular at 35 m.
  L0 already renders at 7.5 m through interpolation; inventing additional
  height detail would violate the source-of-truth constraint. A future
  shoreline-only signed-distance material can soften the water/land boundary
  without changing elevation.
- At 3–30 km, atmosphere intentionally dominates contrast while the camera is
  inside or above the cloud layer. The debug `K` toggle isolates clouds when
  tuning terrain LOD and aerial perspective.
- The fixed seed's starting biome is open grassland, so this capture is not a
  dense-forest brightness reference. Forest/card validation still benefits
  from checking several generated seeds manually.

## Follow-up audit — 2026-07-19

The Release Metal build was re-inspected in fullscreen after reports of a blue
ring, shoreline loss, overhead tree crosses, broken shadow coverage and slow
revisits. Fixed evidence:

- [eye level](screenshots/current-fixes-eye-level.jpeg)
- [35 m canopy/coast](screenshots/current-fixes-canopy-35m.jpeg)
- [3 km inside the weather layer](screenshots/current-fixes-flight-3km.jpeg)
- [3 km with clouds isolated](screenshots/current-fixes-flight-3km-clear.jpeg)
- [160 m nadir before final card cutoff](screenshots/current-fixes-nadir-160m.jpeg)
- [160 m nadir after final card cutoff](screenshots/current-fixes-nadir-160m-final.jpeg)
- [near shadow-cascade debug](screenshots/current-fixes-shadow-debug.jpeg)

Corrections from this pass:

- Removed the cyan fade applied independently at every LOD outer radius. It
  was the direct cause of the circular blue bands; atmosphere is now solely
  continuous view/world-space extinction.
- Kept the finest detailed Terrain Diffusion footprint active under the camera
  at every altitude and weighted missing requests strongly toward view
  direction.
- Added a bounded 512-tile GPU-resident cache. Leaving the current wanted set
  no longer destroys a successfully uploaded tile.
- Removed geometric water displacement and moved small waves to fragment
  shading. The ocean sits 8 cm below sea level and cannot rise through land.
- Restricted each terrain shadow cascade to one source level, eliminating
  parent/child caster z-fighting.
- Removed individual vertical tree cards in steep nadir views. The captured
  pre-cutoff view showed the final thin-card failure mode; the built renderer
  now transitions fully to terrain-integrated forest coverage there. The final
  capture submits zero tree cards at 160 m and runs at 106 FPS.
- Measured the current world's continental elevation field at -7,494 m to
  +5,806 m. The synthetic conditioning cache had intentionally discarded the
  top 0.01 percent tail. Cache version 2 preserves the elevation raster's
  0.00001 percent tail (about 7.5 km in the 10-arc-minute source) while keeping
  1:1 metre scale and leaving climate tails conservative.

The water coastline is stable in the 35 m capture and the LOD-specific blue
ring is absent. The 3 km bookmark lies inside the configured 1.85–5.4 km cloud
layer; its near-white extinction is weather, not a terrain LOD boundary.
The shadow-cascade debug capture shows near-cascade occlusion after removing
overlapping parent caster meshes; its green base is the intentional cascade
visualizer, not final material colour.

### Remaining backend constraint

The native client now keeps a bounded GPU-resident terrain cache, so revisited
tiles return without another network, decode, or upload pass. Missing requests
are also strongly prioritized toward the camera view and travel direction.

The current Python Terrain Diffusion service still serializes generation around
its shared inference pipeline. An already-running uncached request cannot be
preempted by a newer, higher-priority request, so a rapid teleport can briefly
wait for stale server work to finish. Eliminating that delay requires request
cancellation or a preemptible/multi-worker inference service; it is not a
renderer cache miss.
