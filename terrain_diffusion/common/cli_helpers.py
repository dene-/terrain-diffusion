import json
import re
import click


def parse_cache_size(value: str | None) -> int | None:
    """Parse human-readable size (e.g., '100M', '1G', '500K') to bytes."""
    if value is None:
        return None
    value = value.strip().upper()
    match = re.fullmatch(r'(\d+(?:\.\d+)?)\s*([KMGT]?B?)', value)
    if not match:
        raise click.BadParameter(f"Invalid size format: {value}. Use e.g. 100M, 1G, 500K")
    num, suffix = float(match.group(1)), match.group(2).rstrip('B')
    multipliers = {'': 1, 'K': 1024, 'M': 1024**2, 'G': 1024**3, 'T': 1024**4}
    return int(num * multipliers.get(suffix, 1))


def parse_kwargs(kwargs_tuple):
    """Parse key=value tuples into a dict with automatic type inference."""
    result = {}
    for item in kwargs_tuple:
        if '=' not in item:
            raise click.BadParameter(f"Expected key=value format, got: {item}")
        key, value = item.split('=', 1)
        try:
            result[key] = json.loads(value)
        except json.JSONDecodeError:
            result[key] = value
    return result


def apply_performance_preset(
    preset: str | None,
    *,
    device: str | None,
    batch_size: str | None,
    cache_size: str | None,
    dtype: str | None,
    extra_kwargs,
    default_batch_size: str,
    default_cache_size: str,
):
    """Resolve inference defaults, allowing explicit CLI values to win."""
    kwargs = parse_kwargs(extra_kwargs)

    if preset == "mps":
        device = device or "mps"
        batch_size = batch_size or "8"
        cache_size = cache_size or "2G"
        dtype = dtype or "fp16"
        kwargs.setdefault("decoder_tile_size", 768)
        kwargs.setdefault("decoder_tile_stride", 640)

    return (
        device,
        batch_size or default_batch_size,
        cache_size or default_cache_size,
        dtype or "fp32",
        kwargs,
    )
