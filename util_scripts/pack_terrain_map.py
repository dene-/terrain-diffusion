"""Pack a completed chunked terrain export into a streamable .tmap archive."""

from __future__ import annotations

import json
import os
import struct
import zlib
from pathlib import Path

import click
import numpy as np
from PIL import Image
from tqdm import tqdm


MAGIC = b"TDMAP001"
PREFIX = struct.Struct("<8sII")
INDEX_ENTRY = struct.Struct("<QII")


def _chunk_path(region_dir: Path, row: int, column: int) -> Path:
    return region_dir / "elevation" / f"elevation_y{row:02d}_x{column:02d}.png"


def _ecology_path(region_dir: Path, row: int, column: int) -> Path:
    return region_dir / "ecology" / f"ecology_y{row:02d}_x{column:02d}.f16"


def _ecology_tiles(
    region_dir: Path,
    rows: int,
    columns: int,
    origin_x_m: float,
    origin_z_m: float,
) -> tuple[list[Path], dict[str, object]] | None:
    manifest_path = region_dir / "ecology_manifest.json"
    if not manifest_path.is_file():
        return None
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    channels = manifest.get("channels", [])
    if (
        manifest.get("version") != 1
        or manifest.get("encoding") != "float16le_interleaved"
        or manifest.get("codec") != "raw"
        or int(manifest.get("rows", 0)) != rows
        or int(manifest.get("columns", 0)) != columns
        or len(channels) != 8
    ):
        raise click.ClickException(f"Unsupported ecology manifest: {manifest_path}")
    chunk_width = int(manifest["chunk_width"])
    chunk_height = int(manifest["chunk_height"])
    resolution_m = float(manifest["resolution_m"])
    bytes_per_chunk = int(manifest["bytes_per_chunk"])
    expected_bytes = chunk_width * chunk_height * len(channels) * 2
    if (
        chunk_width < 2
        or chunk_height < 2
        or resolution_m <= 0.0
        or bytes_per_chunk != expected_bytes
    ):
        raise click.ClickException(f"Invalid ecology dimensions in {manifest_path}")
    paths = [
        _ecology_path(region_dir, row, column)
        for row in range(rows)
        for column in range(columns)
    ]
    invalid = [path for path in paths if not path.is_file() or path.stat().st_size != expected_bytes]
    if invalid:
        raise click.ClickException(
            f"Ecology export is incomplete: {len(invalid)} of {len(paths)} tiles are missing or invalid"
        )
    metadata: dict[str, object] = {
        "ecology_channels": len(channels),
        "ecology_channel_names": channels,
        "ecology_rows": rows,
        "ecology_columns": columns,
        "ecology_chunk_width": chunk_width,
        "ecology_chunk_height": chunk_height,
        "ecology_resolution_m": resolution_m,
        "ecology_origin_x_m": origin_x_m + resolution_m * 0.5,
        "ecology_origin_z_m": origin_z_m + resolution_m * 0.5,
        "ecology_encoding": "float16le_interleaved",
        "ecology_codec": "raw",
        "ecology_index_entries": len(paths),
        "ecology_index_entry_bytes": INDEX_ENTRY.size,
        "ecology_index_order": "row-major",
        "ecology_bytes_per_chunk": expected_bytes,
        "ecology_payload_bytes": expected_bytes * len(paths),
    }
    return paths, metadata


def _climate_payload(
    region_dir: Path,
    origin_x_m: float,
    origin_z_m: float,
    width_m: float,
    height_m: float,
) -> tuple[bytes, dict[str, object]] | None:
    coarse_path = region_dir / "coarse_fields.npz"
    if not coarse_path.is_file():
        return None

    with np.load(coarse_path) as archive:
        if "fields" not in archive:
            raise click.ClickException(f"Missing fields array in {coarse_path}")
        fields = archive["fields"]
    if fields.ndim != 3 or fields.shape[0] < 6:
        raise click.ClickException(
            f"Invalid coarse climate fields in {coarse_path}: expected at least 6xHxW"
        )

    climate = np.asarray(np.moveaxis(fields[2:6], 0, -1), dtype="<f4", order="C")
    if climate.shape[0] < 2 or climate.shape[1] < 2 or not np.isfinite(climate).all():
        raise click.ClickException(f"Invalid or non-finite climate grid in {coarse_path}")
    height, width, channels = climate.shape
    resolution_x = width_m / width
    resolution_z = height_m / height
    if not np.isclose(resolution_x, resolution_z, rtol=0.0, atol=1e-6):
        raise click.ClickException("Climate grid cells must be square")

    payload = climate.tobytes(order="C")
    metadata: dict[str, object] = {
        "climate_channels": channels,
        "climate_channel_names": [
            "temperature_mean_c",
            "temperature_std_c_x100",
            "precipitation_mean_mm",
            "precipitation_cv_percent",
        ],
        "climate_width": width,
        "climate_height": height,
        "climate_resolution_m": resolution_x,
        # Coarse values represent cell centres, not map-edge vertices.
        "climate_origin_x_m": origin_x_m + resolution_x * 0.5,
        "climate_origin_z_m": origin_z_m + resolution_z * 0.5,
        "climate_encoding": "float32le_interleaved",
        "climate_payload_bytes": len(payload),
        "climate_checksum": zlib.crc32(payload) & 0xFFFFFFFF,
    }
    return payload, metadata


@click.command()
@click.argument("region_dir", type=click.Path(path_type=Path, exists=True, file_okay=False))
@click.option("--output", type=click.Path(path_type=Path), default=None)
def main(region_dir: Path, output: Path | None) -> None:
    """Pack REGION_DIR into one indexed, random-access terrain map."""
    manifest_path = region_dir / "manifest.json"
    if not manifest_path.is_file():
        raise click.ClickException(f"Missing terrain manifest: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    chunks = manifest["chunks"]
    coverage = manifest["coverage"]
    encoding = manifest["elevation_encoding"]
    rows = int(chunks["rows"])
    columns = int(chunks["columns"])
    chunk_width = int(chunks["width_pixels"])
    chunk_height = int(chunks["height_pixels"])
    resolution_m = float(manifest["native_resolution_m_per_pixel"])
    width_m = float(coverage["width_km"]) * 1000.0
    height_m = float(coverage["height_km"]) * 1000.0
    origin_x_m = -width_m * 0.5
    origin_z_m = -height_m * 0.5
    quantization_m = float(encoding["quantization_m"])
    if quantization_m <= 0.0:
        raise click.ClickException("Elevation quantization must be positive")

    paths = [
        _chunk_path(region_dir, row, column)
        for row in range(rows)
        for column in range(columns)
    ]
    missing = [path for path in paths if not path.is_file()]
    if missing:
        raise click.ClickException(
            f"Terrain export is incomplete: {len(missing)} of {len(paths)} chunks are missing"
        )
    for path in paths:
        with Image.open(path) as image:
            if image.size != (chunk_width, chunk_height) or image.mode not in {"I;16", "I"}:
                raise click.ClickException(f"Invalid elevation chunk: {path}")

    climate = _climate_payload(
        region_dir, origin_x_m, origin_z_m, width_m, height_m
    )
    climate_payload, climate_metadata = climate or (b"", {"climate_channels": 0})
    ecology = _ecology_tiles(
        region_dir, rows, columns, origin_x_m, origin_z_m
    )
    ecology_paths, ecology_metadata = ecology or ([], {"ecology_channels": 0})
    archive_metadata = {
        "format": "terrain-diffusion-map",
        "version": 3 if ecology else 2 if climate else 1,
        "id": region_dir.name,
        "name": manifest.get("name", region_dir.name),
        "seed": str(manifest["seed"]),
        "layout_seed": str(manifest.get("layout_seed", manifest["seed"])),
        "range_style": manifest.get("range_style", "unknown"),
        "rows": rows,
        "columns": columns,
        "chunk_width": chunk_width,
        "chunk_height": chunk_height,
        "native_resolution_m": resolution_m,
        "latent_resolution_m": resolution_m * 8.0,
        "coarse_resolution_m": resolution_m * 8.0 * 48.0,
        "origin_x_m": origin_x_m,
        "origin_z_m": origin_z_m,
        "width_m": width_m,
        "height_m": height_m,
        "elevation_offset": 32768,
        "elevation_units_per_m": 1.0 / quantization_m,
        "outside_elevation_m": -6500.0,
        "chunk_codec": "png16",
        "index_order": "row-major",
        "index_entry_bytes": INDEX_ENTRY.size,
        **climate_metadata,
        **ecology_metadata,
    }
    header = json.dumps(archive_metadata, separators=(",", ":"), sort_keys=True).encode("utf-8")
    data_offset = PREFIX.size + len(header) + (len(paths) + len(ecology_paths)) * INDEX_ENTRY.size
    index: list[tuple[int, int, int]] = []
    offset = data_offset
    for path in tqdm(paths, desc="Reading terrain chunks"):
        payload = path.read_bytes()
        index.append((offset, len(payload), zlib.crc32(payload) & 0xFFFFFFFF))
        offset += len(payload)
    offset += len(climate_payload)
    ecology_index: list[tuple[int, int, int]] = []
    for path in tqdm(ecology_paths, desc="Reading ecology tiles"):
        payload = path.read_bytes()
        ecology_index.append((offset, len(payload), zlib.crc32(payload) & 0xFFFFFFFF))
        offset += len(payload)

    output = output or region_dir / "terrain.tmap"
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f"{output.name}.tmp")
    with temporary.open("wb") as stream:
        stream.write(PREFIX.pack(MAGIC, len(header), INDEX_ENTRY.size))
        stream.write(header)
        for entry in index:
            stream.write(INDEX_ENTRY.pack(*entry))
        for entry in ecology_index:
            stream.write(INDEX_ENTRY.pack(*entry))
        for path in tqdm(paths, desc="Writing terrain archive"):
            stream.write(path.read_bytes())
        if climate_payload:
            stream.write(climate_payload)
        for path in tqdm(ecology_paths, desc="Writing ecology archive"):
            stream.write(path.read_bytes())
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, output)
    click.echo(
        json.dumps(
            {
                "archive": str(output),
                "size_bytes": output.stat().st_size,
                "chunks": len(paths),
                "climate_payload_bytes": len(climate_payload),
                "ecology_tiles": len(ecology_paths),
                "ecology_payload_bytes": sum(path.stat().st_size for path in ecology_paths),
                "metadata": archive_metadata,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
