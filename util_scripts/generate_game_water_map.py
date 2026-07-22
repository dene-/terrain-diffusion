"""Derive game-ready ocean depth and mask chunks from exported elevation PNGs."""

from __future__ import annotations

import json
import os
from pathlib import Path

import click
import numpy as np
from PIL import Image
from tqdm import tqdm


CHUNK_SIZE = 512
CHUNKS_PER_SIDE = 40
ELEVATION_OFFSET = 32768
UNITS_PER_METRE = 2.0
PREVIEW_CHUNK_SIZE = 32


def elevation_path(region_dir: Path, row: int, column: int) -> Path:
    return region_dir / "elevation" / f"elevation_y{row:02d}_x{column:02d}.png"


def water_depth_path(region_dir: Path, row: int, column: int) -> Path:
    return region_dir / "water_depth" / f"water_depth_y{row:02d}_x{column:02d}.png"


def water_mask_path(region_dir: Path, row: int, column: int) -> Path:
    return region_dir / "water_mask" / f"water_mask_y{row:02d}_x{column:02d}.png"


def valid_image(path: Path, mode: str) -> bool:
    if not path.is_file():
        return False
    try:
        with Image.open(path) as image:
            if image.size != (CHUNK_SIZE, CHUNK_SIZE):
                return False
            return image.mode in ({"I;16", "I"} if mode == "depth" else {"L"})
    except (OSError, ValueError):
        return False


def atomic_save(image: Image.Image, path: Path) -> None:
    temporary = path.with_name(f"{path.name}.tmp")
    image.save(temporary, format="PNG", compress_level=6)
    os.replace(temporary, path)


def atomic_json(path: Path, payload: dict) -> None:
    temporary = path.with_name(f"{path.name}.tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def write_preview(region_dir: Path) -> None:
    preview_size = CHUNKS_PER_SIDE * PREVIEW_CHUNK_SIZE
    depth_preview = np.zeros((preview_size, preview_size), dtype=np.float32)
    water_fraction = np.zeros_like(depth_preview)

    for row in range(CHUNKS_PER_SIDE):
        for column in range(CHUNKS_PER_SIDE):
            with Image.open(water_depth_path(region_dir, row, column)) as image:
                depth_m = np.asarray(image, dtype=np.uint16).astype(np.float32) / UNITS_PER_METRE
            water = (depth_m > 0.0).astype(np.float32)
            reduced_depth = np.asarray(
                Image.fromarray(depth_m, mode="F").resize(
                    (PREVIEW_CHUNK_SIZE, PREVIEW_CHUNK_SIZE), Image.Resampling.BOX
                ),
                dtype=np.float32,
            )
            reduced_water = np.asarray(
                Image.fromarray(water, mode="F").resize(
                    (PREVIEW_CHUNK_SIZE, PREVIEW_CHUNK_SIZE), Image.Resampling.BOX
                ),
                dtype=np.float32,
            )
            y0, x0 = row * PREVIEW_CHUNK_SIZE, column * PREVIEW_CHUNK_SIZE
            depth_preview[y0 : y0 + PREVIEW_CHUNK_SIZE, x0 : x0 + PREVIEW_CHUNK_SIZE] = reduced_depth
            water_fraction[y0 : y0 + PREVIEW_CHUNK_SIZE, x0 : x0 + PREVIEW_CHUNK_SIZE] = reduced_water

    depth_t = np.sqrt(np.clip(depth_preview / 8000.0, 0.0, 1.0))
    shallow = np.array([51.0, 166.0, 190.0], dtype=np.float32)
    deep = np.array([4.0, 15.0, 54.0], dtype=np.float32)
    rgb = shallow[None, None] * (1.0 - depth_t[..., None]) + deep[None, None] * depth_t[..., None]
    land = np.array([25.0, 31.0, 28.0], dtype=np.float32)
    rgb = rgb * water_fraction[..., None] + land[None, None] * (1.0 - water_fraction[..., None])
    Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).save(
        region_dir / "water_depth_preview.png", optimize=True
    )


@click.command()
@click.argument("region_dir", type=click.Path(path_type=Path, exists=True, file_okay=False))
def main(region_dir: Path) -> None:
    """Create native-resolution ocean depth and mask chunks inside REGION_DIR."""
    missing_sources = [
        str(elevation_path(region_dir, row, column))
        for row in range(CHUNKS_PER_SIDE)
        for column in range(CHUNKS_PER_SIDE)
        if not valid_image(elevation_path(region_dir, row, column), "depth")
    ]
    if missing_sources:
        raise click.ClickException(
            f"Elevation export is incomplete: {len(missing_sources)} of "
            f"{CHUNKS_PER_SIDE**2} chunks are missing or invalid."
        )

    (region_dir / "water_depth").mkdir(exist_ok=True)
    (region_dir / "water_mask").mkdir(exist_ok=True)
    completed = 0
    water_pixels = 0
    maximum_depth_m = 0.0
    progress = tqdm(total=CHUNKS_PER_SIDE**2, desc="Water chunks")
    for row in range(CHUNKS_PER_SIDE):
        for column in range(CHUNKS_PER_SIDE):
            source = elevation_path(region_dir, row, column)
            depth_target = water_depth_path(region_dir, row, column)
            mask_target = water_mask_path(region_dir, row, column)
            with Image.open(source) as image:
                encoded_elevation = np.asarray(image, dtype=np.uint16)
            signed_half_metres = encoded_elevation.astype(np.int32) - ELEVATION_OFFSET
            depth_half_metres = np.maximum(-signed_half_metres, 0).astype(np.uint16)
            water_mask = np.where(depth_half_metres > 0, 255, 0).astype(np.uint8)

            water_pixels += int(np.count_nonzero(water_mask))
            maximum_depth_m = max(maximum_depth_m, float(depth_half_metres.max()) / UNITS_PER_METRE)
            if not valid_image(depth_target, "depth"):
                atomic_save(Image.fromarray(depth_half_metres), depth_target)
            if not valid_image(mask_target, "mask"):
                atomic_save(Image.fromarray(water_mask), mask_target)
            completed += 1
            progress.update(1)
            if completed % 16 == 0:
                atomic_json(
                    region_dir / "water_progress.json",
                    {"completed_chunks": completed, "total_chunks": CHUNKS_PER_SIDE**2},
                )
    progress.close()

    write_preview(region_dir)
    total_pixels = (CHUNK_SIZE * CHUNKS_PER_SIDE) ** 2
    manifest = {
        "source": "elevation chunks at sea level 0 m",
        "coverage": {"rows": CHUNKS_PER_SIDE, "columns": CHUNKS_PER_SIDE},
        "water_depth": {
            "directory": "water_depth",
            "format": "16-bit grayscale PNG",
            "decoded_depth_m": "uint16_pixel_value / 2",
            "quantization_m": 0.5,
            "land_value": 0,
            "maximum_depth_m": maximum_depth_m,
        },
        "water_mask": {
            "directory": "water_mask",
            "format": "8-bit grayscale PNG",
            "land_value": 0,
            "ocean_value": 255,
        },
        "ocean_fraction": water_pixels / total_pixels,
        "inland_hydrology_included": False,
    }
    atomic_json(region_dir / "water_manifest.json", manifest)
    atomic_json(
        region_dir / "water_progress.json",
        {"completed_chunks": CHUNKS_PER_SIDE**2, "total_chunks": CHUNKS_PER_SIDE**2, "complete": True},
    )
    click.echo(f"Water export complete: {manifest}")


if __name__ == "__main__":
    main()
