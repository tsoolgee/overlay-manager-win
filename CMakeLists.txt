cmake_minimum_required(VERSION 3.21)
project(OverlayManager VERSION 3.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "This project builds with MSVC: WebView2 ships as a NuGet package with MSVC-format import libraries.")
endif()

# Static CRT, so the executable runs on a clean machine with no redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(OverlayManager WIN32
  src/main.cpp
  src/app.cpp
  src/config.cpp
  src/json.cpp
  src/manager.cpp
  src/overlay.cpp
  src/updater.cpp
  src/util.cpp
  res/app.rc
)

# One version number, from project() above: the updater compares it against
# the latest release tag, and the .rc stamps it into the file properties.
configure_file(res/version.h.in "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)
target_include_directories(OverlayManager PRIVATE "${CMAKE_BINARY_DIR}/generated")

target_compile_definitions(OverlayManager PRIVATE
  UNICODE _UNICODE
  NOMINMAX                 # so std::min / std::max are usable
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS
)

target_compile_options(OverlayManager PRIVATE
  /W4
  /utf-8                   # the sources contain Hebrew string literals
  /permissive-
  $<$<CONFIG:Release>:/O2>
  $<$<CONFIG:Release>:/GL>
)

target_link_options(OverlayManager PRIVATE
  # res/app.rc embeds the manifest itself; without this the linker adds a second
  # one and the resource compiler rejects the duplicate.
  /MANIFEST:NO
  $<$<CONFIG:Release>:/LTCG>
  $<$<CONFIG:Release>:/OPT:REF>
  $<$<CONFIG:Release>:/OPT:ICF>
)

# --- WebView2 --------------------------------------------------------------
# Point WEBVIEW2_DIR at an extracted Microsoft.Web.WebView2 NuGet package.
# The CI workflow restores it with `nuget install`; see .github/workflows/build.yml.
set(WEBVIEW2_DIR "" CACHE PATH "Path to the Microsoft.Web.WebView2 NuGet package")

if(NOT WEBVIEW2_DIR)
  file(GLOB _wv2_candidates "${CMAKE_SOURCE_DIR}/packages/Microsoft.Web.WebView2*")
  if(_wv2_candidates)
    list(GET _wv2_candidates 0 WEBVIEW2_DIR)
  endif()
endif()

if(NOT WEBVIEW2_DIR OR NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
  message(FATAL_ERROR
    "WebView2 SDK not found.\n"
    "Run:  nuget install Microsoft.Web.WebView2 -OutputDirectory packages\n"
    "or configure with -DWEBVIEW2_DIR=<path to the package>")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_wv2_arch x64)
else()
  set(_wv2_arch x86)
endif()

target_include_directories(OverlayManager PRIVATE
  "${WEBVIEW2_DIR}/build/native/include"
  src
)

# The static loader keeps this a single file: no WebView2Loader.dll to ship.
target_link_libraries(OverlayManager PRIVATE
  "${WEBVIEW2_DIR}/build/native/${_wv2_arch}/WebView2LoaderStatic.lib"
  gdiplus windowscodecs winhttp bcrypt comctl32 comdlg32 shell32 advapi32 user32 gdi32 ole32 shlwapi version
)

set_target_properties(OverlayManager PROPERTIES OUTPUT_NAME "OverlayManager")
