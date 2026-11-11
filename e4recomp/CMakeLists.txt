# e4 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
if(APPLE)
    project(e4 LANGUAGES CXX OBJCXX)
else()
    project(e4 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(E4_SOURCES
    src/main.cpp
    src/e4_particles.cpp
    src/e4_glow.cpp
    src/e4_leaderboard.cpp
)
# HTTPS for the online leaderboards.
if(WIN32)
    list(APPEND E4_SOURCES src/e4_http_win.cpp)
elseif(APPLE)
    list(APPEND E4_SOURCES src/e4_http_mac.mm)
endif()

if(WIN32)
    add_executable(e4 WIN32 ${E4_SOURCES})
else()
    add_executable(e4 ${E4_SOURCES})
endif()

rexglue_setup_target(e4 GPU_PLUGINS xenos)
# XXH3 for matching textures to replacement hashes (e4_particles.cpp).
target_link_libraries(e4 PRIVATE rex::xxhash)
if(WIN32)
    target_link_libraries(e4 PRIVATE winhttp)
elseif(APPLE)
    target_link_libraries(e4 PRIVATE "-framework Foundation")
    set_source_files_properties(src/e4_http_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

# Launcher: picks Original / HD Remaster, resolution and display options, then starts e4.
# SDL3 (from the ReXGlue SDK) + Dear ImGui (vendored in launcher/imgui).
set(E4_IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/launcher/imgui)
set(E4_LAUNCHER_SOURCES
    launcher/launcher.cpp
    launcher/calibration.cpp
    ${E4_IMGUI_DIR}/imgui.cpp
    ${E4_IMGUI_DIR}/imgui_draw.cpp
    ${E4_IMGUI_DIR}/imgui_tables.cpp
    ${E4_IMGUI_DIR}/imgui_widgets.cpp
    ${E4_IMGUI_DIR}/imgui_impl_sdl3.cpp
    ${E4_IMGUI_DIR}/imgui_impl_sdlgpu3.cpp
)
if(WIN32)
    add_executable(e4_launcher WIN32 ${E4_LAUNCHER_SOURCES})
else()
    add_executable(e4_launcher ${E4_LAUNCHER_SOURCES})
endif()
target_include_directories(e4_launcher PRIVATE ${E4_IMGUI_DIR})
target_link_libraries(e4_launcher PRIVATE SDL3::SDL3-static)
