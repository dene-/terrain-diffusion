import os
from typing import Optional, Tuple

import click

import numpy as np
import torch
from flask import Flask, Response, jsonify, request

from terrain_diffusion.common.device import select_device
from terrain_diffusion.inference.world_pipeline import WorldPipeline, resolve_hdf5_path
from terrain_diffusion.common.cli_helpers import apply_performance_preset, parse_cache_size

app = Flask(__name__)

_PIPELINE: Optional[WorldPipeline] = None
_PIPELINE_CONFIG: dict = {}


def _select_device() -> str:
    return select_device()


def _get_pipeline() -> WorldPipeline:
    global _PIPELINE
    if _PIPELINE is not None:
        return _PIPELINE

    cfg = _PIPELINE_CONFIG
    device = cfg.get('device') or _select_device()
    caching_strategy = cfg.get('caching_strategy', 'direct')
    _PIPELINE = WorldPipeline.from_pretrained(
        cfg.get('model_path', 'xandergos/terrain-diffusion-30m'),
        seed=cfg.get('seed'),
        latents_batch_size=cfg.get('latents_batch_size', 4),
        log_mode=cfg.get('log_mode', 'verbose'),
        torch_compile=cfg.get('torch_compile', False),
        dtype=cfg.get('dtype'),
        caching_strategy=caching_strategy,
        cache_limit=cfg.get('cache_limit', 100 * 1024 * 1024),
        **cfg.get('kwargs', {}),
    )
    _PIPELINE.to(device)
    hdf5_file = cfg.get('hdf5_file')
    if caching_strategy == 'direct':
        _PIPELINE.bind(hdf5_file=hdf5_file)
    else:
        _PIPELINE.bind(hdf5_file or 'TEMP')
    print(f"World seed: {_PIPELINE.seed}")
    return _PIPELINE


def _parse_quad() -> Tuple[int, int, int, int]:
    def _get_int(name: str) -> int:
        val = request.args.get(name, type=int)
        if val is None:
            raise ValueError(f"Missing required query param '{name}'")
        return val

    i1 = _get_int("i1")
    j1 = _get_int("j1")
    i2 = _get_int("i2")
    j2 = _get_int("j2")
    if i2 <= i1 or j2 <= j1:
        raise ValueError("Expected i2>i1 and j2>j1")
    return i1, j1, i2, j2


def _elev_to_int16(elev: torch.Tensor) -> np.ndarray:
    """Convert elevation (meters) to int16, clamped."""
    arr = elev.detach().cpu().numpy().astype(np.float32, copy=False)
    trans = np.floor(arr)
    return np.clip(trans, -32768, 32767).astype('<i2', copy=False)


def _binary_response(elev: torch.Tensor, climate: Optional[torch.Tensor]) -> Response:
    """
    Binary response format:
      - elevation: int16 little-endian (H*W*2 bytes)
      - climate: 4 channels of float32 little-endian (H*W*4*4 bytes)
        [temp, t_season, precip, p_cv]
    """
    elev_i16 = _elev_to_int16(elev)
    h, w = elev_i16.shape
    payload = elev_i16.tobytes()

    if climate is not None and climate.shape[0] >= 4:
        # climate shape: (4, H, W) -> transpose to (H, W, 4) for interleaved layout
        climate_np = climate[:4].detach().cpu().numpy().astype('<f4', copy=False)
        climate_np = np.transpose(climate_np, (1, 2, 0))  # (H, W, 4)
        payload += climate_np.tobytes()

    resp = Response(payload, mimetype="application/octet-stream")
    resp.headers["X-Height"] = str(h)
    resp.headers["X-Width"] = str(w)
    return resp


def _get_terrain(
    world: WorldPipeline,
    i1: int,
    j1: int,
    i2: int,
    j2: int,
    scale: int,
    with_climate: bool = True,
) -> dict:
    """
    Get terrain data at arbitrary scale.
    
    Args:
        world: WorldPipeline instance
        i1, j1, i2, j2: Coordinates in target (scaled) resolution
        scale: Scale factor relative to native resolution (1 = native, 2 = 2x, etc.)
    
    Returns dict with 'elev' (H, W) and 'climate' (4, H, W) tensors.
    """
    if scale == 1:
        # Native - just fetch directly
        out = world.get(i1, j1, i2, j2, with_climate=with_climate)
        return {"elev": out["elev"], "climate": out.get("climate")}

    # Convert target coordinates to native resolution space
    i1_native = i1 // scale
    j1_native = j1 // scale
    i2_native = -(-i2 // scale)  # ceil division
    j2_native = -(-j2 // scale)

    # Add 1 pixel padding for bilinear interpolation edge handling
    i1_native_pad = i1_native - 1
    j1_native_pad = j1_native - 1
    i2_native_pad = i2_native + 1
    j2_native_pad = j2_native + 1

    out_native = world.get(
        i1_native_pad,
        j1_native_pad,
        i2_native_pad,
        j2_native_pad,
        with_climate=with_climate,
    )
    elev_native = out_native["elev"]
    climate_native = out_native.get("climate")

    # Compute output size
    out_h = i2 - i1
    out_w = j2 - j1

    # Upsample elevation using bilinear interpolation
    elev_up = torch.nn.functional.interpolate(
        elev_native.unsqueeze(0).unsqueeze(0),
        scale_factor=scale,
        mode='bilinear',
        align_corners=False
    ).squeeze()

    # Calculate crop indices
    pad_up = scale  # 1 pixel padding in native = scale pixels in upsampled space
    offset_i = i1 - i1_native * scale
    offset_j = j1 - j1_native * scale
    crop_i1 = pad_up + offset_i
    crop_j1 = pad_up + offset_j

    elev = elev_up[crop_i1:crop_i1 + out_h, crop_j1:crop_j1 + out_w]

    climate = None
    if climate_native is not None:
        climate_up = torch.nn.functional.interpolate(
            climate_native.unsqueeze(0),
            scale_factor=scale,
            mode='bilinear',
            align_corners=False
        ).squeeze(0)
        climate = climate_up[:, crop_i1:crop_i1 + out_h, crop_j1:crop_j1 + out_w]

    return {"elev": elev, "climate": climate}


def _get_latent_terrain(
    world: WorldPipeline,
    i1: int,
    j1: int,
    i2: int,
    j2: int,
    stride: int,
) -> dict:
    """Read Terrain Diffusion's generated low-frequency elevation field.

    Coordinates are in latent pixels. Each latent pixel represents
    ``native_resolution * latent_compression`` metres. Unlike ``world.coarse``,
    this is the elevation component used by the detailed decoder itself.
    """
    weighted = world.latents[:, i1:i2, j1:j2]
    values = weighted[:-1] / (weighted[-1:] + 1e-8)
    elevation_sqrt = values[4] * 38.6 - 31.4
    elevation = torch.sign(elevation_sqrt) * torch.square(elevation_sqrt)
    return {"elev": _feature_preserving_decimate(elevation, stride), "climate": None}


def _feature_preserving_decimate(field: torch.Tensor, stride: int) -> torch.Tensor:
    """Reduce a generated field without erasing its strongest landforms.

    The mean keeps adjacent globally aligned blocks continuous. A bounded
    offset toward whichever local extreme differs most from that mean retains
    important ridges, valleys and coastlines better than point sampling while
    remaining strictly inside the source block's value range.
    """
    if stride == 1:
        return field
    source = field.unsqueeze(0).unsqueeze(0)
    average = torch.nn.functional.avg_pool2d(
        source, kernel_size=stride, stride=stride, ceil_mode=True,
        count_include_pad=False,
    )
    maximum = torch.nn.functional.max_pool2d(
        source, kernel_size=stride, stride=stride, ceil_mode=True,
    )
    minimum = -torch.nn.functional.max_pool2d(
        -source, kernel_size=stride, stride=stride, ceil_mode=True,
    )
    high_delta = maximum - average
    low_delta = minimum - average
    strongest_delta = torch.where(high_delta >= -low_delta, high_delta, low_delta)
    return (average + strongest_delta * 0.28).squeeze(0).squeeze(0)


def _average_decimate(field: torch.Tensor, stride: int) -> torch.Tensor:
    if stride == 1:
        return field
    return torch.nn.functional.avg_pool2d(
        field.unsqueeze(0), kernel_size=stride, stride=stride,
        ceil_mode=True, count_include_pad=False,
    ).squeeze(0)


def _get_orbital_terrain(
    world: WorldPipeline,
    i1: int,
    j1: int,
    i2: int,
    j2: int,
    stride: int,
) -> dict:
    """Read the generated continental stage for planet-scale geometry.

    One coarse cell spans 48 latent pixels. This stage is deliberately used
    only beyond the detailed latent rings, where sub-kilometre relief is below
    a screen pixel but coherent continents and climate must remain visible.
    """
    weighted = world.coarse[:, i1:i2, j1:j2]
    values = weighted[:-1] / (weighted[-1:] + 1e-8)
    elevation_sqrt = values[0]
    elevation = torch.sign(elevation_sqrt) * torch.square(elevation_sqrt)
    climate = torch.stack((values[2], values[3], values[4], values[5]))
    return {
        "elev": _feature_preserving_decimate(elevation, stride),
        "climate": _average_decimate(climate, stride),
    }


@app.get("/health")
def health():
    return jsonify({"status": "ok"})


@app.get("/world")
def world_info():
    world = _get_pipeline()
    return jsonify({
        "seed": str(world.seed),
        "native_resolution": world.native_resolution,
        "latent_resolution": world.native_resolution * world.latent_compression,
        "coarse_resolution": world.native_resolution * world.latent_compression * 48,
    })


@app.get("/terrain")
def terrain():
    """
    Get terrain data at arbitrary scale.
    
    Query params:
        i1, j1, i2, j2: Bounding box in target resolution coordinates
        scale: Integer scale factor relative to native resolution (default: 1)
               1 = native, 2 = 2x, 4 = 4x, 8 = 8x, etc.
        climate: Set to 0 to omit climate channels (default: 1)
        seed: World seed (optional; changes seed when different from current)
    
    Returns binary data:
        - elevation: int16-le (H*W*2 bytes), meters
        - climate: float32-le interleaved (H*W*4*4 bytes)
                   channels: temp, t_season, precip, p_cv
    """
    try:
        i1, j1, i2, j2 = _parse_quad()
        scale = request.args.get("scale", default=1, type=int)
        with_climate = request.args.get("climate", default=1, type=int) != 0
        if scale < 1:
            raise ValueError("scale must be >= 1")

        world = _get_pipeline()
        seed = request.args.get("seed", type=int)
        if seed is not None and world.change_seed(seed):
            print(f"World seed changed to: {world.seed}")
        out = _get_terrain(world, i1, j1, i2, j2, scale, with_climate=with_climate)
        return _binary_response(out["elev"], out.get("climate"))
    except Exception as e:
        return jsonify({"error": str(e)}), 400


@app.get("/terrain/lod")
def latent_terrain_lod():
    """Return decimated elevation from the detailed pipeline's latent stage."""
    try:
        i1, j1, i2, j2 = _parse_quad()
        stride = request.args.get("stride", default=1, type=int)
        if stride < 1 or stride > 64:
            raise ValueError("stride must be between 1 and 64")
        if i2 - i1 > 2049 or j2 - j1 > 2049:
            raise ValueError("latent LOD source bounds may not exceed 2049 pixels per axis")
        world = _get_pipeline()
        out = _get_latent_terrain(world, i1, j1, i2, j2, stride)
        return _binary_response(out["elev"], None)
    except Exception as e:
        return jsonify({"error": str(e)}), 400


@app.get("/terrain/orbital")
def orbital_terrain_lod():
    """Return climate-bearing continental geometry for orbital rendering."""
    try:
        i1, j1, i2, j2 = _parse_quad()
        stride = request.args.get("stride", default=1, type=int)
        if stride < 1 or stride > 16:
            raise ValueError("stride must be between 1 and 16")
        if i2 - i1 > 1025 or j2 - j1 > 1025:
            raise ValueError("orbital LOD source bounds may not exceed 1025 pixels per axis")
        world = _get_pipeline()
        out = _get_orbital_terrain(world, i1, j1, i2, j2, stride)
        return _binary_response(out["elev"], out["climate"])
    except Exception as e:
        return jsonify({"error": str(e)}), 400


@click.command()
@click.argument("model_path", default="xandergos/terrain-diffusion-30m")
@click.option("--caching-strategy", type=click.Choice(["indirect", "direct"]), default="direct", help="Caching strategy: 'indirect' uses HDF5, 'direct' uses in-memory LRU cache")
@click.option("--hdf5-file", default=None, help="HDF5 file path (required for indirect caching, optional for direct)")
@click.option("--cache-size", default=None, help="Cache size (default: 100M; MPS preset: 2G)")
@click.option("--seed", type=int, default=None, help="Random seed (default: from file or random)")
@click.option("--device", type=click.Choice(["cuda", "mps", "cpu"]), default=None, help="Device (default: auto)")
@click.option("--batch-size", type=str, default=None, help="Latent batch size (default: 1,4; MPS preset: 8)")
@click.option("--log-mode", type=click.Choice(["info", "verbose"]), default="verbose", help="Logging mode")
@click.option("--compile/--no-compile", "torch_compile", default=True, help="Use torch.compile for faster inference")
@click.option("--dtype", type=click.Choice(["fp32", "bf16", "fp16"]), default=None, help="Model dtype (default: fp32; MPS preset: fp16)")
@click.option("--performance-preset", type=click.Choice(["mps"]), default=None)
@click.option("--host", default="0.0.0.0", help="Server host")
@click.option("--port", type=int, default=int(os.getenv("PORT", "8000")), help="Server port")
@click.option("--kwarg", "extra_kwargs", multiple=True, help="Additional key=value kwargs (e.g. --kwarg native_resolution=30)")
def main(model_path, hdf5_file, caching_strategy, cache_size, seed, device, batch_size, log_mode, torch_compile, dtype, performance_preset, host, port, extra_kwargs):
    """Terrain API server"""
    global _PIPELINE_CONFIG
    if caching_strategy == 'indirect' and hdf5_file is None:
        hdf5_file = 'TEMP'
    if hdf5_file is not None:
        hdf5_file = resolve_hdf5_path(hdf5_file)
    device, batch_size, cache_size, dtype, kwargs = apply_performance_preset(
        performance_preset,
        device=device,
        batch_size=batch_size,
        cache_size=cache_size,
        dtype=dtype,
        extra_kwargs=extra_kwargs,
        default_batch_size="1,4",
        default_cache_size="100M",
    )
    # Parse batch size(s)
    if ',' in batch_size:
        batch_sizes = [int(x.strip()) for x in batch_size.split(',')]
    else:
        batch_sizes = int(batch_size)
    # Normalize dtype
    if dtype == 'fp32':
        dtype = None
    _PIPELINE_CONFIG = {
        'model_path': model_path,
        'hdf5_file': hdf5_file,
        'caching_strategy': caching_strategy,
        'cache_limit': parse_cache_size(cache_size),
        'seed': seed,
        'device': device,
        'latents_batch_size': batch_sizes,
        'log_mode': log_mode,
        'torch_compile': torch_compile,
        'dtype': dtype,
        'kwargs': kwargs,
    }
    _get_pipeline()
    app.run(host=host, port=port, debug=False, threaded=False)


if __name__ == "__main__":
    main()
