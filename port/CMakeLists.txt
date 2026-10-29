# monopoly - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(monopoly LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(MONOPOLY_SOURCES
    src/main.cpp
    src/netplay_bridge.cpp
    src/script_input.cpp
)

# Online play (shared with the monopoly_net join tool in the top-level project).
add_library(netplay STATIC ../src/netplay/netplay.cpp ../src/netplay/video_codec.cpp ../src/netplay/lobby.cpp)
target_include_directories(netplay PUBLIC ../src)
target_compile_definitions(netplay PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(netplay PUBLIC ws2_32 iphlpapi mfplat mfuuid wmcodecdspuuid ole32 oleaut32 strmiids)

if(WIN32)
    enable_language(RC)
    add_executable(monopoly WIN32 ${MONOPOLY_SOURCES} assets/monopoly.rc)
else()
    add_executable(monopoly ${MONOPOLY_SOURCES})
endif()

rexglue_setup_target(monopoly GPU_PLUGINS xenos)
target_link_libraries(monopoly PRIVATE netplay)

# Precompiled shader storage (built with tools/warm_shader_cache.ps1) ships next to the exe;
# the app seeds the user's cache from it at startup.
add_custom_command(TARGET monopoly POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/shader_cache $<TARGET_FILE_DIR:monopoly>/shader_cache
    VERBATIM)

# A PDB with public symbols, so native stacks resolve to the recompiled sub_XXXXXXXX names.
if(WIN32)
    target_link_options(monopoly PRIVATE "LINKER:/DEBUG")
endif()
