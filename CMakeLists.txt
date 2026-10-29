cmake_minimum_required(VERSION 3.25)
project(monopoly_port VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(netplay STATIC src/netplay/netplay.cpp src/netplay/video_codec.cpp src/netplay/lobby.cpp)
target_include_directories(netplay PUBLIC src)
target_compile_definitions(netplay PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(netplay PUBLIC ws2_32 iphlpapi mfplat mfuuid wmcodecdspuuid ole32 oleaut32 strmiids)

add_executable(monopoly_net WIN32 src/netplay/net_tool.cpp port/assets/monopoly.rc)
target_link_libraries(monopoly_net PRIVATE netplay xinput xaudio2 shell32 user32 gdi32)

# Launcher: Dear ImGui (from the ReXGlue SDK checkout) on D3D9's fixed-function pipeline, so it has no shaders.
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/rexglue-sdk/thirdparty/imgui)
# Default public lobby directory for new launchers (players can change it in Settings).
set(MONOPOLY_LOBBY_SERVER "" CACHE STRING "Public lobby directory URL, e.g. https://lobbies.example.org")
add_executable(monopoly_launcher WIN32 src/launcher/launcher.cpp src/launcher/upnp.cpp src/launcher/directory.cpp
    src/launcher/install.cpp
    src/launcher/launcher.rc
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp)
set_target_properties(monopoly_launcher PROPERTIES OUTPUT_NAME MonopolyLauncher)
target_include_directories(monopoly_launcher PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/rexglue-sdk/thirdparty/stb)
target_compile_definitions(monopoly_launcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX IMGUI_DISABLE_DEMO_WINDOWS
    MONOPOLY_LOBBY_SERVER="${MONOPOLY_LOBBY_SERVER}" MONOPOLY_VERSION="${PROJECT_VERSION}")
target_link_libraries(monopoly_launcher PRIVATE netplay winhttp d3d9 iphlpapi ws2_32 shell32 ole32 user32 gdi32 dwmapi)

enable_testing()
add_executable(netplay_selftest tests/netplay_selftest.cpp)
target_link_libraries(netplay_selftest PRIVATE netplay)
add_test(NAME netplay_selftest COMMAND netplay_selftest)

add_executable(lobby_selftest tests/lobby_selftest.cpp)
target_link_libraries(lobby_selftest PRIVATE netplay)
add_test(NAME lobby_selftest COMMAND lobby_selftest)

add_executable(install_selftest tests/install_selftest.cpp src/launcher/install.cpp)
target_include_directories(install_selftest PRIVATE src)
add_test(NAME install_selftest COMMAND install_selftest)

add_executable(directory_selftest tests/directory_selftest.cpp src/launcher/directory.cpp)
target_link_libraries(directory_selftest PRIVATE netplay winhttp)
add_test(NAME directory_selftest COMMAND directory_selftest)

# Asks the router for its internet address over UPnP (read-only; changes nothing on the router).
add_executable(upnp_probe tests/upnp_probe.cpp src/launcher/upnp.cpp)
target_link_libraries(upnp_probe PRIVATE netplay)

add_executable(net_probe tests/net_probe.cpp)
target_link_libraries(net_probe PRIVATE netplay)
