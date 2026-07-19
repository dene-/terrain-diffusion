# Terrain Stream Godot client

This directory is the new Godot 4.7.1 Forward+ presentation layer. Terrain
Diffusion and the renderer-independent C++ streaming core remain authoritative.

## Build the GDExtension on Apple Silicon

```bash
cmake -S godot/extension -B godot/extension/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build godot/extension/build --parallel
/Applications/Godot.app/Contents/MacOS/Godot --path godot --editor
```

Use a Debug build during migration because `reloadable = true` permits editor
hot reload. Use `-DCMAKE_BUILD_TYPE=Release` for performance captures once the
vertical slice is operational. Use separate Debug and Release build directories
because the chosen CMake build type also selects the matching `godot-cpp`
template ABI and output filename.

The pinned `godot-cpp` commit is fetched by CMake and generated against API
version 4.7. Generated libraries are placed in `godot/bin/`; they are build
artifacts and are not committed.

The Terrain Diffusion API is not required to open this initial scaffold. It
will be required once the shared terrain core is connected.
