cmake_minimum_required(VERSION 3.25)
project(RRXXRecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set_property(GLOBAL PROPERTY JOB_POOLS compile_pool=2 link_pool=1)
set(CMAKE_JOB_POOL_COMPILE compile_pool)
set(CMAKE_JOB_POOL_LINK link_pool)

get_filename_component(RRXX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(REXSDK_SOURCE "${RRXX_ROOT}/build/rexglue-sdk" CACHE PATH "ReXGlue SDK source directory")
set(REXSDK_LIB "${REXSDK_SOURCE}/out/win-amd64" CACHE PATH "Built ReXGlue libraries")
set(REXGPU_SOURCE "${RRXX_ROOT}/build/rexglue-d3d12" CACHE PATH "Xenos D3D12 plugin source directory")
set(RRXX_PPC_DIR "${RRXX_ROOT}/generated/ppc" CACHE PATH "Locally generated PPC source directory")
option(RRXX_SDK_GUEST_CLOCK "Use the SDK guest clock for generated timebase reads" OFF)

file(GLOB PPC_SOURCES CONFIGURE_DEPENDS "${RRXX_PPC_DIR}/ppc_recomp.*.cpp")
if(NOT PPC_SOURCES OR NOT EXISTS "${RRXX_PPC_DIR}/ppc_func_mapping.cpp")
  message(FATAL_ERROR "Generate PPC sources locally from your own game copy; nothing generated is distributed here")
endif()
if(NOT EXISTS "${REXSDK_LIB}/rexruntime.lib")
  message(FATAL_ERROR "Build ReXGlue SDK first and set REXSDK_SOURCE/REXSDK_LIB")
endif()

foreach(source IN LISTS PPC_SOURCES)
  set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS
    "/FI${CMAKE_CURRENT_SOURCE_DIR}/generated_memory.h")
  if(RRXX_SDK_GUEST_CLOCK)
    file(STRINGS "${source}" timebase_reads REGEX "__rdtsc")
    if(timebase_reads)
      set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/guest_clock.h")
    endif()
  endif()
endforeach()

set(RRXX_TRANSLATED_SOURCES ${PPC_SOURCES}
  "${RRXX_PPC_DIR}/ppc_func_mapping.cpp" crt_memory.cpp
  "${RRXX_ROOT}/runtime/compiler_compat.cpp")

# Boundary implementations are generated locally and never committed.
function(rrxx_add_local_boundary cache_name symbol output_name)
  set(input "${${cache_name}}")
  if(NOT input)
    return()
  endif()
  file(READ "${input}" unit)
  string(REPLACE "\r\n" "\n" unit "${unit}")
  string(FIND "${unit}" "PPC_FUNC_IMPL(__imp__${symbol}) {" start)
  if(start LESS 0)
    message(FATAL_ERROR "${cache_name} does not contain ${symbol}")
  endif()
  string(SUBSTRING "${unit}" ${start} -1 tail)
  string(FIND "${tail}" "\n}\n" finish)
  if(finish LESS 0)
    message(FATAL_ERROR "${cache_name} has no complete ${symbol} body")
  endif()
  math(EXPR length "${finish} + 3")
  string(SUBSTRING "${tail}" 0 ${length} body)
  string(REPLACE "PPC_FUNC_IMPL(__imp__${symbol})" "PPC_FUNC_IMPL(${symbol})" body "${body}")
  set(output "${CMAKE_CURRENT_BINARY_DIR}/${output_name}")
  file(GENERATE OUTPUT "${output}" CONTENT
    "// Locally generated overlay. Do not distribute.\n#include \"ppc_recomp_shared.h\"\n${body}")
  list(APPEND RRXX_TRANSLATED_SOURCES "${output}")
  set_property(SOURCE "${output}" APPEND PROPERTY COMPILE_OPTIONS
    "/FI${CMAKE_CURRENT_SOURCE_DIR}/generated_memory.h")
  set(RRXX_TRANSLATED_SOURCES "${RRXX_TRANSLATED_SOURCES}" PARENT_SCOPE)
endfunction()

set(RRXX_FADE_BOUNDARY_INPUT "" CACHE FILEPATH "Local regenerated fade-boundary unit")
set(RRXX_NEW_GAME_BOUNDARY_INPUT "" CACHE FILEPATH "Local regenerated New Game boundary unit")
rrxx_add_local_boundary(RRXX_FADE_BOUNDARY_INPUT sub_82062200 local_fade_boundary.cpp)
rrxx_add_local_boundary(RRXX_NEW_GAME_BOUNDARY_INPUT sub_8214E948 local_new_game_boundary.cpp)

add_library(rrxx_translated OBJECT ${RRXX_TRANSLATED_SOURCES})
add_executable(rrxx_windowed WIN32 windowed_main.cpp
  "${REXSDK_SOURCE}/src/ui/rex_app.cpp"
  "${REXSDK_SOURCE}/src/ui/windowed_app_main_sdl.cpp"
  $<TARGET_OBJECTS:rrxx_translated>)

foreach(target rrxx_translated rrxx_windowed)
  target_compile_definitions(${target} PRIVATE NOMINMAX REX_PLATFORM_WINDOWS=1 SPDLOG_FMT_EXTERNAL)
  target_compile_options(${target} PRIVATE /W0 /bigobj "/FI${CMAKE_CURRENT_SOURCE_DIR}/context.h")
  target_include_directories(${target} PRIVATE "${RRXX_PPC_DIR}"
    "${REXSDK_SOURCE}/include" "${REXSDK_SOURCE}/thirdparty/simde"
    "${REXSDK_SOURCE}/thirdparty/simde/simde"
    "${REXSDK_SOURCE}/thirdparty/fmt/include"
    "${REXSDK_SOURCE}/thirdparty/spdlog/include")
endforeach()

target_link_libraries(rrxx_windowed PRIVATE "${REXSDK_LIB}/rexruntime.lib" "${REXSDK_LIB}/fmt.lib")
target_link_options(rrxx_windowed PRIVATE /MAP)
target_include_directories(rrxx_windowed PRIVATE
  "${REXSDK_SOURCE}/thirdparty/imgui" "${REXSDK_SOURCE}/thirdparty/sdl3/include"
  "${REXGPU_SOURCE}/include")
target_compile_definitions(rrxx_windowed PRIVATE REXGLUE_BUILD_CONFIG="${CMAKE_BUILD_TYPE}")
foreach(dll rexruntime rexgpu-xenos)
  add_custom_command(TARGET rrxx_windowed POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${REXSDK_LIB}/${dll}.dll" "$<TARGET_FILE_DIR:rrxx_windowed>")
endforeach()
