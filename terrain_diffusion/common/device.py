"""Runtime device selection shared by inference entry points."""

import os

import torch


def select_device(requested: str | None = None) -> str:
    """Resolve an explicit, environment, or automatically detected device."""
    if requested:
        return requested

    environment_device = os.getenv("TERRAIN_DEVICE")
    if environment_device:
        return environment_device

    if torch.cuda.is_available():
        return "cuda"

    mps = getattr(torch.backends, "mps", None)
    if mps is not None and mps.is_available():
        return "mps"

    print("Warning: No CUDA or MPS accelerator is available; using CPU.")
    return "cpu"
