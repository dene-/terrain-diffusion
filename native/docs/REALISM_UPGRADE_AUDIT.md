# Native realism upgrade: repository audit

Audit date: 2026-07-18

This document maps the supplied Terrain Stream realism plan onto the native
C++23 renderer. The original plan uses Three.js terminology; this project uses
SDL3/SDL_GPU and runtime-cross-compiled HLSL. The visual and physical intent is
preserved, but web-only mechanisms such as transferable `ArrayBuffer`s and
Three.js material flags are translated to native equivalents.

## Existing implementation map

| Area | Current implementation | Upgrade ownership |
|---|---|---|
| Application lifecycle and UI | `src/Application.*`, `src/Hud.*` | Debug modes, camera bookmarks, tuning input, performance counters |
| Physical/player scale | `src/Config.hpp`, `src/CameraController.*` | Central metric and visual configuration; keep 1 unit = 1 metre |
| Service contract and decoding | `src/TerrainClient.*`, `terrain_diffusion/inference/api.py` | Strict payload validation, padded derivative borders, source/resolution diagnostics |
| Tile state and async work | `src/TerrainStreamer.*` | Readiness-aware LOD replacement, hysteresis, request/upload/memory budgets, stale work accounting |
| CPU mesh and ecology derivation | `src/TerrainMesh.*` | Physical slope, curvature, wetness, rock, scree, forest and snow masks; globally deterministic placement |
| GPU tile resources and passes | `src/Renderer.*` | Packed biome attributes, readiness-aware draw masks, cloud resources, debug rendering, profiling |
| Terrain material | `shaders/terrain.*.hlsl` | Scale-aware biome material, triplanar-style rock breakup, macro form lighting, shared atmosphere/cloud shadow |
| Billboard trees and grass | `shaders/vegetation.*.hlsl`, `Renderer.cpp` | Projected-pixel LOD/culling, brighter foliage response, grounded cards, shared atmosphere |
| Sky and atmosphere | `shaders/sky.frag.hlsl` plus repeated fog functions | Unified environment, physical sky approximation, world-anchored volumetric clouds |
| Water | `shaders/water.*.hlsl` | Shared atmosphere and environment lighting |
| Shadows | two terrain shadow maps in `Renderer.cpp` | Preserve near/far terrain form; restrict tree casting; add low-frequency cloud shadow |
| Spherical/far rendering | vertex shaders and `TerrainStreamer::makeSpecs` | Preserve ridges and coherent atmospheric coverage across detailed, latent and orbital sources |

## Verified metric and data contract

- Active model: `xandergos/terrain-diffusion-30m` unless overridden at server startup.
- `/world` is authoritative and currently reports 30 m native, 240 m latent,
  and 11,520 m continental spacing.
- L0 requests use `scale=4`, therefore their render spacing is 7.5 m.
- Elevation is signed little-endian int16 and represents metres.
- Climate is four interleaved little-endian float32 values per sample:
  annual mean temperature, temperature seasonality, annual precipitation and
  precipitation seasonality.
- Tile origins are `tile index * segments * sample spacing`; adjacent tiles
  include the same edge coordinate.
- World, player and camera scale is one unit per metre. Vertical exaggeration
  remains 1.0 unless an explicit debug configuration changes it.
- Camera-relative X/Z coordinates, reversed-Z depth and exact tangent-sphere
  projection are already active.

## Existing strengths retained

- Terrain Diffusion remains the only elevation/climate source.
- Detailed, latent and orbital requests are deterministic and random-access.
- Network decode, mesh generation, normals and vegetation run on a worker.
- Vegetation candidates are based on global placement cells and seed, so tile
  generation order does not change candidate identity.
- Terrain and vegetation are batched by tile; no object-per-tree architecture.
- Near crossed cards, mid camera-facing cards, grass instancing, two shadow
  ranges, skirts, floating origin, spherical water and native HUD already exist.

## Correctness and design gaps found at audit time

This list is retained as the baseline that motivated the work. The resolution
table immediately below is the current state; the numbered findings are not a
description of the renderer after the upgrade.

1. Source payloads accept trailing or missing climate bytes instead of
   requiring the exact contract selected by the request.
2. The visible mesh consumes all returned samples; there is no derivative-only
   border. Edge normals therefore use one-sided estimates.
3. Terrain carries base color plus forest coverage only. Slope is recomputed
   from normals and curvature/wetness/rock/scree/snow are not explicit fields.
4. Parent LOD clipping is configured by radius but not by finer GPU readiness.
   Independent source heights can overlap or expose gaps while streaming.
5. LOD thresholds are fixed world rings rather than measured geometric error
   projected to pixels; altitude selects a minimum level heuristically.
6. Tree counts are range-based. There is no projected-height culling or
   hysteresis, and GPU work is still submitted for instances that the vertex
   shader later rejects.
7. Foliage has an improved ambient term but no explicit transmission, mip-aware
   cutoff, or stable screen-size transition.
8. Fog logic is duplicated in terrain, trees and water and blends mainly toward
   a fixed horizon color.
9. Sky clouds are screen-UV noise. They are camera-bound white patches rather
   than world-space volumes and cast no terrain shadow.
10. Streaming has a request count and uploads-per-frame limit, but no upload-byte,
    memory, derivation-stage or vegetation budget.
11. The HUD supports event-driven `G` diagnostics, but not mask/LOD visual modes,
    live tuning, bookmarks or memory/timing counters.
12. At audit time there was no HDR intermediate target or explicit tone-mapping
    pass. The SDL swapchain handled final presentation, so linear lighting and
    an explicit filmic curve had to be added consistently.

## Resolution map

| Audit finding | Native resolution |
|---|---|
| Payload validation | Exact elevation-only or elevation-plus-climate byte count; malformed payloads are rejected before mesh work. |
| Border derivatives | Requests include a derivative-only border; the visible mesh uses the interior and physical central differences use the border. |
| Terrain fields | Packed slope, local/macro curvature, wetness, macro variation, forest, rock, scree, and snow fields feed the material and debug modes. |
| Parent/child overlap | Parents are clipped only inside measured GPU-resident child coverage, with hysteretic circular LOD annuli and short dithered transitions. |
| LOD selection | Terrain bands use configurable screen-space error, altitude/horizon coverage, direction priority, and refine/coarsen hysteresis. |
| Tree LOD | Spatial ranges select crossed or camera-facing cards by projected pixels and deterministically thin representatives before terrain-only forest takes over. |
| Foliage response | Atlas RGB dilation, alpha testing, sky/wrapped sun light, transmission, terrain fog, grounded pivots, and near-only casting prevent black floating cutouts. |
| Atmosphere | Terrain, vegetation, water, and sky share one sun/sky/exposure state and the same height-aware aerial-perspective model. |
| Clouds | Screen-space patches were replaced by a global weather field and eroded 3D volume, early-exit raymarch, quarter-linear-resolution pass, cirrus, wind, and global cloud shadows. The field now evolves over time (sheared weather octave, drifting 3D/2D erosion coordinates), a mid-scale cells octave breaks systems into varied clump sizes with scattered fringe puffs, height-graded ambient darkens cloud bases, and a camera-faded fine octave wisps near edges. |
| Streaming budgets | Request, decode/derive/vegetation jobs, upload count/bytes, and CPU terrain/vegetation memory are explicitly budgeted and exposed in diagnostics. |
| Debug/tuning | Fourteen visual modes, event-driven diagnostics, six bookmarks, 25 live parameters, JSON persistence, timings, draw counts, and memory are available. |
| Output pipeline | The scene now renders into `R16G16B16A16_FLOAT`, uses linear lighting, then runs an ACES filmic and linear-to-sRGB presentation pass. |

## Cloud research and adaptation decision

Primary reference: [AmanSachan1/Meteoros](https://github.com/AmanSachan1/Meteoros)
(MIT), a Vulkan implementation of the Decima/Nubis cloud approach. Its useful
production ideas are a 2D weather field, shaped 3D density, bounded raymarch,
cone/light sampling, early exit and temporal reprojection. The project reports
under 3 ms at 1080p on a notebook GTX 1070 and documents evaluating only 1/16
of pixels per frame through reprojection, with a preliminary 5.97x speedup.

Secondary references:

- [adrianderstroff/realtime-clouds](https://github.com/adrianderstroff/realtime-clouds)
  for a compact Horizon-style OpenGL implementation.
- [SaschaWillems/Vulkan](https://github.com/SaschaWillems/Vulkan) for portable
  Vulkan/HLSL compute and 3D texture patterns.
- [Optimisations for Real-Time Volumetric Cloudscapes](https://arxiv.org/abs/1609.05344)
  for jittered low-step analytical integration and temporal accumulation.

The native implementation does not import another renderer. It uses HLSL
through SDL_shadercross, a world-anchored weather field, bounded cloud-shell
intersection, bounded steps, early transmittance exit, a quarter-linear-resolution FP16
render target, and a matching cheap 2D global cloud-shadow function in
terrain/vegetation/water shading. The shadow function samples the exact noise
realization the volume ray-marches (same hash, octaves, wind and evolution
drift), projected from the cloud mid-layer along the sun, so ground shadows
correspond to the clouds overhead and morph in sync with them. Temporal history
was deliberately not added: the renderer has no motion-vector/history-validity
path, while quarter-linear resolution already reduces the expensive pass to
one-sixteenth of the full-resolution pixel count without temporal ghosting.

## Current source and rendering assumptions

- The server's `/world` response is authoritative for native, latent, and
  continental spacing; the client does not hard-code a different model native
  resolution.
- Elevation values are physical metres. Vertical exaggeration is a visible,
  serialized debug/art setting and defaults to `1.0`.
- Annual temperature is used as supplied by the server; the client does not
  apply a second full lapse-rate correction.
- SDL_GPU selects Metal on the validated Apple Silicon build and keeps the
  HLSL source portable to Vulkan and D3D12 through SDL_shadercross.
- Alpha-to-coverage is not assumed because the current scene target is
  single-sampled; foliage uses alpha test, depth writes, atlas edge dilation,
  and distance-dependent representative LOD instead.

## Validation constraint

Automated tests are explicitly excluded by the user. Validation consists of
Release builds, shader compilation on Metal, runtime diagnostic logs, fixed
seed/camera bookmarks, user-provided screenshots, and measured CPU/GPU-facing
counters. Test tasks from the supplied generic plan are recorded as waived,
not silently treated as completed.
