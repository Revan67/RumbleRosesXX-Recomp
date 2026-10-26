cmake_minimum_required(VERSION 3.20)
project(RRXXRecomp LANGUAGES CXX)

# Generated recompilation units are exceptionally large. Keep Ninja from
# launching enough clang-cl instances to exhaust RAM during clean rebuilds.
set_property(GLOBAL PROPERTY JOB_POOLS rrxx_compile_pool=2 rrxx_link_pool=1)
set(CMAKE_JOB_POOL_COMPILE rrxx_compile_pool)
set(CMAKE_JOB_POOL_LINK rrxx_link_pool)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB RRXX_RECOMP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/ppc/ppc_recomp.*.cpp")

add_library(rrxx_recompiled_obj OBJECT ${RRXX_RECOMP_SOURCES})
target_include_directories(rrxx_recompiled_obj PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/ppc"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde")

if(MSVC)
    target_compile_options(rrxx_recompiled_obj PRIVATE /W0 /bigobj)
endif()

set(RRXX_IMPORT_STUBS "${CMAKE_CURRENT_BINARY_DIR}/generated_import_stubs.cpp")
add_custom_command(
    OUTPUT "${RRXX_IMPORT_STUBS}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/runtime/generate_import_stubs.ps1"
        -Header "${CMAKE_CURRENT_SOURCE_DIR}/generated/ppc/ppc_recomp_shared.h"
        -Output "${RRXX_IMPORT_STUBS}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/generate_import_stubs.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/ppc/ppc_recomp_shared.h"
    VERBATIM)

add_executable(rrxx_host
    runtime/main.cpp
    runtime/compiler_compat.cpp
    runtime/kernel_imports.cpp
    runtime/filesystem_imports.cpp
    runtime/platform_imports.cpp
    runtime/host_window.cpp
    generated/ppc/ppc_func_mapping.cpp
    "${RRXX_IMPORT_STUBS}"
    $<TARGET_OBJECTS:rrxx_recompiled_obj>)
target_include_directories(rrxx_host PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/ppc"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde")
target_compile_options(rrxx_host PRIVATE /W0 /bigobj)
target_link_options(rrxx_host PRIVATE /MAP:rrxx_host.map)
target_link_libraries(rrxx_host PRIVATE Xinput9_1_0)
