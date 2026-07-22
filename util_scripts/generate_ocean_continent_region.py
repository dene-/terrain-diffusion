"""Generate a high-fidelity ocean-framed continent in resumable 512 px chunks."""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

import click
import numpy as np
from PIL import Image
from scipy import ndimage
from tqdm import tqdm

from terrain_diffusion.common.cli_helpers import parse_cache_size
from terrain_diffusion.common.device import select_device
from terrain_diffusion.inference.world_pipeline import WorldPipeline


MODEL = "xandergos/terrain-diffusion-30m"
SEED = 20260722
CHUNK_SIZE = 512
CHUNKS_PER_SIDE = 40
QUERY_CHUNKS_PER_SIDE = 4
NATIVE_RESOLUTION_M = 30
COARSE_CELL_PIXELS = 256
COARSE_CELLS_PER_SIDE = CHUNK_SIZE * CHUNKS_PER_SIDE // COARSE_CELL_PIXELS
CONDITIONING_PADDING = 16
ELEVATION_UNITS_PER_M = 2.0
ELEVATION_OFFSET = 32768
ENCODED_MIN = -32768
ENCODED_MAX = 32767
COND_SNR = [0.03, 0.08, 0.15, 0.08, 0.15]
DEFAULT_LAYOUT_SEED = 102


def _coherent_noise(shape: tuple[int, int], seed: int, sigma: float) -> np.ndarray:
    rng = np.random.default_rng(seed)
    noise = ndimage.gaussian_filter(rng.standard_normal(shape), sigma=sigma, mode="reflect")
    noise -= noise.mean()
    noise /= max(noise.std(), 1e-6)
    return noise.astype(np.float32)


def _rotated_ellipse_score(
    x: np.ndarray,
    y: np.ndarray,
    cx: float,
    cy: float,
    rx: float,
    ry: float,
    angle_degrees: float,
) -> np.ndarray:
    angle = np.deg2rad(angle_degrees)
    dx, dy = x - cx, y - cy
    along = np.cos(angle) * dx + np.sin(angle) * dy
    across = -np.sin(angle) * dx + np.cos(angle) * dy
    return 1.0 - np.sqrt((along / rx) ** 2 + (across / ry) ** 2)


def _ridge_segment(
    x: np.ndarray,
    y: np.ndarray,
    cx: float,
    cy: float,
    length: float,
    width: float,
    angle_degrees: float,
    amplitude: float,
) -> np.ndarray:
    angle = np.deg2rad(angle_degrees)
    dx, dy = x - cx, y - cy
    along = np.cos(angle) * dx + np.sin(angle) * dy
    across = -np.sin(angle) * dx + np.cos(angle) * dy
    return amplitude * np.exp(-0.5 * (along / length) ** 6 - 0.5 * (across / width) ** 2)


def build_conditioning(
    layout_seed: int = DEFAULT_LAYOUT_SEED,
    climate_seed: int = SEED,
    range_style: str = "segmented",
) -> tuple[np.ndarray, int, int]:
    """Return elevation and four climate controls on a padded coarse grid."""
    origin = -CONDITIONING_PADDING
    size = COARSE_CELLS_PER_SIDE + 2 * CONDITIONING_PADDING
    y, x = np.mgrid[origin : origin + size, origin : origin + size].astype(np.float32)
    # Generate the noise on the visible 80x80 map and reflect-pad it. Keeping the
    # visible realization independent of inference padding makes layout changes
    # reproducible while still supplying context outside every map edge.
    def map_noise(seed: int, sigma: float) -> np.ndarray:
        core = _coherent_noise(
            (COARSE_CELLS_PER_SIDE, COARSE_CELLS_PER_SIDE), seed, sigma
        )
        return np.pad(core, CONDITIONING_PADDING, mode="reflect")

    climate_noise = map_noise(climate_seed + 13, 6.0)

    # A coordinate-warped plate plus three octaves of signed noise produces the
    # continent. Unlike unions of ellipses, its threshold has irregular capes,
    # coves, peninsulas and narrow straits at several spatial scales.
    warp_x = x + 4.8 * map_noise(layout_seed + 1, 6.5) + 1.3 * map_noise(layout_seed + 2, 2.5)
    warp_y = y + 4.8 * map_noise(layout_seed + 3, 6.5) + 1.3 * map_noise(layout_seed + 4, 2.5)
    angle = np.deg2rad(-17.0)
    dx, dy = warp_x - 39.0, warp_y - 40.0
    plate_x = (np.cos(angle) * dx + np.sin(angle) * dy) / 25.5
    plate_y = (-np.sin(angle) * dx + np.cos(angle) * dy) / 21.5
    plate_radius = np.sqrt(plate_x**2 + plate_y**2)
    theta = np.arctan2(plate_y, plate_x)
    continent_score = (
        1.0
        - plate_radius
        + 0.18 * map_noise(layout_seed + 5, 5.5)
        + 0.115 * map_noise(layout_seed + 6, 2.1)
        + 0.055 * map_noise(layout_seed + 7, 0.72)
        + 0.09 * np.sin(3.0 * theta + 0.2)
        + 0.045 * np.sin(7.0 * theta - 1.1)
    )

    # Rifts cut asymmetric gulfs into the noisy field. They modify an already
    # irregular boundary rather than stamping visible circular bays into it.
    for cx, cy, rx, ry, angle_degrees, amplitude in (
        (58.0, 29.0, 9.0, 4.0, -35.0, 0.40),
        (24.0, 57.0, 7.0, 3.0, 18.0, 0.33),
        (48.0, 64.0, 3.5, 8.0, -14.0, 0.28),
        (18.0, 31.0, 5.0, 3.0, -5.0, 0.22),
    ):
        angle = np.deg2rad(angle_degrees)
        local_x, local_y = x - cx, y - cy
        along = np.cos(angle) * local_x + np.sin(angle) * local_y
        across = -np.sin(angle) * local_x + np.cos(angle) * local_y
        continent_score -= amplitude * np.exp(
            -0.5 * ((along / rx) ** 2 + (across / ry) ** 2)
        )

    # Four noisy tectonic corridors create archipelagos. The noise threshold,
    # rather than one primitive per island, determines every island silhouette.
    island_score = np.full_like(continent_score, -10.0)
    island_noise = (
        0.42 * map_noise(layout_seed + 8, 2.0)
        + 0.22 * map_noise(layout_seed + 9, 0.7)
    )
    for cx, cy, rx, ry, angle_degrees in (
        (12.0, 19.0, 10.0, 3.0, -28.0),
        (69.0, 20.0, 11.0, 3.0, 50.0),
        (68.0, 65.0, 13.0, 4.0, -35.0),
        (13.0, 68.0, 12.0, 3.0, 20.0),
    ):
        angle = np.deg2rad(angle_degrees)
        local_x, local_y = x - cx, y - cy
        along = np.cos(angle) * local_x + np.sin(angle) * local_y
        across = -np.sin(angle) * local_x + np.cos(angle) * local_y
        region_score = (
            0.35
            - 0.5 * ((along / rx) ** 2 + (across / ry) ** 2)
            + island_noise
        )
        island_score = np.maximum(island_score, region_score)

    raw_land_score = np.maximum(continent_score, island_score)
    distance_to_map_edge = np.minimum.reduce(
        (x, y, COARSE_CELLS_PER_SIDE - 1.0 - x, COARSE_CELLS_PER_SIDE - 1.0 - y)
    )
    raw_land_score -= 1.5 * np.exp(-0.5 * (distance_to_map_edge / 1.5) ** 2)
    land = raw_land_score >= 0.0
    organic_kernel = np.array([[0, 1, 0], [1, 1, 1], [0, 1, 0]], dtype=bool)
    land = ndimage.binary_opening(land, structure=organic_kernel)
    land = ndimage.binary_closing(land, structure=organic_kernel)
    land_score = np.where(
        land,
        np.maximum(raw_land_score, 0.025),
        np.minimum(raw_land_score, -0.025),
    )
    land_gate = np.clip(land_score / 0.18, 0.0, 1.0)
    ocean_distance = np.maximum(-land_score, 0.0)
    land_distance = np.maximum(land_score, 0.0)

    # Broad shelves descend to abyssal plains; the outer padded frame and all map
    # edges remain deep ocean. Land rises gradually inland from a low coastline.
    ocean_elevation = -120.0 - 6400.0 * (1.0 - np.exp(-3.6 * ocean_distance))
    land_elevation = 80.0 + 1050.0 * (1.0 - np.exp(-3.0 * land_distance))
    elevation = np.where(land_score >= 0.0, land_elevation, ocean_elevation)

    # A curving convergent boundary supplies the range-scale uplift.
    ridge_center_y = (
        53.0
        - 0.40 * (x - 18.0)
        - 3.4 * np.sin((x - 14.0) / 8.0)
        - 1.0 * np.sin((x - 9.0) / 2.7)
    )
    ridge_gate = 1.0 / (1.0 + np.exp(-(x - 17.0) / 1.5))
    ridge_gate *= 1.0 / (1.0 + np.exp((x - 64.0) / 1.5))
    ridge_width = 1.0 + 0.45 * np.clip(map_noise(layout_seed + 12, 5.0), -1.0, 1.0)
    ridge_variation = np.clip(
        0.58
        + 0.31 * map_noise(layout_seed + 10, 4.0)
        + 0.16 * map_noise(layout_seed + 11, 1.3),
        0.08,
        1.15,
    )
    if range_style == "himalayan":
        relative_to_crest = y - ridge_center_y
        crest_width = 1.15 + 0.35 * np.clip(
            map_noise(layout_seed + 18, 3.5), -1.0, 1.0
        )
        main_range = np.exp(-0.5 * (relative_to_crest / crest_width) ** 2)
        main_range *= ridge_gate * (2100.0 + 2900.0 * ridge_variation)

        # An uplifted plateau lies behind the main thrust, while three offset
        # folds and lower foothills give the system depth instead of one stripe.
        plateau_variation = np.clip(
            0.82 + 0.22 * map_noise(layout_seed + 19, 4.5), 0.45, 1.25
        )
        plateau = np.exp(-0.5 * ((relative_to_crest + 4.4) / 3.0) ** 4)
        plateau *= ridge_gate * (1750.0 + 1150.0 * plateau_variation)
        parallel_folds = ridge_gate * (
            1850.0
            * np.exp(-0.5 * ((relative_to_crest + 2.35) / 0.85) ** 2)
            * np.clip(0.7 + 0.3 * map_noise(layout_seed + 20, 2.2), 0.2, 1.2)
            + 1250.0
            * np.exp(-0.5 * ((relative_to_crest + 5.2) / 1.1) ** 2)
            * np.clip(0.7 + 0.3 * map_noise(layout_seed + 21, 2.6), 0.2, 1.2)
            + 1050.0
            * np.exp(-0.5 * ((relative_to_crest - 2.8) / 1.35) ** 2)
            * np.clip(0.75 + 0.25 * map_noise(layout_seed + 22, 2.8), 0.25, 1.2)
        )
        peak_specs = (
            (20.0, 4700.0, 1.25),
            (26.0, 6100.0, 1.35),
            (32.0, 7200.0, 1.3),
            (38.0, 5600.0, 1.25),
            (44.0, 8400.0, 1.4),
            (50.0, 6800.0, 1.2),
            (56.0, 7600.0, 1.35),
            (62.0, 5200.0, 1.2),
        )
        branch_ranges = (
            _ridge_segment(x, y, 28.0, 43.0, 6.5, 1.0, 48.0, 1800.0)
            + _ridge_segment(x, y, 42.0, 38.0, 5.5, 1.0, 58.0, 1500.0)
            + _ridge_segment(x, y, 54.0, 48.0, 5.0, 1.0, -48.0, 1600.0)
        )
    else:
        main_range = np.exp(-0.5 * ((y - ridge_center_y) / ridge_width) ** 2)
        main_range *= ridge_gate * (1500.0 + 3200.0 * ridge_variation)
        plateau = np.zeros_like(elevation)
        parallel_folds = np.zeros_like(elevation)
        peak_specs = (
            (23.0, 5200.0, 1.6),
            (34.0, 6500.0, 1.8),
            (46.0, 8500.0, 1.6),
            (57.0, 6000.0, 1.7),
        )
        branch_ranges = (
            _ridge_segment(x, y, 33.0, 35.0, 8.0, 1.4, 38.0, 2100.0)
            + _ridge_segment(x, y, 52.0, 50.0, 6.0, 1.3, -45.0, 1800.0)
        )

    peak_knots = np.zeros_like(elevation)
    for px, amplitude, width in peak_specs:
        py = (
            53.0
            - 0.40 * (px - 18.0)
            - 3.4 * np.sin((px - 14.0) / 8.0)
            - 1.0 * np.sin((px - 9.0) / 2.7)
        )
        peak_knots += amplitude * np.exp(
            -0.5
            * (((x - px) / width) ** 2 + ((y - py) / (width * 0.8)) ** 2)
        )

    island_relief = (
        2300.0
        * np.power(np.clip(island_score / 0.42, 0.0, 1.0), 1.4)
        * np.clip(0.8 + 0.2 * map_noise(layout_seed + 17, 1.5), 0.35, 1.25)
    )
    elevation += (
        main_range
        + plateau
        + parallel_folds
        + peak_knots
        + branch_ranges
        + island_relief
    ) * land_gate

    # Deep offshore trenches sit beyond the eastern and southern island arcs.
    trench = (
        1500.0 * np.exp(-0.5 * (((x - 75.0) / 4.0) ** 2 + ((y - 42.0) / 20.0) ** 2))
        + 1200.0 * np.exp(-0.5 * (((x - 42.0) / 22.0) ** 2 + ((y - 75.0) / 4.0) ** 2))
    )
    elevation -= trench * (land_score < 0.0)
    elevation = np.clip(elevation, -7800.0, 12000.0).astype(np.float32)

    # Climate controls deliberately span tropical, arid, temperate, boreal and
    # alpine regimes. Elevation lapse is applied later by WorldPipeline.
    latitude = np.clip((y - 0.0) / COARSE_CELLS_PER_SIDE, 0.0, 1.0)
    temperature = 5.0 + 25.0 * latitude + 1.2 * climate_noise
    temperature_std = 720.0 - 480.0 * latitude + 55.0 * climate_noise

    tectonic_axis = y - ridge_center_y
    western_wet = 1100.0 * np.exp(-0.5 * ((tectonic_axis + 4.0) / 6.0) ** 2) * land_gate
    eastern_shadow = 1050.0 * np.exp(-0.5 * ((tectonic_axis - 8.0) / 8.0) ** 2) * land_gate
    tropical_wet = 1350.0 * np.exp(-0.5 * ((y - 65.0) / 13.0) ** 2)
    northwest_dry = 850.0 * np.exp(-0.5 * (((x - 29.0) / 12.0) ** 2 + ((y - 25.0) / 10.0) ** 2))
    precipitation = 1150.0 + western_wet + tropical_wet - eastern_shadow - northwest_dry
    precipitation += 170.0 * climate_noise
    precipitation = np.clip(precipitation, 180.0, 4200.0)
    precipitation_cv = np.clip(28.0 + 0.026 * (1500.0 - precipitation) + 8.0 * climate_noise, 18.0, 120.0)

    return np.stack(
        [elevation, temperature, temperature_std, precipitation, precipitation_cv],
        axis=0,
    ).astype(np.float32), origin, origin


def chunk_path(output_dir: Path, row: int, col: int) -> Path:
    return output_dir / "elevation" / f"elevation_y{row:02d}_x{col:02d}.png"


def valid_chunk(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        with Image.open(path) as image:
            return image.size == (CHUNK_SIZE, CHUNK_SIZE) and image.mode in {"I;16", "I"}
    except (OSError, ValueError):
        return False


def atomic_write_json(path: Path, payload: dict) -> None:
    temporary = path.with_name(f"{path.name}.tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def write_elevation_chunk(path: Path, elevation: np.ndarray) -> None:
    quantized = np.rint(elevation * ELEVATION_UNITS_PER_M).clip(ENCODED_MIN, ENCODED_MAX).astype(np.int32)
    encoded = (quantized + ELEVATION_OFFSET).astype(np.uint16)
    temporary = path.with_name(f"{path.name}.tmp")
    Image.fromarray(encoded).save(temporary, format="PNG", compress_level=6)
    os.replace(temporary, path)


def _elevation_rgb(elevation: np.ndarray) -> np.ndarray:
    stops = np.array([-8000, -5000, -1500, 0, 200, 1200, 3000, 5500, 8000, 11000], dtype=np.float32)
    colors = np.array([
        [5, 18, 54], [8, 43, 92], [20, 91, 145], [64, 147, 174], [64, 126, 69],
        [112, 139, 74], [145, 126, 94], [142, 140, 139], [220, 228, 234], [255, 255, 255],
    ], dtype=np.float32)
    return np.stack([np.interp(elevation, stops, colors[:, channel]) for channel in range(3)], axis=-1).astype(np.uint8)


def _biome_rgb(elevation: np.ndarray, coarse: np.ndarray) -> np.ndarray:
    temperature = coarse[2]
    precipitation = coarse[4]
    rgb = np.empty((*elevation.shape, 3), dtype=np.uint8)
    rgb[:] = (112, 160, 82)  # temperate grassland
    rgb[(temperature < 11) & (precipitation >= 650)] = (58, 105, 78)      # boreal
    rgb[(temperature >= 11) & (precipitation >= 1500)] = (44, 126, 69)   # forest
    rgb[(temperature >= 23) & (precipitation >= 2200)] = (27, 107, 65)   # rainforest
    rgb[(temperature >= 20) & (precipitation >= 650) & (precipitation < 1500)] = (153, 159, 69)  # savanna
    rgb[precipitation < 650] = (196, 162, 91)                             # arid
    rgb[elevation >= 2500] = (127, 121, 113)                             # alpine
    rgb[elevation >= 5000] = (224, 231, 237)                             # snow/ice
    ocean = elevation < 0
    depth_t = np.clip((-elevation) / 8000.0, 0.0, 1.0)
    rgb[ocean, 0] = (35 - 29 * depth_t[ocean]).astype(np.uint8)
    rgb[ocean, 1] = (111 - 88 * depth_t[ocean]).astype(np.uint8)
    rgb[ocean, 2] = (165 - 94 * depth_t[ocean]).astype(np.uint8)
    return rgb


def write_coarse_outputs(output_dir: Path, world: WorldPipeline, conditioning: np.ndarray) -> dict:
    raw = world.coarse[:, 0:COARSE_CELLS_PER_SIDE, 0:COARSE_CELLS_PER_SIDE]
    coarse = (raw[:-1] / raw[-1:]).cpu().numpy().astype(np.float32)
    elevation = np.sign(coarse[0]) * np.square(coarse[0])
    np.savez_compressed(
        output_dir / "coarse_fields.npz",
        fields=coarse,
        elevation_m=elevation,
        conditioning=conditioning,
    )
    editor_heightmap = (
        np.rint(elevation * ELEVATION_UNITS_PER_M)
        .clip(ENCODED_MIN, ENCODED_MAX)
        .astype(np.int32)
        + ELEVATION_OFFSET
    ).astype(np.uint16)
    Image.fromarray(editor_heightmap).save(
        output_dir / "editor_heightmap.png", format="PNG", compress_level=6
    )
    Image.fromarray(_elevation_rgb(elevation)).resize((800, 800), Image.Resampling.BICUBIC).save(
        output_dir / "coarse_elevation_preview.png", optimize=True
    )
    Image.fromarray(_biome_rgb(elevation, coarse)).resize((800, 800), Image.Resampling.BICUBIC).save(
        output_dir / "coarse_biome_preview.png", optimize=True
    )

    land = elevation >= 0.0
    labels, component_count = ndimage.label(land)
    edge_water = bool(
        np.all(~land[0]) and np.all(~land[-1]) and np.all(~land[:, 0]) and np.all(~land[:, -1])
    )
    return {
        "minimum_m": float(elevation.min()),
        "maximum_m": float(elevation.max()),
        "mean_m": float(elevation.mean()),
        "land_fraction": float(land.mean()),
        "land_component_count": int(component_count),
        "water_on_all_edges": edge_water,
        "above_5000m_fraction": float(np.mean(elevation >= 5000.0)),
        "below_minus_5000m_fraction": float(np.mean(elevation <= -5000.0)),
    }


def base_manifest(
    conditioning: np.ndarray,
    origin_i: int,
    origin_j: int,
    dtype: str,
    seed: int,
    layout_seed: int,
    range_style: str,
) -> dict:
    pixels = CHUNK_SIZE * CHUNKS_PER_SIDE
    return {
        "name": "Ocean-Framed Continent and Islands",
        "model": MODEL,
        "seed": seed,
        "layout_seed": layout_seed,
        "range_style": range_style,
        "native_resolution_m_per_pixel": NATIVE_RESOLUTION_M,
        "chunks": {
            "rows": CHUNKS_PER_SIDE,
            "columns": CHUNKS_PER_SIDE,
            "total": CHUNKS_PER_SIDE**2,
            "width_pixels": CHUNK_SIZE,
            "height_pixels": CHUNK_SIZE,
            "width_m": CHUNK_SIZE * NATIVE_RESOLUTION_M,
            "height_m": CHUNK_SIZE * NATIVE_RESOLUTION_M,
            "filename_pattern": "elevation/elevation_y{row:02d}_x{column:02d}.png",
        },
        "coverage": {
            "width_pixels": pixels,
            "height_pixels": pixels,
            "width_km": pixels * NATIVE_RESOLUTION_M / 1000,
            "height_km": pixels * NATIVE_RESOLUTION_M / 1000,
        },
        "elevation_encoding": {
            "format": "16-bit grayscale PNG",
            "decoded_elevation_m": "(uint16_pixel_value - 32768) / 2",
            "quantization_m": 0.5,
            "minimum_representable_m": ENCODED_MIN / ELEVATION_UNITS_PER_M,
            "maximum_representable_m": ENCODED_MAX / ELEVATION_UNITS_PER_M,
        },
        "conditioning": {
            "type": "ocean-framed irregular continent, four archipelagos, alpine spine, controlled climate",
            "origin_coarse_cells": [origin_i, origin_j],
            "shape": list(conditioning.shape),
            "coarse_cell_size_m": COARSE_CELL_PIXELS * NATIVE_RESOLUTION_M,
            "snr": COND_SNR,
        },
        "fidelity": {
            "pipeline_dtype": dtype,
            "onestep_latent": False,
            "mps_fast_math": False,
            "decoder_tile_size": 768,
            "decoder_tile_stride": 640,
            "persistent_intermediate_cache": False,
        },
    }


@click.command()
@click.argument(
    "output_dir",
    type=click.Path(path_type=Path),
    default=Path("generated/ocean_continent_islands_600km_square_30m"),
)
@click.option("--device", type=click.Choice(["cuda", "mps", "cpu"]), default=None)
@click.option("--batch-size", default="8")
@click.option("--cache-size", default="2G")
@click.option("--dtype", type=click.Choice(["fp32", "bf16", "fp16"]), default="fp32")
@click.option("--max-chunks", type=click.IntRange(min=1), default=None)
@click.option("--coarse-preview-only", is_flag=True)
@click.option("--seed", type=int, default=SEED, show_default=True)
@click.option("--layout-seed", type=int, default=DEFAULT_LAYOUT_SEED, show_default=True)
@click.option(
    "--range-style",
    type=click.Choice(["segmented", "himalayan"]),
    default="segmented",
    show_default=True,
)
def main(
    output_dir: Path,
    device: str | None,
    batch_size: str,
    cache_size: str,
    dtype: str,
    max_chunks: int | None,
    coarse_preview_only: bool,
    seed: int,
    layout_seed: int,
    range_style: str,
) -> None:
    """Export the fixed high-fidelity ocean continent to OUTPUT_DIR."""
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "elevation").mkdir(exist_ok=True)
    device = select_device(device)
    batch_sizes = [int(value) for value in batch_size.split(",")] if "," in batch_size else int(batch_size)
    model_dtype = None if dtype == "fp32" else dtype

    conditioning, origin_i, origin_j = build_conditioning(
        layout_seed=layout_seed,
        climate_seed=seed,
        range_style=range_style,
    )
    manifest_path = output_dir / "manifest.json"
    manifest = base_manifest(
        conditioning,
        origin_i,
        origin_j,
        dtype,
        seed,
        layout_seed,
        range_style,
    )
    atomic_write_json(manifest_path, manifest)

    world = WorldPipeline.from_pretrained(
        MODEL,
        seed=seed,
        latents_batch_size=batch_sizes,
        log_mode="info",
        torch_compile=False,
        dtype=model_dtype,
        caching_strategy="direct",
        cache_limit=parse_cache_size(cache_size),
        onestep_latent=False,
        T=2,
        decoder_tile_size=768,
        decoder_tile_stride=640,
    )
    defaults = (-6500.0, 15.0, 400.0, 1200.0, 50.0)
    for channel in range(5):
        world.set_custom_conditioning_import(
            channel,
            conditioning[channel],
            origin_i,
            origin_j,
            default_value=defaults[channel],
        )
    world.set_cond_snr(COND_SNR)
    world.to(device).bind()

    started_at = time.monotonic()
    try:
        coarse_stats = write_coarse_outputs(output_dir, world, conditioning)
        manifest["coarse_elevation"] = coarse_stats
        atomic_write_json(manifest_path, manifest)
        click.echo(f"Coarse elevation: {coarse_stats}")
        if coarse_preview_only:
            return

        completed_before = sum(
            valid_chunk(chunk_path(output_dir, row, col))
            for row in range(CHUNKS_PER_SIDE)
            for col in range(CHUNKS_PER_SIDE)
        )
        generated_now = 0
        progress = tqdm(total=CHUNKS_PER_SIDE**2, initial=completed_before, desc="Elevation chunks")
        try:
            for block_row in range(0, CHUNKS_PER_SIDE, QUERY_CHUNKS_PER_SIDE):
                for block_col in range(0, CHUNKS_PER_SIDE, QUERY_CHUNKS_PER_SIDE):
                    row_end = min(block_row + QUERY_CHUNKS_PER_SIDE, CHUNKS_PER_SIDE)
                    col_end = min(block_col + QUERY_CHUNKS_PER_SIDE, CHUNKS_PER_SIDE)
                    missing = [
                        (row, col)
                        for row in range(block_row, row_end)
                        for col in range(block_col, col_end)
                        if not valid_chunk(chunk_path(output_dir, row, col))
                    ]
                    if not missing:
                        continue

                    elevation = world.get(
                        block_row * CHUNK_SIZE,
                        block_col * CHUNK_SIZE,
                        row_end * CHUNK_SIZE,
                        col_end * CHUNK_SIZE,
                        with_climate=False,
                    )["elev"].cpu().numpy()
                    for row, col in missing:
                        local_i = (row - block_row) * CHUNK_SIZE
                        local_j = (col - block_col) * CHUNK_SIZE
                        write_elevation_chunk(
                            chunk_path(output_dir, row, col),
                            elevation[
                                local_i : local_i + CHUNK_SIZE,
                                local_j : local_j + CHUNK_SIZE,
                            ],
                        )
                        generated_now += 1
                        progress.update(1)
                        atomic_write_json(
                            output_dir / "progress.json",
                            {
                                "completed_chunks": completed_before + generated_now,
                                "total_chunks": CHUNKS_PER_SIDE**2,
                                "last_completed": {"row": row, "column": col},
                                "elapsed_seconds_this_run": time.monotonic() - started_at,
                            },
                        )
                        if max_chunks is not None and generated_now >= max_chunks:
                            click.echo(f"Stopped after {generated_now} new chunk(s); rerun to resume.")
                            return

            elapsed = time.monotonic() - started_at
            manifest["generation"] = {
                "complete": True,
                "completed_chunks": CHUNKS_PER_SIDE**2,
                "elapsed_seconds_final_run": elapsed,
            }
            atomic_write_json(manifest_path, manifest)
            atomic_write_json(
                output_dir / "progress.json",
                {
                    "completed_chunks": CHUNKS_PER_SIDE**2,
                    "total_chunks": CHUNKS_PER_SIDE**2,
                    "complete": True,
                    "elapsed_seconds_this_run": elapsed,
                },
            )
        finally:
            progress.close()
    finally:
        world.close()


if __name__ == "__main__":
    main()
