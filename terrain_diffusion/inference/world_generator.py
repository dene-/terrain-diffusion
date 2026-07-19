import click
from terrain_diffusion.common.device import select_device
from terrain_diffusion.inference.world_pipeline import WorldPipeline, resolve_hdf5_path
from terrain_diffusion.common.cli_helpers import apply_performance_preset, parse_cache_size
from tqdm import tqdm


def generate_world(model_path: str, hdf5_file: str | None = None, seed: int | None = None, coarse_window: int = 64, device: str | None = None, caching_strategy: str = 'direct', **kwargs) -> None:
    device = select_device(device)

    world = WorldPipeline.from_pretrained(model_path, seed=seed, caching_strategy=caching_strategy, **kwargs)
    world.to(device)
    if caching_strategy == 'direct':
        world.bind(hdf5_file=hdf5_file)
    else:
        world.bind(hdf5_file or 'TEMP')
    with world:
        print(f"World seed: {world.seed}")
        ci0, ci1 = -coarse_window, coarse_window
        cj0, cj1 = -coarse_window, coarse_window
        
        tile_size = 2048
        pbar = tqdm(total=((ci1-ci0)//8)*((cj1-cj0)//8), desc="Generating world")
        for i in range(ci0, ci1, tile_size//256):
            for j in range(cj0, cj1, tile_size//256):
                world.residual[:, i*256:i*256+tile_size, j*256:j*256+tile_size]
                pbar.update(1)


@click.command()
@click.argument("model_path", default="xandergos/terrain-diffusion-30m")
@click.option("--caching-strategy", type=click.Choice(["indirect", "direct"]), default="direct", help="Caching strategy: 'indirect' uses HDF5, 'direct' uses in-memory LRU cache")
@click.option("--hdf5-file", default=None, help="HDF5 file path (required for indirect caching, optional for direct)")
@click.option("--cache-size", default=None, help="Cache size (default: 100M; MPS preset: 2G)")
@click.option("--seed", type=int, default=None, help="Random seed (default: random or from file)")
@click.option("--device", type=click.Choice(["cuda", "mps", "cpu"]), default=None, help="Device (default: auto)")
@click.option("--batch-size", type=str, default=None, help="Latent batch size (default: 1,2,4,8,16; MPS preset: 8)")
@click.option("--log-mode", type=click.Choice(["info", "verbose"]), default="verbose", help="Logging mode")
@click.option("--coarse-window", type=int, default=50, help="Coarse window size")
@click.option("--compile/--no-compile", "torch_compile", default=True, help="Use torch.compile for faster inference")
@click.option("--dtype", type=click.Choice(["fp32", "bf16", "fp16"]), default=None, help="Model dtype (default: fp32; MPS preset: fp16)")
@click.option("--performance-preset", type=click.Choice(["mps"]), default=None)
@click.option("--kwarg", "extra_kwargs", multiple=True, help="Additional key=value kwargs (e.g. --kwarg coarse_pooling=2)")
def main(model_path, hdf5_file, caching_strategy, cache_size, seed, device, batch_size, log_mode, coarse_window, torch_compile, dtype, performance_preset, extra_kwargs):
    """Generate a world using the terrain diffusion pipeline"""
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
        default_batch_size="1,2,4,8,16",
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
    generate_world(
        model_path,
        hdf5_file,
        seed=seed,
        coarse_window=coarse_window,
        device=device,
        latents_batch_size=batch_sizes,
        log_mode=log_mode,
        torch_compile=torch_compile,
        dtype=dtype,
        caching_strategy=caching_strategy,
        cache_limit=parse_cache_size(cache_size),
        **kwargs,
    )


if __name__ == "__main__":
    main()
