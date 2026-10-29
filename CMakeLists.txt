cmake_minimum_required(VERSION 3.25)
project(monopoly_port LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(netplay STATIC src/netplay/netplay.cpp src/netplay/video_codec.cpp)
target_include_directories(netplay PUBLIC src)
target_compile_definitions(netplay PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(netplay PUBLIC ws2_32 mfplat mfuuid wmcodecdspuuid ole32 oleaut32 strmiids)

add_executable(monopoly_net WIN32 src/netplay/net_tool.cpp)
target_link_libraries(monopoly_net PRIVATE netplay xinput xaudio2 shell32 user32 gdi32)

enable_testing()
add_executable(netplay_selftest tests/netplay_selftest.cpp)
target_link_libraries(netplay_selftest PRIVATE netplay)
add_test(NAME netplay_selftest COMMAND netplay_selftest)

add_executable(net_probe tests/net_probe.cpp)
target_link_libraries(net_probe PRIVATE netplay)
