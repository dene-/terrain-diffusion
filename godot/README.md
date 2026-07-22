# Terrain Stream Godot client

This directory is the new Godot 4.7.1 Forward+ presentation layer. Terrain
Diffusion and the renderer-independent C++ streaming core remain authoritative.

## Build the GDExtension on Apple Silicon

```bash
cmake -S godot/extension -B godot/extension/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build godot/extension/build --parallel
/Applications/Godot.app/Contents/MacOS/Godot --path godot --editor
```

Use a Debug build during migration because `reloadable = true` permits editor
hot reload. Use `-DCMAKE_BUILD_TYPE=Release` for performance captures once the
vertical slice is operational. Use separate Debug and Release build directories
because the chosen CMake build type also selects the matching `godot-cpp`
template ABI and output filename.

Restart the editor after adding or removing GDExtension methods or properties.
Hot reload can replace function implementations, but an already-open editor may
retain the previous native method table. The local terrain tool detects that
case and keeps the overview active until restart instead of calling stale APIs.

The pinned `godot-cpp` commit is fetched by CMake and generated against API
version 4.7. Generated libraries are placed in `godot/bin/`; they are build
artifacts and are not committed.

## Terrain sources and map switching

The client can stream either from the Terrain Diffusion HTTP API or from a
local `.tmap` archive. To choose a map in Godot, open `scenes/main.tscn`, select
`WorldRoot/TerrainWorld3D`, and assign its **Terrain Map** resource in the
Inspector. Map definitions live in `config/maps/`; create another `.tres` there
to add a switchable map. If its archive has not been packed yet, startup falls
back to the HTTP API and reports the missing path.

The selected definition provides two editor views. While an archive is absent,
an editor-only 1:200 overview is built from its 16-bit coarse heightmap and
biome image. The overview is 6,400 vertices, so the continent, islands and
mountain range remain visible without decoding 1,600 runtime chunks. Select
`EditorMapPreview` and press `F`, or select `TerrainWorld3D` and click **Frame
Map Preview** for the overhead view.

Once the selected `.tmap` exists, **Stream Local Map In Editor** replaces that
miniature with a real 1:1 local world. The editor camera becomes the observer
for the same wandering clipmap, screen-space-error LOD selection, terrain
shader and continuous water used by the game. The authoring preview deliberately
omits runtime vegetation batches, limits terrain to one paced upload at a time,
and uses a lighter ocean grid so it can coexist with the editor renderer without
stalling Metal. Click **Focus Local 1:1 Map** to move to dry land near the map
centre, and **Reload Local Map** after replacing an archive. Generated tiles are
transient and are removed when the preview is reloaded or disabled. They have
no scene owner, so Godot never serializes them and they safely remain alive
during an editor save. Place persistent meshes, scenes, markers and gameplay
objects under `WorldRoot/AuthoredWorld`, using stable map-centred metre
coordinates. Editor streaming deliberately disables
floating-origin rebasing; at runtime the controller offsets that container
whenever the player rebases, keeping authored objects aligned with streamed
terrain.

Godot's free editor camera has a 4 km far plane. The streamer still selects and
warms its LOD rings from physical camera altitude and screen-space error, but
the ordinary editor viewport cannot display terrain beyond that editor-camera
limit. Run the game for aircraft and full physical-horizon inspection.

Generate the detailed ecology layer from the final 30 m elevation and the
macro-climate foundation, then pack the completed 512 px chunk export:

```bash
./.venv/bin/python util_scripts/generate_ecology_map.py \
  generated/ocean_continent_seed_20260731_himalayan_600km_square_30m
./.venv/bin/python -m util_scripts.pack_terrain_map \
  generated/ocean_continent_seed_20260731_himalayan_600km_square_30m
```

The archive stores metadata plus a fixed random-access chunk index, so the
game decodes only nearby chunks instead of loading the complete map into RAM.
Version 3 archives include eight interleaved FP16 ecology channels at 120 m:
temperature mean and seasonality, precipitation mean and seasonality, soil
moisture, drainage, wind exposure, and substrate rockiness. The runtime
bilinearly samples these fields for terrain materials and deterministic tree
and grass placement without storing individual vegetation instances. The
four-channel macro climate remains embedded as a fallback; elevation-only
version 1 and macro-climate version 2 archives remain supported. For
diagnostics or one-off launches, `--terrain-map=/absolute/map.tmap` overrides
the catalog.

The authored map is centered at world origin. Beyond its bounds, terrain
becomes deep seabed while the continuous ocean follows the player to the
physical horizon. Camera views and flight are capped at 15 km above sea level;
orbital terrain is intentionally outside this game's scope.

Runtime terrain uses a dependency-aware wandering hierarchy: coarse coverage
must be resident before finer children can appear, requests are kept to a small
priority frontier during fast flight, and local archive chunks decode in
parallel. L0 retains the full 128-segment 3.75 m grid; coarser tiles retain 64
segments. Fine/coarse borders snap to the actual neighboring grid and share the
same canonical reconstructed elevation field, avoiding both holes and visible
vertical skirts.

`PhysicalAtmosphere` is the authoritative environment. Its sky and aerial
perspective integrate wavelength-dependent Rayleigh and aerosol extinction
through a spherical Earth atmosphere with 8.4 km and 1.25 km scale heights.
The terrain streamer supplies only viewer altitude and physical horizon depth,
so haze naturally weakens above the dense lower atmosphere rather than using a
camera-centred fog ramp.

The child directional light is the sun: at astronomical distance this is the
physically correct local representation. Its 0.53-degree angular diameter,
spectral color, energy, direction, and terrain shadows follow the shared solar
state. `solar_time_hours`, latitude, day of year, cycle enablement, and
`minutes_per_day` are editable on `PhysicalAtmosphere`. The default 96-minute
cycle advances in game and stays fixed in the editor.

Clouds are deliberately absent from this version. The next cloud system must
be a sun-lit volumetric world layer at physical altitude; no skybox cloud
texture or camera-following sheet is retained from Sky3D.

The Terrain Diffusion API remains the development/random-world fallback. Local
version 3 archives contain elevation, compact macro climate, and detailed
FP16 ecology used by procedural terrain materials and vegetation. Authored
water-depth layers remain a future map channel.
