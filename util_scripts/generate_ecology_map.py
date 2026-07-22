"""Generate 120 m FP16 environmental tiles from final terrain and macro climate."""

from __future__ import annotations

import json
import os
from pathlib import Path

import click
import numpy as np
from PIL import Image
from scipy import ndimage
from tqdm import tqdm


DOWNSAMPLE_FACTOR = 4
CHANNEL_NAMES = (
    "temperature_mean_c",
    "temperature_std_c_x100",
    "precipitation_mean_mm",
    "precipitation_cv_percent",
    "soil_moisture_index",
    "drainage_index",
    "wind_exposure_index",
    "substrate_rockiness_index",
)
CHANNEL_COUNT = len(CHANNEL_NAMES)
ELEVATION_OFFSET = 32768


def _elevation_path(region_dir: Path, row: int, column: int) -> Path:
    return region_dir / "elevation" / f"elevation_y{row:02d}_x{column:02d}.png"


def _ecology_path(region_dir: Path, row: int, column: int) -> Path:
    return region_dir / "ecology" / f"ecology_y{row:02d}_x{column:02d}.f16"


def _atomic_json(path: Path, payload: dict[str, object]) -> None:
    temporary = path.with_name(f"{path.name}.tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _atomic_bytes(path: Path, payload: bytes) -> None:
    temporary = path.with_name(f"{path.name}.tmp")
    temporary.write_bytes(payload)
    os.replace(temporary, path)


def _resample_cells(source: np.ndarray, shape: tuple[int, int]) -> np.ndarray:
    """Bilinearly resample cell-centred data while preserving the map extent."""
    zoom = (shape[0] / source.shape[0], shape[1] / source.shape[1])
    return ndimage.zoom(
        source,
        zoom=zoom,
        order=1,
        mode="nearest",
        prefilter=False,
        grid_mode=True,
    ).astype(np.float32, copy=False)


def _smoothstep(edge0: float, edge1: float, value: np.ndarray) -> np.ndarray:
    t = np.clip((value - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _load_elevation(
    region_dir: Path,
    rows: int,
    columns: int,
    chunk_width: int,
    chunk_height: int,
    quantization_m: float,
) -> np.ndarray:
    ecology_width = chunk_width // DOWNSAMPLE_FACTOR
    ecology_height = chunk_height // DOWNSAMPLE_FACTOR
    elevation = np.empty(
        (rows * ecology_height, columns * ecology_width), dtype=np.float32
    )
    progress = tqdm(total=rows * columns, desc="Downsampling elevation")
    for row in range(rows):
        for column in range(columns):
            path = _elevation_path(region_dir, row, column)
            if not path.is_file():
                raise click.ClickException(f"Missing elevation chunk: {path}")
            with Image.open(path) as image:
                encoded = np.asarray(image, dtype=np.uint16)
            if encoded.shape != (chunk_height, chunk_width):
                raise click.ClickException(f"Invalid elevation chunk dimensions: {path}")
            metres = (encoded.astype(np.int32) - ELEVATION_OFFSET) * quantization_m
            reduced = metres.reshape(
                ecology_height,
                DOWNSAMPLE_FACTOR,
                ecology_width,
                DOWNSAMPLE_FACTOR,
            ).mean(axis=(1, 3), dtype=np.float32)
            y0 = row * ecology_height
            x0 = column * ecology_width
            elevation[y0 : y0 + ecology_height, x0 : x0 + ecology_width] = reduced
            progress.update(1)
    progress.close()
    return elevation


def _generate_channels(
    region_dir: Path,
    elevation: np.ndarray,
    native_resolution_m: float,
) -> tuple[np.memmap, Path]:
    coarse_path = region_dir / "coarse_fields.npz"
    if not coarse_path.is_file():
        raise click.ClickException(f"Missing macro climate source: {coarse_path}")
    with np.load(coarse_path) as archive:
        fields = np.asarray(archive["fields"], dtype=np.float32)
        coarse_elevation = np.asarray(archive["elevation_m"], dtype=np.float32)
    if fields.ndim != 3 or fields.shape[0] < 6 or coarse_elevation.shape != fields.shape[1:]:
        raise click.ClickException("coarse_fields.npz has an unsupported layout")

    ecology_dir = region_dir / "ecology"
    ecology_dir.mkdir(exist_ok=True)
    temporary = ecology_dir / ".ecology_global.tmp"
    output = np.memmap(
        temporary,
        mode="w+",
        dtype="<f2",
        shape=(*elevation.shape, CHANNEL_COUNT),
    )

    resolution_m = native_resolution_m * DOWNSAMPLE_FACTOR
    broad_elevation = ndimage.gaussian_filter(elevation, sigma=24.0, mode="nearest")
    terrain_elevation = np.maximum(elevation, 0.0)

    temperature = _resample_cells(fields[2], elevation.shape)
    macro_elevation = np.maximum(_resample_cells(coarse_elevation, elevation.shape), 0.0)
    # Apply a standard environmental lapse rate to the final 30 m terrain,
    # which contains mountain and valley detail absent from the macro grid.
    temperature -= (terrain_elevation - macro_elevation) * 0.0065
    temperature = np.clip(temperature, -45.0, 52.0)
    output[:, :, 0] = temperature
    del macro_elevation

    temperature_std_x100 = np.clip(
        _resample_cells(fields[3], elevation.shape), 0.0, 2_000.0
    )
    output[:, :, 1] = temperature_std_x100
    del temperature_std_x100

    precipitation = _resample_cells(fields[4], elevation.shape)
    # The authored continent's dominant moisture flow is west-to-east. A
    # broad elevation derivative adds local windward lift and leeward drying
    # without turning individual 120 m ridges into climate discontinuities.
    eastward_gradient = np.gradient(broad_elevation, resolution_m, axis=1)
    precipitation *= np.exp(np.clip(eastward_gradient * 2.2, -0.35, 0.45))
    precipitation = np.clip(precipitation, 40.0, 5_500.0)
    output[:, :, 2] = precipitation
    del eastward_gradient

    precipitation_cv = np.clip(
        _resample_cells(fields[5], elevation.shape), 0.0, 150.0
    )
    output[:, :, 3] = precipitation_cv

    local_elevation = ndimage.gaussian_filter(elevation, sigma=2.0, mode="nearest")
    gradient_z, gradient_x = np.gradient(local_elevation, resolution_m)
    slope = np.hypot(gradient_x, gradient_z)
    local_mean = ndimage.gaussian_filter(elevation, sigma=12.0, mode="nearest")
    relative_height = elevation - local_mean
    ridge_position = np.clip(0.5 + relative_height / 600.0, 0.0, 1.0)
    drainage = np.clip(slope / 0.42 * 0.68 + ridge_position * 0.32, 0.0, 1.0)
    drainage[elevation <= 0.0] = 0.0
    output[:, :, 5] = drainage

    broad_relative_height = elevation - broad_elevation
    exposure = np.clip(
        0.18 + np.maximum(broad_relative_height, 0.0) / 850.0 + slope / 0.65 * 0.34,
        0.0,
        1.0,
    )
    exposure[elevation <= 0.0] = 0.0
    output[:, :, 6] = exposure

    local_square_mean = ndimage.gaussian_filter(
        np.square(elevation, dtype=np.float32), sigma=2.0, mode="nearest"
    )
    ruggedness = np.sqrt(
        np.maximum(local_square_mean - np.square(local_elevation, dtype=np.float32), 0.0)
    )
    high_alpine = _smoothstep(2_100.0, 4_000.0, elevation)
    rockiness = np.clip(
        slope / 0.52 * 0.52 + ruggedness / 220.0 * 0.28 + high_alpine * 0.34,
        0.0,
        1.0,
    )
    rockiness[elevation <= 0.0] = 1.0
    output[:, :, 7] = rockiness

    effective_temperature = np.maximum(temperature, 0.0)
    evapotranspiration = np.maximum(
        250.0,
        250.0 + 25.0 * effective_temperature + 0.7 * np.square(effective_temperature),
    )
    water_balance = precipitation / evapotranspiration
    water_balance *= 1.0 - 0.35 * np.minimum(precipitation_cv / 100.0, 1.0)
    convergence = np.clip(0.5 + (local_mean - elevation) / 650.0, 0.0, 1.0)
    soil_moisture = np.clip(
        ((water_balance - 0.10) / 1.25)
        * (0.55 + convergence * 0.55)
        * (1.0 - drainage * 0.42)
        * (1.0 - exposure * 0.22),
        0.0,
        1.0,
    )
    soil_moisture[elevation <= 0.0] = 0.0
    output[:, :, 4] = soil_moisture
    output.flush()
    return output, temporary


def _write_preview(region_dir: Path, ecology: np.memmap, elevation: np.ndarray) -> None:
    preview_size = 1_024
    soil = np.asarray(
        Image.fromarray(np.asarray(ecology[:, :, 4], dtype=np.float32), mode="F").resize(
            (preview_size, preview_size), Image.Resampling.BILINEAR
        ),
        dtype=np.float32,
    )
    rock = np.asarray(
        Image.fromarray(np.asarray(ecology[:, :, 7], dtype=np.float32), mode="F").resize(
            (preview_size, preview_size), Image.Resampling.BILINEAR
        ),
        dtype=np.float32,
    )
    temperature = np.asarray(
        Image.fromarray(np.asarray(ecology[:, :, 0], dtype=np.float32), mode="F").resize(
            (preview_size, preview_size), Image.Resampling.BILINEAR
        ),
        dtype=np.float32,
    )
    reduced_elevation = np.asarray(
        Image.fromarray(elevation, mode="F").resize(
            (preview_size, preview_size), Image.Resampling.BILINEAR
        ),
        dtype=np.float32,
    )
    dry = np.array([174.0, 146.0, 82.0], dtype=np.float32)
    wet = np.array([48.0, 120.0, 62.0], dtype=np.float32)
    rgb = dry + (wet - dry) * soil[:, :, None]
    stone = np.array([124.0, 126.0, 123.0], dtype=np.float32)
    rgb = rgb * (1.0 - rock[:, :, None] * 0.72) + stone * rock[:, :, None] * 0.72
    snow = np.clip((1.0 - (temperature + 8.0) / 12.0) * 0.65
        + _smoothstep(2_500.0, 4_800.0, reduced_elevation) * 0.65, 0.0, 1.0)
    rgb = rgb * (1.0 - snow[:, :, None]) + np.array(
        [226.0, 233.0, 238.0], dtype=np.float32
    ) * snow[:, :, None]
    ocean = reduced_elevation <= 0.0
    depth = np.sqrt(np.clip(-reduced_elevation / 8_000.0, 0.0, 1.0))
    ocean_rgb = np.stack(
        [28.0 - depth * 22.0, 112.0 - depth * 86.0, 163.0 - depth * 90.0], axis=-1
    )
    rgb[ocean] = ocean_rgb[ocean]
    Image.fromarray(np.clip(rgb, 0.0, 255.0).astype(np.uint8)).save(
        region_dir / "ecology_preview.png", optimize=True
    )


@click.command()
@click.argument("region_dir", type=click.Path(path_type=Path, exists=True, file_okay=False))
def main(region_dir: Path) -> None:
    """Generate detailed environmental tiles for REGION_DIR."""
    manifest_path = region_dir / "manifest.json"
    if not manifest_path.is_file():
        raise click.ClickException(f"Missing terrain manifest: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    chunks = manifest["chunks"]
    encoding = manifest["elevation_encoding"]
    rows = int(chunks["rows"])
    columns = int(chunks["columns"])
    chunk_width = int(chunks["width_pixels"])
    chunk_height = int(chunks["height_pixels"])
    native_resolution_m = float(manifest["native_resolution_m_per_pixel"])
    quantization_m = float(encoding["quantization_m"])
    if chunk_width % DOWNSAMPLE_FACTOR or chunk_height % DOWNSAMPLE_FACTOR:
        raise click.ClickException("Elevation chunk dimensions must be divisible by four")

    elevation = _load_elevation(
        region_dir,
        rows,
        columns,
        chunk_width,
        chunk_height,
        quantization_m,
    )
    ecology, temporary = _generate_channels(region_dir, elevation, native_resolution_m)
    ecology_chunk_width = chunk_width // DOWNSAMPLE_FACTOR
    ecology_chunk_height = chunk_height // DOWNSAMPLE_FACTOR
    expected_bytes = ecology_chunk_width * ecology_chunk_height * CHANNEL_COUNT * 2
    progress = tqdm(total=rows * columns, desc="Writing ecology tiles")
    for row in range(rows):
        for column in range(columns):
            y0 = row * ecology_chunk_height
            x0 = column * ecology_chunk_width
            tile = np.ascontiguousarray(
                ecology[
                    y0 : y0 + ecology_chunk_height,
                    x0 : x0 + ecology_chunk_width,
                    :,
                ],
                dtype="<f2",
            )
            payload = tile.tobytes(order="C")
            if len(payload) != expected_bytes:
                raise click.ClickException("Generated ecology tile has an invalid size")
            _atomic_bytes(_ecology_path(region_dir, row, column), payload)
            progress.update(1)
    progress.close()

    _write_preview(region_dir, ecology, elevation)
    statistics = {
        name: {
            "minimum": float(np.min(ecology[:, :, channel])),
            "maximum": float(np.max(ecology[:, :, channel])),
            "mean": float(np.mean(ecology[:, :, channel], dtype=np.float64)),
        }
        for channel, name in enumerate(CHANNEL_NAMES)
    }
    ecology_manifest: dict[str, object] = {
        "version": 1,
        "source": "final 30 m elevation downsampled with macro climate refinement",
        "rows": rows,
        "columns": columns,
        "chunk_width": ecology_chunk_width,
        "chunk_height": ecology_chunk_height,
        "resolution_m": native_resolution_m * DOWNSAMPLE_FACTOR,
        "encoding": "float16le_interleaved",
        "codec": "raw",
        "channels": list(CHANNEL_NAMES),
        "bytes_per_chunk": expected_bytes,
        "statistics": statistics,
    }
    _atomic_json(region_dir / "ecology_manifest.json", ecology_manifest)
    del ecology
    temporary.unlink(missing_ok=True)
    click.echo(json.dumps(ecology_manifest, indent=2))


if __name__ == "__main__":
    main()
