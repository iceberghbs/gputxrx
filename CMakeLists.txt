cmake_minimum_required(VERSION 3.20)
project(gputxrx CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
endif()

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_library(gputxrx_core
    src/checksum.cpp
    src/cli.cpp
    src/payload.cpp
    src/storage.cpp
    src/backends/d3d12_backend.cpp
)
target_include_directories(gputxrx_core PUBLIC include)
target_link_libraries(gputxrx_core PUBLIC d3d12 dxgi dxguid)

add_executable(gputxrx src/main.cpp)
target_link_libraries(gputxrx PRIVATE gputxrx_core)

# Milestone 0 probe: cross-process GPU sharing capability check.
add_executable(test_gpu_roundtrip tests/test_gpu_roundtrip.cpp)
target_link_libraries(test_gpu_roundtrip PRIVATE gputxrx_core)

add_executable(test_checksums tests/test_checksums.cpp)
target_link_libraries(test_checksums PRIVATE gputxrx_core)

add_executable(test_payload tests/test_payload.cpp)
target_link_libraries(test_payload PRIVATE gputxrx_core)

add_executable(test_shared_resource tests/test_shared_resource.cpp)
target_link_libraries(test_shared_resource PRIVATE gputxrx_core)

add_executable(test_resource_lifetime tests/test_resource_lifetime.cpp)
target_link_libraries(test_resource_lifetime PRIVATE gputxrx_core)
