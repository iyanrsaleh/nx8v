cmake_minimum_required(VERSION 3.20)
project(nx8v CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# V8 monolith is built with the static CRT (MultiThreaded). Everything we
# link in (including libuv) must use the same runtime to avoid LNK2038.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# libuv (event loop) — fetched and built automatically.
# ---------------------------------------------------------------------------
include(FetchContent)
set(UV_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(UV_BUILD_BENCH   OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG        v1.48.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libuv)

# ---------------------------------------------------------------------------
# V8 paths
# Setelah build V8, set variabel berikut:
#   -DV8_DIR=C:/path/to/v8          (root repo V8)
#   -DV8_BUILD_DIR=C:/path/to/v8/out/x64.release
#
# Atau set environment variable V8_DIR dan V8_BUILD_DIR sebelum cmake.
# ---------------------------------------------------------------------------

if(NOT DEFINED V8_DIR)
  if(DEFINED ENV{V8_DIR})
    set(V8_DIR "$ENV{V8_DIR}")
  else()
    # Default: cari di subfolder deps/v8
    set(V8_DIR "${CMAKE_SOURCE_DIR}/deps/v8")
  endif()
endif()

if(NOT DEFINED V8_BUILD_DIR)
  if(DEFINED ENV{V8_BUILD_DIR})
    set(V8_BUILD_DIR "$ENV{V8_BUILD_DIR}")
  else()
    set(V8_BUILD_DIR "${V8_DIR}/out/x64.release")
  endif()
endif()

message(STATUS "V8_DIR      = ${V8_DIR}")
message(STATUS "V8_BUILD_DIR= ${V8_BUILD_DIR}")

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
set(V8_INCLUDE_DIRS
  "${V8_DIR}/include"
)

# ---------------------------------------------------------------------------
# V8 static libraries (MSVC x64 release build dari GN)
# ---------------------------------------------------------------------------
# Urutan link penting: v8_monolith sudah cukup kalau build dengan
# v8_monolithic_build=true. Kalau tidak, link semua komponen.
set(V8_MONOLITH "${V8_BUILD_DIR}/obj/v8_monolith.lib")

if(EXISTS "${V8_MONOLITH}")
  message(STATUS "Using monolithic V8 library: ${V8_MONOLITH}")
  set(V8_LIBRARIES "${V8_MONOLITH}")
else()
  # Fallback: link komponen terpisah
  message(STATUS "Monolith not found, linking V8 components separately")
  set(V8_LIBRARIES
    "${V8_BUILD_DIR}/obj/v8_base_without_compiler.lib"
    "${V8_BUILD_DIR}/obj/v8_compiler.lib"
    "${V8_BUILD_DIR}/obj/v8_snapshot.lib"
    "${V8_BUILD_DIR}/obj/v8_libplatform.lib"
    "${V8_BUILD_DIR}/obj/v8_libbase.lib"
    "${V8_BUILD_DIR}/obj/v8_libsampler.lib"
    "${V8_BUILD_DIR}/obj/torque_generated/v8_initializers.lib"
  )
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(nx8v
  src/main.cc
  # Headers listed here so they appear in IDE project trees
  src/runtime.h
  src/utils.h
  src/bindings/console.h
  src/bindings/process.h
  src/bindings/timers.h
  src/bindings/loop.h
  src/bindings/fs.h
  src/bindings/module.h
  src/bindings/repl.h
  src/bindings/events.h
  src/bindings/http.h
  src/bindings/os.h
  src/bindings/url.h
)

target_include_directories(nx8v PRIVATE
  ${V8_INCLUDE_DIRS}
  "${CMAKE_SOURCE_DIR}/src"   # so #include "runtime.h" works from bindings/
  # libuv include dir comes from the uv::uv_a target
)

target_link_libraries(nx8v PRIVATE
  ${V8_LIBRARIES}
  uv_a                    # static libuv (FetchContent target)
  # Windows system libs yang dibutuhkan V8
  winmm.lib
  dbghelp.lib
  shlwapi.lib
  ws2_32.lib              # libuv needs Winsock
  userenv.lib             # libuv may use GetUserProfileDirectory
  psapi.lib
  iphlpapi.lib
)

# MSVC-specific flags yang dibutuhkan V8
if(MSVC)
  target_compile_options(nx8v PRIVATE
    /utf-8           # source dan eksekusi dalam UTF-8
    /EHsc            # C++ exception handling
    /DUNICODE        # Unicode API
    /D_UNICODE
    # V8 menggunakan _ITERATOR_DEBUG_LEVEL=0 di release build
    $<$<CONFIG:Release>:/D_ITERATOR_DEBUG_LEVEL=0>
  )

  # V8 static lib dibangun tanpa _DLL (tidak pakai CRT DLL di monolith)
  # Gunakan MT (static CRT) agar cocok dengan V8 monolith
  set_property(TARGET nx8v PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

  target_link_options(nx8v PRIVATE
    /SUBSYSTEM:CONSOLE
    /STACK:67108864  # 64 MB stack (V8 butuh stack besar untuk JIT compilation)
  )
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS nx8v RUNTIME DESTINATION bin)
