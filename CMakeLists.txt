cmake_minimum_required(VERSION 3.28...3.31)

#
# Cord - OBS Studio plugin.
#
# Run scripts/fetch-deps.ps1 once, then:
#   cmake --preset windows-x64
#   cmake --build --preset windows-x64
#
# OBS is deliberately NOT built from source. The script fetches libobs headers
# from the matching obs-studio tag, synthesises the one generated header
# (obsconfig.h), and produces obs.lib directly from the installed obs.dll - so
# the plugin links against exactly the OBS it will run in. See docs/building.md.
#
project(cord VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR
    "Cord is Windows-only: it uses WASAPI for the microphone send and ships "
    "a Windows adb. The capture path itself is portable, so a Linux or macOS "
    "port means replacing src/micsend.")
endif()

set(CORD_DEPS_ROOT "${CMAKE_SOURCE_DIR}/deps" CACHE PATH
  "Where fetch-deps.ps1 put the dependencies")

# --- libobs ----------------------------------------------------------------

set(CORD_OBS_HEADERS "${CORD_DEPS_ROOT}/obs-headers")
set(CORD_OBS_LIB "${CORD_DEPS_ROOT}/lib/obs.lib")
# libobs's pthread functions are in a separate DLL on Windows (the w32-pthreads
# shim), not in obs.dll, so they need their own import library.
set(CORD_PTHREAD_LIB "${CORD_DEPS_ROOT}/lib/w32-pthreads.lib")

if(NOT EXISTS "${CORD_OBS_HEADERS}/libobs/obs-module.h")
  message(FATAL_ERROR
    "libobs headers not found at ${CORD_OBS_HEADERS}.\n"
    "Run: powershell -ExecutionPolicy Bypass -File scripts/fetch-deps.ps1")
endif()

if(NOT EXISTS "${CORD_OBS_LIB}")
  message(FATAL_ERROR
    "obs.lib not found at ${CORD_OBS_LIB}.\n"
    "Run: powershell -ExecutionPolicy Bypass -File scripts/fetch-deps.ps1")
endif()

add_library(obs::libobs STATIC IMPORTED)
set_target_properties(obs::libobs PROPERTIES
  IMPORTED_LOCATION "${CORD_OBS_LIB}"
  # Two include roots are needed:
  #  - libobs/ itself, since headers are used as <obs-module.h> and
  #    <util/platform.h>
  #  - w32-pthreads/, because libobs/util/threading.h does #include <pthread.h>
  #    and on Windows that is OBS's bundled shim, not a system header
  INTERFACE_INCLUDE_DIRECTORIES
    "${CORD_OBS_HEADERS}/libobs;${CORD_OBS_HEADERS}/w32-pthreads")

add_library(obs::w32pthreads STATIC IMPORTED)
set_target_properties(obs::w32pthreads PROPERTIES
  IMPORTED_LOCATION "${CORD_PTHREAD_LIB}")

# --- FFmpeg (from obs-deps) ------------------------------------------------

set(CORD_FFMPEG_ROOT "${CORD_DEPS_ROOT}/obs-deps")

if(NOT EXISTS "${CORD_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
  message(FATAL_ERROR
    "FFmpeg headers not found under ${CORD_FFMPEG_ROOT}.\n"
    "Run: powershell -ExecutionPolicy Bypass -File scripts/fetch-deps.ps1")
endif()

foreach(lib avcodec avutil)
  add_library(ffmpeg::${lib} STATIC IMPORTED)
  set_target_properties(ffmpeg::${lib} PROPERTIES
    IMPORTED_LOCATION "${CORD_FFMPEG_ROOT}/lib/${lib}.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CORD_FFMPEG_ROOT}/include")
endforeach()

# --- target ----------------------------------------------------------------

add_library(cord MODULE)

target_sources(cord PRIVATE
  src/plugin-main.c
  src/device/adb-client.c
  src/device/device-cache.c
  src/device/scrcpy-proto.c
  src/device/scrcpy-session.c
  src/video/decoder.c
  src/audio/audio-decode.c
  src/audio/wasapi-player.cpp
  src/source/phone-source.c
  src/micsend/micsend-control.c
  src/micsend/wasapi-bridge.cpp)

target_include_directories(cord PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_BINARY_DIR}/generated")

target_link_libraries(cord PRIVATE
  obs::libobs
  obs::w32pthreads
  ffmpeg::avcodec
  ffmpeg::avutil
  ws2_32    # the adb client speaks the socket protocol directly
  avrt      # Pro Audio thread priority for the mic send
  ole32     # WASAPI device enumeration
  uuid)     # the IID_/CLSID_ constants WASAPI needs

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/plugin-support.h.in"
  "${CMAKE_BINARY_DIR}/generated/plugin-support.h"
  @ONLY)

target_compile_definitions(cord PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN  # trim windows.h; it collides with libobs otherwise
  NOMINMAX)

if(MSVC)
  target_compile_options(cord PRIVATE /W4 /utf-8)
  # libobs and FFmpeg headers are not /WX-clean, so warnings stay non-fatal.
endif()

set_target_properties(cord PROPERTIES PREFIX "" OUTPUT_NAME "cord")

# --- install ---------------------------------------------------------------
#
# OBS's native plugin layout, relative to the OBS install directory:
#
#   obs-plugins/64bit/cord.dll
#   data/obs-plugins/cord/locale/en-US.ini
#   data/obs-plugins/cord/bin/{adb.exe,scrcpy-server,...}
#
# That second path is what obs_module_file() resolves against, which is how
# the plugin finds the bundled adb and the scrcpy server at runtime.
#
# Install with the OBS directory as the prefix:
#   cmake --install build --prefix "D:/path/to/OBS Studio"
#
install(TARGETS cord
  RUNTIME DESTINATION "obs-plugins/64bit"
  LIBRARY DESTINATION "obs-plugins/64bit")

install(DIRECTORY data/locale
  DESTINATION "data/obs-plugins/cord")

# Fetched by scripts/fetch-deps.ps1, not in git; OPTIONAL so a bare clone
# still installs.
install(DIRECTORY data/bin/
  DESTINATION "data/obs-plugins/cord/bin"
  OPTIONAL)
