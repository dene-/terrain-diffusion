"""Generate a resumable 614.4 km square mountain region in 512 px chunks.

The export keeps only final, lossless 16-bit elevation PNGs. Terrain Diffusion's
intermediate tensors stay in a bounded in-memory cache and are discarded when the
process exits. Re-running the command validates and skips completed chunks.
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

import click
import numpy as np
import torch
from PIL import Image
from tqdm import tqdm

from terrain_diffusion.common.cli_helpers import apply_performance_preset, parse_cache_size
from terrain_diffusion.common.device import select_device
from terrain_diffusion.inference.world_pipeline import WorldPipeline


MODEL = "xandergos/terrain-diffusion-30m"
SEED = 20260721
CHUNK_SIZE = 512
CHUNKS_PER_SIDE = 40
QUERY_CHUNKS_PER_SIDE = 4
NATIVE_RESOLUTION_M = 30
COARSE_CELL_PIXELS = 256
COARSE_CELLS_PER_SIDE = CHUNK_SIZE * CHUNKS_PER_SIDE // COARSE_CELL_PIXELS
CONDITIONING_PADDING = 16
ELEVATION_OFFSET = 32768
ELEVATION_MIN_M = -32768
ELEVATION_MAX_M = 32767
COND_SNR = [0.05, 0.1, 1.0, 0.1, 1.0]


def build_mountain_conditioning() -> tuple[np.ndarray, int, int]:
    """Create a long, irregular alpine belt inside a raised continental interior."""
    origin = -CONDITIONING_PADDING
    size = COARSE_CELLS_PER_SIDE + 2 * CONDITIONING_PADDING
    y, x = np.mgrid[origin : origin + size, origin : origin + size].astype(np.float32)

    # A broad continental floor keeps the entire generated square inland.
    conditioning = np.full((size, size), 650.0, dtype=np.float32)

    # A gently curved 500+ km mountain belt, plus a parallel range and knots that
    # break up the silhouette. Coordinates are 7.68 km coarse cells.
    centerline = 40.0 + 7.0 * np.sin((x - 8.0) / 12.0) + 2.5 * np.sin((x + 3.0) / 4.7)
    cross = y - centerline
    longitudinal_envelope = np.exp(-0.5 * ((x - 40.0) / 34.0) ** 6)
    main_ridge = 7800.0 * np.exp(-0.5 * (cross / 4.2) ** 2) * longitudinal_envelope

    secondary_centerline = centerline - 11.0 + 1.8 * np.sin(x / 5.3)
    secondary_cross = y - secondary_centerline
    secondary_ridge = (
        3500.0
        * np.exp(-0.5 * (secondary_cross / 3.1) ** 2)
        * np.exp(-0.5 * ((x - 43.0) / 30.0) ** 6)
    )

    broad_uplift = 1100.0 * np.exp(
        -0.5 * (((x - 40.0) / 34.0) ** 2 + ((y - 38.0) / 22.0) ** 2)
    )

    peak_knots = np.zeros_like(conditioning)
    for px, amplitude, width in (
        (14.0, 2200.0, 2.8),
        (28.0, 2600.0, 3.0),
        (43.0, 3000.0, 3.2),
        (57.0, 2500.0, 2.9),
        (70.0, 2100.0, 2.7),
    ):
        py = 40.0 + 7.0 * np.sin((px - 8.0) / 12.0) + 2.5 * np.sin((px + 3.0) / 4.7)
        peak_knots += amplitude * np.exp(
            -0.5 * (((x - px) / width) ** 2 + ((y - py) / width) ** 2)
        )

    conditioning += broad_uplift + main_ridge + secondary_ridge + peak_knots
    return np.clip(conditioning, 650.0, 11500.0).astype(np.float32), origin, origin


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
    rounded = np.rint(elevation).clip(ELEVATION_MIN_M, ELEVATION_MAX_M).astype(np.int32)
    encoded = (rounded + ELEVATION_OFFSET).astype(np.uint16)
    temporary = path.with_name(f"{path.name}.tmp")
    Image.fromarray(encoded).save(temporary, format="PNG", compress_level=6)
    os.replace(temporary, path)


def write_coarse_outputs(output_dir: Path, world: WorldPipeline, conditioning: np.ndarray) -> dict:
    coarse_raw = world.coarse[:, 0:COARSE_CELLS_PER_SIDE, 0:COARSE_CELLS_PER_SIDE]
    coarse = (coarse_raw[:-1] / coarse_raw[-1:]).cpu().numpy().astype(np.float32)
    coarse_elevation = np.sign(coarse[0]) * np.square(coarse[0])

    np.savez_compressed(
        output_dir / "coarse_fields.npz",
        fields=coarse,
        elevation_m=coarse_elevation,
        conditioning_elevation_m=conditioning,
    )

    low = float(np.percentile(coarse_elevation, 1))
    high = float(np.percentile(coarse_elevation, 99))
    normalized = np.clip((coarse_elevation - low) / max(high - low, 1.0), 0.0, 1.0)
    # Use 8-bit for the human-facing overview. Some previewers render 16-bit
    # grayscale PNG byte order incorrectly even though the data is valid.
    preview = (normalized * 255.0).astype(np.uint8)
    Image.fromarray(preview).resize((640, 640), Image.Resampling.NEAREST).save(
        output_dir / "coarse_elevation_preview.png",
        format="PNG",
        compress_level=6,
    )

    return {
        "min_m": float(coarse_elevation.min()),
        "max_m": float(coarse_elevation.max()),
        "mean_m": float(coarse_elevation.mean()),
        "land_fraction": float(np.mean(coarse_elevation > 0.0)),
        "above_3000m_fraction": float(np.mean(coarse_elevation >= 3000.0)),
        "above_5000m_fraction": float(np.mean(coarse_elevation >= 5000.0)),
    }


def base_manifest(conditioning: np.ndarray, origin_i: int, origin_j: int) -> dict:
    pixels_per_side = CHUNK_SIZE * CHUNKS_PER_SIDE
    return {
        "name": "Tall Mountain Continental Region",
        "model": MODEL,
        "seed": SEED,
        "native_resolution_m_per_pixel": NATIVE_RESOLUTION_M,
        "chunks": {
            "rows": CHUNKS_PER_SIDE,
            "columns": CHUNKS_PER_SIDE,
            "total": CHUNKS_PER_SIDE**2,
            "chunk_width_pixels": CHUNK_SIZE,
            "chunk_height_pixels": CHUNK_SIZE,
            "chunk_width_m": CHUNK_SIZE * NATIVE_RESOLUTION_M,
            "chunk_height_m": CHUNK_SIZE * NATIVE_RESOLUTION_M,
            "filename_pattern": "elevation/elevation_y{row:02d}_x{column:02d}.png",
        },
        "coverage": {
            "width_pixels": pixels_per_side,
            "height_pixels": pixels_per_side,
            "width_km": pixels_per_side * NATIVE_RESOLUTION_M / 1000,
            "height_km": pixels_per_side * NATIVE_RESOLUTION_M / 1000,
            "area_km2": (pixels_per_side * NATIVE_RESOLUTION_M / 1000) ** 2,
        },
        "elevation_encoding": {
            "format": "16-bit grayscale PNG",
            "decoded_elevation_m": "uint16_pixel_value - 32768",
            "quantization_m": 1,
            "minimum_m": ELEVATION_MIN_M,
            "maximum_m": ELEVATION_MAX_M,
        },
        "conditioning": {
            "type": "custom_extreme_alpine_belt_on_continental_interior",
            "origin_coarse_cells": [origin_i, origin_j],
            "shape_coarse_cells": list(conditioning.shape),
            "coarse_cell_size_m": COARSE_CELL_PIXELS * NATIVE_RESOLUTION_M,
            "continental_floor_m": 650,
            "minimum_m": float(conditioning.min()),
            "maximum_m": float(conditioning.max()),
            "snr": COND_SNR,
        },
        "runtime_storage": {
            "persistent_intermediate_cache": False,
            "normals_and_meshes": "derive at runtime",
            "climate": "coarse_fields.npz",
            "generation_query_chunks_per_side": QUERY_CHUNKS_PER_SIDE,
        },
    }


@click.command()
@click.argument(
    "output_dir",
    type=click.Path(path_type=Path),
    default=Path("generated/tall_mountain_continent_600km_square_30m"),
)
@click.option("--device", type=click.Choice(["cuda", "mps", "cpu"]), default=None)
@click.option("--performance-preset", type=click.Choice(["mps"]), default=None)
@click.option("--batch-size", default=None, help="Latent batch size (MPS preset: 8).")
@click.option("--cache-size", default=None, help="Bounded in-memory cache (MPS preset: 2G).")
@click.option("--dtype", type=click.Choice(["fp32", "bf16", "fp16"]), default=None)
@click.option("--max-chunks", type=click.IntRange(min=1), default=None, help="Stop after generating this many new chunks.")
@click.option("--coarse-preview-only", is_flag=True, help="Write coarse outputs without decoding native chunks.")
def main(
    output_dir: Path,
    device: str | None,
    performance_preset: str | None,
    batch_size: str | None,
    cache_size: str | None,
    dtype: str | None,
    max_chunks: int | None,
    coarse_preview_only: bool,
) -> None:
    """Export the fixed 40x40 extreme-mountain region to OUTPUT_DIR."""
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "elevation").mkdir(exist_ok=True)

    device, batch_size, cache_size, dtype, kwargs = apply_performance_preset(
        performance_preset,
        device=device,
        batch_size=batch_size,
        cache_size=cache_size,
        dtype=dtype,
        extra_kwargs=(),
        default_batch_size="1,2,4,8,16",
        default_cache_size="1G",
    )
    device = select_device(device)
    batch_sizes = [int(value) for value in batch_size.split(",")] if "," in batch_size else int(batch_size)
    model_dtype = None if dtype == "fp32" else dtype

    conditioning, origin_i, origin_j = build_mountain_conditioning()
    manifest = base_manifest(conditioning, origin_i, origin_j)
    manifest_path = output_dir / "manifest.json"
    if manifest_path.exists():
        existing_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for preserved_key in ("elevation_statistics", "validation", "files"):
            if preserved_key in existing_manifest:
                manifest[preserved_key] = existing_manifest[preserved_key]
    atomic_write_json(manifest_path, manifest)

    world = WorldPipeline.from_pretrained(
        MODEL,
        seed=SEED,
        latents_batch_size=batch_sizes,
        log_mode="info",
        torch_compile=False,
        dtype=model_dtype,
        caching_strategy="direct",
        cache_limit=parse_cache_size(cache_size),
        onestep_latent=True,
        **kwargs,
    )
    world.set_custom_conditioning_import(0, conditioning, origin_i, origin_j, default_value=650.0)
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
                    block_rows = range(block_row, min(block_row + QUERY_CHUNKS_PER_SIDE, CHUNKS_PER_SIDE))
                    block_cols = range(block_col, min(block_col + QUERY_CHUNKS_PER_SIDE, CHUNKS_PER_SIDE))
                    missing = [
                        (row, col)
                        for row in block_rows
                        for col in block_cols
                        if not valid_chunk(chunk_path(output_dir, row, col))
                    ]
                    if not missing:
                        continue

                    block_row_end = min(block_row + QUERY_CHUNKS_PER_SIDE, CHUNKS_PER_SIDE)
                    block_col_end = min(block_col + QUERY_CHUNKS_PER_SIDE, CHUNKS_PER_SIDE)
                    i1 = block_row * CHUNK_SIZE
                    j1 = block_col * CHUNK_SIZE
                    elevation = world.get(
                        i1,
                        j1,
                        block_row_end * CHUNK_SIZE,
                        block_col_end * CHUNK_SIZE,
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
            atomic_write_json(
                output_dir / "progress.json",
                {
                    "completed_chunks": CHUNKS_PER_SIDE**2,
                    "total_chunks": CHUNKS_PER_SIDE**2,
                    "complete": True,
                    "elapsed_seconds_this_run": elapsed,
                },
            )
            manifest["generation"] = {
                "complete": True,
                "completed_chunks": CHUNKS_PER_SIDE**2,
                "elapsed_seconds_final_run": elapsed,
            }
            atomic_write_json(manifest_path, manifest)
        finally:
            progress.close()
    finally:
        world.close()


if __name__ == "__main__":
    main()
