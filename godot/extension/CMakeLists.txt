cmake_minimum_required(VERSION 3.25)

project(terrain_stream_godot VERSION 0.1.0 LANGUAGES CXX)

# The extension build lives below the Godot project. Keep Godot's resource
# scanner out of CMake products and fetched dependencies (some dependencies
# intentionally contain malformed image fixtures).
file(WRITE "${CMAKE_BINARY_DIR}/.gdignore" "")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Godot's editor selects the template_debug GDExtension ABI, but this client
# performs production-scale streaming and visibility work while iterating in
# the editor. Keep symbols/assertions while avoiding an otherwise unoptimized
# terrain core that makes editor play misleadingly slower than the native app.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(MSVC)
        add_compile_options(/O2)
    else()
        add_compile_options(-O2)
    endif()
endif()

include(FetchContent)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../terrain_core"
    "${CMAKE_CURRENT_BINARY_DIR}/terrain_core"
)

# godot-cpp currently has no 4.7 release branch. This exact upstream commit is
# the first stable master snapshot used here whose generated API advertises
# Godot 4.7 support. Pinning avoids silently moving with master.
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "" FORCE)
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    set(GODOTCPP_TARGET "template_release" CACHE STRING "" FORCE)
else()
    set(GODOTCPP_TARGET "template_debug" CACHE STRING "" FORCE)
endif()
set(GODOTCPP_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    godot_cpp
    GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
    GIT_TAG ba0edfed90512ec64aba51d4295a3e7e30112f86
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(godot_cpp)

add_library(terrain_stream_godot SHARED
    src/register_types.cpp
    src/PlayerController.cpp
    src/TerrainGodotAdapter.cpp
    src/WorldVisualSettings.cpp
    src/TerrainWorld3D.cpp
)

target_include_directories(terrain_stream_godot PRIVATE src)
target_link_libraries(terrain_stream_godot PRIVATE godot-cpp terrain::core)

get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)

set(TERRAIN_GODOT_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bin")
set_target_properties(terrain_stream_godot PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    POSITION_INDEPENDENT_CODE ON
    BUILD_RPATH_USE_ORIGIN ON
    LIBRARY_OUTPUT_DIRECTORY "$<1:${TERRAIN_GODOT_OUTPUT_DIR}>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${TERRAIN_GODOT_OUTPUT_DIR}>"
    PDB_OUTPUT_DIRECTORY "$<1:${TERRAIN_GODOT_OUTPUT_DIR}>"
    OUTPUT_NAME "terrain_stream${GODOTCPP_SUFFIX}"
)

if(MSVC)
    target_compile_options(terrain_stream_godot PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(terrain_stream_godot PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
