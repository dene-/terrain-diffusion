cmake_minimum_required(VERSION 3.25)

project(terrain_native VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Static dependencies keep the prototype self-contained on all three desktop
# platforms. Every revision is pinned so a future dependency update cannot
# silently change the renderer.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

set(SDLSHADERCROSS_VENDORED ON CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_STATIC ON CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_SPIRVCROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_DXC ON CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_CLI OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_TESTS OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3_shadercross
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_shadercross.git
    GIT_TAG e55cf5e31ced6f3d1be5cc6d0c50e99384f9f4ba
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(SDL3_shadercross)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)

set(HTTPLIB_TEST OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cpp_httplib)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(stb)

add_executable(terrain_native
    src/main.cpp
    src/Application.cpp
    src/CameraController.cpp
    src/Hud.cpp
    src/VisualConfig.cpp
    src/TerrainClient.cpp
    src/TerrainMesh.cpp
    src/TerrainStreamer.cpp
    src/Renderer.cpp
)

set_property(TARGET terrain_native PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

target_include_directories(terrain_native PRIVATE src)
target_include_directories(terrain_native SYSTEM PRIVATE "${stb_SOURCE_DIR}")
target_link_libraries(terrain_native PRIVATE
    SDL3::SDL3-static
    SDL3_shadercross::SDL3_shadercross-static
    glm::glm
    httplib::httplib
    nlohmann_json::nlohmann_json
)

target_compile_definitions(terrain_native PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_LEFT_HANDED
    GLM_FORCE_RADIANS
)

if(MSVC)
    target_compile_options(terrain_native PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(terrain_native PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        $<$<CONFIG:Release>:-ffp-contract=fast>
    )
endif()

if(APPLE)
    set_target_properties(terrain_native PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Terrain Native"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.terraindiffusion.native"
    )
    set(TERRAIN_SHADER_DESTINATION "$<TARGET_BUNDLE_CONTENT_DIR:terrain_native>/Resources/shaders")
    set(TERRAIN_ASSET_DESTINATION "$<TARGET_BUNDLE_CONTENT_DIR:terrain_native>/Resources/assets")
else()
    set(TERRAIN_SHADER_DESTINATION "$<TARGET_FILE_DIR:terrain_native>/shaders")
    set(TERRAIN_ASSET_DESTINATION "$<TARGET_FILE_DIR:terrain_native>/assets")
endif()

add_custom_target(terrain_native_runtime_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "${TERRAIN_SHADER_DESTINATION}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../web/public/assets"
        "${TERRAIN_ASSET_DESTINATION}"
    COMMENT "Synchronizing runtime shaders and vegetation assets"
)
add_dependencies(terrain_native terrain_native_runtime_assets)
