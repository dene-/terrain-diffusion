# Godot migration native baseline and core contract

Captured from commit `5a4e84da7f61dffc0324672d6099cd0ae363de0b`
on 2026-07-19. This freezes the custom native client as a comparison target;
the migration must not expand that renderer except where extracting shared
terrain code requires a small compatibility change.

## Build and run

The validated Apple Silicon Release commands are:

```bash
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build native/build --parallel
./native/build/terrain_native.app/Contents/MacOS/terrain_native \
  --visual-config native/config/world_visuals.json
```

The corresponding Terrain Diffusion service command is:

```bash
uv sync
PYTORCH_MPS_FAST_MATH=1 uv run python -m terrain_diffusion api \
  xandergos/terrain-diffusion-30m \
  --performance-preset mps \
  --batch-size 8 \
  --cache-size 2G \
  --log-mode info \
  --port 8000
```

Fixed visual seed: `6966025870760447731`.

Bookmarks `1` through `6` select eye level, 35 m canopy, 3 km, 30 km,
160 m nadir, and 800 km horizon views. The reference captures are stored in
[`native/docs/screenshots`](../native/docs/screenshots) and are indexed by
[`VISUAL_AUDIT_2026-07-18.md`](../native/docs/VISUAL_AUDIT_2026-07-18.md).

## Existing measured evidence

These fullscreen Apple Silicon measurements were recorded by the native visual
audit after widening quality-equivalent far tiles:

| Bookmark | Resident terrain | Draw calls | FPS | Mean frame time |
|---|---:|---:|---:|---:|
| 35 m canopy | 38 | 216 | 120 | 8.33 ms |
| 3 km | 70 | 236 | 91 | 10.99 ms |
| 30 km | 57 | 130 | 102 | 9.80 ms |
| 800 km horizon | 38 | 42 | 120 | 8.33 ms |

The native HUD also exposes request, decode, derivative, vegetation, upload,
CPU terrain memory, CPU vegetation memory, tree and grass counts. The existing
captures predate a persisted counter dump, so cache-hit count and 1% low remain
manual baseline measurements rather than guessed values. They must be captured
the next time the native service and client are intentionally run together.

## Authoritative Terrain API contract

The service remains at `http://127.0.0.1:8000` by default.

```text
GET /world
GET /terrain?i1=-4&j1=-4&i2=133&j2=133&scale=8&seed=6966025870760447731
GET /terrain/lod?i1=-1&j1=-1&i2=130&j2=130&stride=1&seed=6966025870760447731
GET /terrain/orbital?i1=-1&j1=-1&i2=130&j2=130&stride=1&seed=6966025870760447731
```

`/world` returns the seed and native, latent, and continental spacing in
metres. `/terrain` returns signed little-endian int16 elevation in metres,
optionally followed by four interleaved little-endian float32 climate channels.
The `X-Width` and `X-Height` response headers are authoritative. The exact
payload size is therefore:

```text
elevation only: width * height * 2
with climate:   width * height * (2 + 4 * 4)
```

The last live audit reported 30 m native, 240 m latent, and 11,520 m
continental spacing. Scale 8 therefore yields 3.75 m samples from the 30 m
model. The service was offline while this migration baseline was written, so
these values remain recorded evidence, not a new live confirmation.

## Minimal renderer-independent C++ boundary

Godot consumes the existing terrain pipeline through these responsibilities:

```cpp
struct TerrainSource {
    WorldInfo fetchWorld();
    RawTile fetchTile(TileKey, LodSpec, WorldInfo, Seed, TileFetchTiming*);
};

struct TerrainStream {
    void update(AbsoluteCameraPosition, AbsoluteViewDirection);
    std::vector<TerrainEvent> drainReady(UploadCountBudget, UploadByteBudget);
    void releaseUploadedPayload(TileKey);
    void regenerate(Seed);
    std::optional<float> sampleHeight(AbsoluteX, AbsoluteZ) const;
    StreamStats stats() const;
};

struct CoordinateFrame {
    LocalPosition toLocal(AbsolutePosition) const;
    AbsolutePosition toAbsolute(LocalPosition) const;
    RebaseDelta rebaseIfNeeded(AbsolutePosition);
};
```

The concrete first adapter may retain the current `TerrainClient` and
`TerrainStreamer` names. The boundary is defined by behavior, not by adding an
abstract-class hierarchy before one is needed.

## Ownership and lifetime

- `TerrainClient` owns downloaded page-cache storage and produces `RawTile`
  values with independent vectors.
- Worker threads exclusively construct a `TileMesh`; a completed mesh crosses
  the queue as `std::shared_ptr<TileMesh>`.
- Godot receives ready events on the main thread only. The adapter bulk-copies
  vertex/index data into Godot packed arrays and instance data into `MultiMesh`.
- Godot resources own GPU data after upload. The core retains only source/cache
  data required by its configured retention policy.
- `releaseUploadedPayload(key)` may discard redundant CPU render payloads only
  after the Godot upload has completed.
- Godot objects, `Ref` values, scene nodes, and RenderingServer resources are
  never created or mutated by terrain worker threads.
- Every asynchronous result carries its world generation identity. Results for
  an evicted tile, old seed, superseded LOD, or old derivation version are
  discarded before touching Godot state.
- Absolute positions remain C++ doubles. Only positions relative to the current
  floating origin cross into Godot's single-precision scene transform.

