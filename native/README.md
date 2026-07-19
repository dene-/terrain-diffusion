# Terrain Native

`terrain_native` is a programmed C++23 renderer for Terrain Diffusion. It uses
SDL's modern GPU API and selects Metal on macOS, Direct3D 12 or Vulkan on
Windows, and Vulkan on Linux. Terrain generation stays in the existing Python
process; the renderer streams binary height/climate tiles over localhost.

The native renderer includes:

- camera-relative coordinates, reversed-Z depth and exact spherical projection;
- detailed 7.5 m terrain near the player;
- concentric streamed LOD rings from the detailed decoder and its continuous
  latent elevation backbone, followed by climate-bearing continental geometry
  when terrain is viewed from orbital altitude;
- mesh skirts and dithered ring transitions;
- asynchronous network, mesh, normal and vegetation generation;
- an FP16 HDR scene target with linear lighting and an ACES filmic output pass;
- scale-aware biome materials with triplanar rock/scree, snow, wet ground,
  macro colour, and procedural eye-level detail without texture seams;
- continuous climate-driven forest stands and aggregated orbital canopy shading
  with spruce, birch, oak, acacia and
  tropical tree assets shared with the browser demo;
- projected-pixel tree LOD from crossed near cards through camera-facing and
  deterministically thinned far cards into terrain-only forest, plus
  alpha-tested tree shadows over detailed L0/L1 terrain;
- dense 64-blade instanced grass clusters with climate filtering, height patches,
  colour variation and GPU wind animation;
- non-overlapping close/far terrain shadow maps, altitude-integrated atmospheric extinction, a
  world-anchored eroded cloud volume with quarter-linear-resolution
  evaluation, orbital sky substitution and spherical water whose waves are
  shading-only and cannot cross coast geometry;
- human-scale walking, running, jumping and adjustable flight;
- a native HUD matching the browser demo: seed, stream progress, coordinates,
  controls, random terrain, far coverage, vegetation counts and rangefinder.

## macOS (Apple Silicon)

Install command-line tools, CMake and Ninja:

```bash
xcode-select --install
brew install uv cmake ninja
```

From the repository root, create the Python environment and start Terrain
Diffusion in terminal 1:

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

Configure and build the native engine in terminal 2:

```bash
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build native/build --parallel
./native/build/terrain_native.app/Contents/MacOS/terrain_native \
  --visual-config native/config/world_visuals.json
```

If CMake produces a normal executable rather than an app bundle for your
generator, run `./native/build/terrain_native` instead. The first configure is
longer because SDL and the pinned shader compiler are built locally. Later
builds are incremental.

## Windows

Install Visual Studio 2022 with **Desktop development with C++**, CMake, Git,
and `uv` from a PowerShell terminal:

```powershell
winget install --id=astral-sh.uv -e
```

From the repository root, create the locked Python environment and start the
Terrain Diffusion service in terminal 1:

```powershell
uv sync
uv run python -m terrain_diffusion api `
  xandergos/terrain-diffusion-30m `
  --batch-size 8 `
  --cache-size 2G `
  --port 8000
```

On an NVIDIA GPU, add `--dtype fp16` for substantially lower VRAM and memory
bandwidth. Then configure the native engine from a Developer PowerShell in
terminal 2:

```powershell
cmake -S native -B native/build -A x64
cmake --build native/build --config Release --parallel
.\native\build\Release\terrain_native.exe
```

SDL normally selects Direct3D 12. A Vulkan-capable driver can also be selected
by SDL when appropriate; the application shader source is cross-compiled at
startup for the selected backend.

## Linux

Install `uv` using its official standalone installer, then open a new shell:

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
```

Install a C++23 compiler, CMake, Ninja, Git, and the Vulkan loader/driver. On
Ubuntu-like distributions:

```bash
sudo apt install build-essential cmake ninja-build git libvulkan1 vulkan-tools \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxkbcommon-dev \
  libwayland-dev
```

Create the Python environment and start Terrain Diffusion in terminal 1:

```bash
uv sync
uv run python -m terrain_diffusion api \
  xandergos/terrain-diffusion-30m \
  --batch-size 8 \
  --cache-size 2G \
  --port 8000
```

For NVIDIA CUDA, add `--dtype fp16`. Configure and run the native Vulkan
client in terminal 2:

```bash
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build native/build --parallel
./native/build/terrain_native
```

## Controls

| Input | Action |
|---|---|
| Left click | Enter the world / capture the mouse |
| Mouse | Look |
| WASD | Move |
| Shift | Run / fast flight |
| Space | Jump / ascend |
| C or Ctrl | Descend |
| F | Toggle walking and flight |
| Mouse wheel | Adjust flight speed |
| R | Generate a new seeded world |
| G | Write a rate-limited diagnostic snapshot to the console |
| V | Cycle 14 terrain/biome/LOD/fog debug views |
| K | Toggle clouds |
| 1–6 | Fixed eye-level, canopy, 3 km, 30 km, 160 m nadir, and 800 km horizon bookmarks |
| T | Select the next live visual parameter |
| [ / ] | Decrease / increase the selected visual parameter |
| P | Save live tuning to the selected visual configuration JSON |
| Escape | Release or capture the mouse |

The in-world HUD reports FPS, position, distance to the terrain under the
reticle, fully installed/requested LOD radius, pending tile count and
vegetation counts. The random-terrain control in its top-right corner is also
clickable while the mouse is released. If the initial detailed region is
ocean, startup automatically refines a nearby land candidate or streams a
deterministic distant region instead of leaving the loading overlay active.

## Options

```text
terrain_native --server http://127.0.0.1:8000
               --width 1600 --height 900
               --visual-config native/config/world_visuals.json
               [--gpu-debug]
```

`--gpu-debug` enables backend validation and is intentionally off for normal
performance runs. `world_visuals.json` centralizes physical units, material
thresholds, forest ecology and pixel LODs, snow, atmosphere, lighting, clouds,
and streaming/network/upload/memory budgets.

## Runtime architecture and validation

The service is the sole source of large-scale elevation and climate. The
client validates each binary payload, builds a minimum viable terrain mesh,
derives border-aware physical normals and biome masks off the render thread,
uploads within per-frame byte/count budgets, then releases redundant CPU
payloads. Uploaded tiles remain in a bounded GPU-resident cache for immediate
revisits. Detailed, latent, and orbital tiles form circular, hysteretic LOD
annuli; a parent remains visible until its child footprint is GPU-resident.
The finest detailed Terrain Diffusion footprint is always requested beneath
the camera, while async requests are weighted strongly toward the camera view
direction and secondarily toward travel direction.

Press `G` when a visual defect is visible. The diagnostic includes the camera
transform, sampled ground height, altitude above ground, active terrain stack,
installed/requested radius, and pending count without logging every frame. The
HUD additionally reports averaged network, decode, derivative, vegetation, and
GPU-upload timings plus retained CPU memory. The complete audit and current
manual-validation ledger are in
[`docs/REALISM_UPGRADE_AUDIT.md`](docs/REALISM_UPGRADE_AUDIT.md) and
[`docs/REALISM_UPGRADE_PLAN.md`](docs/REALISM_UPGRADE_PLAN.md).
