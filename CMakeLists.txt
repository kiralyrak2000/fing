cmake_minimum_required(VERSION 3.20)
project(vanish CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GS- /Gy")

add_library(vanish SHARED dll/dllmain.cpp)
set_target_properties(vanish PROPERTIES OUTPUT_NAME vanish PREFIX "")
target_link_libraries(vanish PRIVATE d3d11 dxgi d2d1 dwrite)

add_executable(injector injector/main.cpp)
