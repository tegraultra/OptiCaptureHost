cmake_minimum_required(VERSION 3.20)
project(OptiCaptureHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(OptiCaptureHost WIN32 src/main.cpp src/host.rc)

target_compile_definitions(OptiCaptureHost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(OptiCaptureHost PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(OptiCaptureHost PRIVATE d3d11 d3d12 d3dcompiler dxgi dwmapi shcore shell32 user32 windowsapp)

# The launcher: plain GDI+ window (no Direct3D, so OptiScaler's dxgi.dll is never loaded into it).
add_executable(OptiCaptureLauncher WIN32 src/launcher.cpp src/launcher.rc)

target_compile_definitions(OptiCaptureLauncher PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(OptiCaptureLauncher PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(OptiCaptureLauncher PRIVATE gdiplus user32 gdi32 shell32 dwmapi)
