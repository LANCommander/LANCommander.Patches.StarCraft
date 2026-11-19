cmake_minimum_required(VERSION 3.25)

# The LANCommander Interposer is built into the patch (see below). Its dependencies (MinHook, yaml-cpp) come from
# vcpkg, through this project's vcpkg.json and the vcpkg toolchain, with the Interposer's own triplets (static CRT,
# v143) so the libraries match how the Interposer itself is built.
set(SCPATCH_INTERPOSER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../LANCommander.Interposer" CACHE PATH
    "Checkout of LANCommander.Interposer, embedded into the patch")
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  set(VCPKG_TARGET_TRIPLET x86-windows-static CACHE STRING "")
endif()
if(NOT DEFINED VCPKG_OVERLAY_TRIPLETS)
  set(VCPKG_OVERLAY_TRIPLETS "${SCPATCH_INTERPOSER_DIR}/triplets" CACHE STRING "")
endif()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
endif()

project(scpatch CXX)

# StarCraft 1.16.1 is a 32-bit process; the patch must be built for x86.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configure with -A Win32 (StarCraft is a 32-bit process)")
endif()

# The version the patch writes to its log; CI passes the tag (v1.2.3) or the commit.
set(SCPATCH_VERSION "dev" CACHE STRING "Version written to scpatch.log, e.g. v1.2.3")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- LANCommander Interposer, embedded ------------------------------------------------------------------------
# The patch carries the Interposer's static library (LANCommander::Interposer::Embedded) and attaches it from
# DllMain, so version.dll is the only file to ship. Every Interposer feature is opt-in: list the ones to build in.
# A feature left out keeps its exports but behaves as switched off, whatever .interposer\Config.yml says, and
# because this DLL loads first it is off for the whole process (a second Interposer defers to this one).
#   MATCHES        match statistics: required for scpatch's match reporting (src/features/matches.cpp)
#   REGISTRY       virtual registry (CD key, player name values set by LANCommander)
#   IDENTITY       user/computer name overrides
#   PLUGINS        .interposer\Plugins\ (e.g. the CDKey plugin)
# Also available: FILES FASTDL DIRECTINPUT AUDIO OSVERSION NETWORK RELAY RICH_PRESENCE VOICE. FILES, NETWORK and
# RELAY hook the same file and socket APIs that save_paths.cpp and network.cpp wrap; they chain, but they are
# left out until tested together in a LAN game. VOICE also needs opus, enet and miniaudio in vcpkg.json.
set(SCPATCH_INTERPOSER_FEATURES "MATCHES;REGISTRY;IDENTITY;PLUGINS" CACHE STRING
    "Interposer features built into the patch (INTERPOSER_FEATURE_<NAME> without the prefix)")
set(_scpatch_all_interposer_features
    REGISTRY FILES FASTDL DIRECTINPUT AUDIO OSVERSION IDENTITY NETWORK RELAY RICH_PRESENCE MATCHES VOICE PLUGINS)
foreach(_feature IN LISTS SCPATCH_INTERPOSER_FEATURES)
  if(NOT _feature IN_LIST _scpatch_all_interposer_features)
    message(FATAL_ERROR "SCPATCH_INTERPOSER_FEATURES: unknown feature '${_feature}'. "
                        "Known: ${_scpatch_all_interposer_features}")
  endif()
endforeach()
foreach(_feature IN LISTS _scpatch_all_interposer_features)
  if(_feature IN_LIST SCPATCH_INTERPOSER_FEATURES)
    set(INTERPOSER_FEATURE_${_feature} ON CACHE BOOL "" FORCE)
  else()
    set(INTERPOSER_FEATURE_${_feature} OFF CACHE BOOL "" FORCE)
  endif()
endforeach()
if(NOT "MATCHES" IN_LIST SCPATCH_INTERPOSER_FEATURES)
  message(STATUS "scpatch: Interposer built without MATCHES; match reporting will log that it is unavailable")
endif()
set(INTERPOSER_EMBEDDED_ONLY ON CACHE BOOL "" FORCE)
if(NOT EXISTS "${SCPATCH_INTERPOSER_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "LANCommander.Interposer not found at ${SCPATCH_INTERPOSER_DIR}; set SCPATCH_INTERPOSER_DIR")
endif()
add_subdirectory("${SCPATCH_INTERPOSER_DIR}" interposer EXCLUDE_FROM_ALL)

# Shaders are compiled to bytecode headers at build time.
find_program(FXC fxc REQUIRED
  HINTS "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/render/present.hlsl)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
add_custom_command(
  OUTPUT ${SHADER_OUT}/present_vs.h ${SHADER_OUT}/present_ps.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
  COMMAND ${FXC} /nologo /T vs_4_0 /E VSMain /O3 /Vn g_VSMain /Fh ${SHADER_OUT}/present_vs.h ${SHADER_SRC}
  COMMAND ${FXC} /nologo /T ps_4_0 /E PSMain /O3 /Vn g_PSMain /Fh ${SHADER_OUT}/present_ps.h ${SHADER_SRC}
  DEPENDS ${SHADER_SRC}
  VERBATIM)

add_library(scpatch SHARED
  src/loader/version_proxy.cpp
  src/loader/version.def
  src/core/main.cpp
  src/core/log.cpp
  src/core/config.cpp
  src/core/patch.cpp
  src/core/paths.cpp
  src/features/features.cpp
  src/features/save_paths.cpp
  src/features/renderer.cpp
  src/features/network.cpp
  src/features/frame_pacing.cpp
  src/features/widescreen.cpp
  src/features/zoom.cpp
  src/features/selection.cpp
  src/features/rally.cpp
  src/features/controls.cpp
  src/features/rules.cpp
  src/features/matches.cpp
  src/core/patch_set.cpp
  src/core/crash.cpp
  src/render/ddraw_emu.cpp
  src/render/presenter.cpp
  ${SHADER_OUT}/present_vs.h
  ${SHADER_OUT}/present_ps.h
)

target_include_directories(scpatch PRIVATE src ${SHADER_OUT})
target_compile_definitions(scpatch PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
  SCPATCH_VERSION="${SCPATCH_VERSION}")
target_compile_options(scpatch PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(scpatch PRIVATE shell32 ole32 d3d11 dxgi dxguid ws2_32 iphlpapi
  LANCommander::Interposer::Embedded)
# Output is named version.dll so Windows loads it from the game folder in place of the system DLL.
set_target_properties(scpatch PROPERTIES OUTPUT_NAME version)
