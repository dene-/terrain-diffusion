cmake_minimum_required(VERSION 3.25)

include(FetchContent)
find_package(Threads REQUIRED)

# Keep the renderer-independent terrain pipeline usable from either the native
# SDL client or the Godot GDExtension. Each dependency is pinned to the same
# revision the native client used before this target was extracted.
if(NOT TARGET glm::glm)
    FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 1.0.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glm)
endif()

if(NOT TARGET httplib::httplib)
    set(HTTPLIB_TEST OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        cpp_httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.18.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(cpp_httplib)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(terrain_core STATIC
    src/TerrainClient.cpp
    src/TerrainMesh.cpp
    src/TerrainStreamer.cpp
    src/VisualConfig.cpp
)

add_library(terrain::core ALIAS terrain_core)

target_compile_features(terrain_core PRIVATE cxx_std_23)
set_target_properties(terrain_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(terrain_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(terrain_core
    PUBLIC
        glm::glm
        httplib::httplib
        Threads::Threads
    PRIVATE
        nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(terrain_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(terrain_core PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        $<$<CONFIG:Release>:-ffp-contract=fast>
    )
endif()
