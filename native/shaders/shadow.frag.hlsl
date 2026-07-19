// SDL_GPU requires a fragment stage even for a depth-only graphics pipeline.
// Depth is written by fixed-function rasterization; no color target is bound.
void main()
{
}

