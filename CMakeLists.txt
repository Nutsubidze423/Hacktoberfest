cmake_minimum_required(VERSION 3.25)
project(kvm_demo LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "kvm_demo builds on Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT to match the x64-windows-static vcpkg triplet: exes run on A and B without redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)

# Source files are globbed per directory, so adding a new .cpp needs no CMakeLists edit
# (fewer merge conflicts between tracks). Re-run configure is automatic via CONFIGURE_DEPENDS.
add_subdirectory(common)
add_subdirectory(host_a)
add_subdirectory(client_b)
add_subdirectory(tools)
