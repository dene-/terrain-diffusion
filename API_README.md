# Terrain API

Flask-based REST API for serving terrain and climate data from the terrain diffusion pipeline.

## Starting the Server

```bash
uv run python -m terrain_diffusion api xandergos/terrain-diffusion-30m --port 8000
```

### Configuration Options

- `model_path`: Hugging Face model name or local model path
- `--performance-preset mps`: Apple Silicon MPS, FP16, batching, and larger cache preset
- `--hdf5-file`: Optional HDF5 cache path
- `--port`: Server port (default: 8000, or `PORT` env var)
- `--host`: Server host (default: `0.0.0.0`)
- `--seed`: Random seed (default: from file or random)
- `--device`: Device (`cuda`, `mps`, or `cpu`; default: auto-detect)
- `--batch-size`: Latent generation batch size
- `--cache-size`: Direct cache size such as `2G`
- `--log-mode`: Logging mode (`info` or `verbose`, default: `verbose`)
- `--kwarg key=value`: Additional pipeline option, repeatable

## `GET /health`

Returns `{"status":"ok"}` once the HTTP process is available.

## `GET /world`

Return the active world seed plus native, latent and continental-stage horizontal
resolutions in metres.

**Response:**
```json
{"seed": "123456789", "native_resolution": 30, "latent_resolution": 240, "coarse_resolution": 11520}
```

## `GET /terrain`

Get terrain elevation and climate data for a bounding box.

**Query Parameters:**
- `i1`, `j1`, `i2`, `j2`: Bounding box coordinates in target resolution (required)
- `scale`: Integer upsampling factor relative to the selected model's native resolution
  (default: 1). For the 30m model, `scale=4` returns samples 7.5m apart.
- `climate`: Set to `0` to omit climate data and reduce interactive terrain latency
- `seed`: Optional integer world seed

**Example:**
```
GET /terrain?i1=0&j1=0&i2=256&j2=256&scale=1
```

**Response:**

Binary data with:
- **Elevation**: `int16` little-endian (H×W×2 bytes)
  - Values are in meters (floored) and clamped to [-32768, 32767]
- **Climate**: `float32` little-endian interleaved (H×W×4×4 bytes)
  - Channels: `temp`, `t_season`, `precip`, `p_cv`
  - `temp`: Annual Mean Temperature (C)
  - `t_season`: Temperature Seasonality (standard deviation ×100)
  - `precip`: Annual Precipitation (mm/year)
  - `p_cv`: Precipitation Seasonality (Coefficient of Variation, Percentage)
  - Equivalent to BIO1, BIO4, BIO12, and BIO15 from [WorldClim](https://www.worldclim.org/data/bioclim.html).
  - Layout: (H, W, 4) interleaved

**Response Headers:**
- `X-Height`: Output height in pixels
- `X-Width`: Output width in pixels

**Error Response:**
```json
{"error": "error message"}
```

## `GET /terrain/lod`

Return a decimated elevation tile from the generated latent elevation field used by the
detailed decoder. Coordinates are latent-grid indices. `stride` selects progressively
lower-density far LODs from the same field and may range from 1 through 64. The binary
layout and response headers are identical to the elevation portion of `GET /terrain`.

## `GET /terrain/orbital`

Return planet-scale elevation and climate from Terrain Diffusion's generated continental
stage. Coordinates are continental-grid indices and `stride` may range from 1 through 16.
The response uses the same interleaved elevation/climate binary layout as `GET /terrain`.
This endpoint supplies geometry only after detailed latent relief becomes smaller than a
screen pixel; it is not used for playable terrain or collision.

## Usage Example

```python
import requests
import numpy as np

# Request terrain data
response = requests.get(
    "http://localhost:8000/terrain",
    params={"i1": 0, "j1": 0, "i2": 256, "j2": 256, "scale": 1}
)

# Parse headers
h = int(response.headers["X-Height"])
w = int(response.headers["X-Width"])

# Parse elevation (int16)
elev_bytes = response.content[:h * w * 2]
elevation = np.frombuffer(elev_bytes, dtype="<i2").reshape(h, w)

# Parse climate (float32, 4 channels)
climate_bytes = response.content[h * w * 2:]
climate = np.frombuffer(climate_bytes, dtype="<f4").reshape(h, w, 4)
```
