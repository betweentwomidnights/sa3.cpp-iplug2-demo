cmake_minimum_required(VERSION 3.20)
project(SA3IPlug2DemoRepo VERSION 0.4.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/iPlug2" CACHE PATH "iPlug2 root directory")
# defaults to a sibling checkout (clone sa3.cpp next to this repo); override with -DSA3_CPP_DIR=...
get_filename_component(_SA3_SIBLING "${CMAKE_CURRENT_SOURCE_DIR}/../sa3.cpp" ABSOLUTE)
set(SA3_CPP_DIR "${_SA3_SIBLING}" CACHE PATH "sa3.cpp repository root")
set(_SA3_DEFAULT_BUILD_DIR "${SA3_CPP_DIR}/build")
# ggml's libraries may carry a prefix (sa3.cpp's SA3_PRIVATE_GGML: sa3-cuda-<commit>-ggml-cuda.dll).
file(GLOB _SA3_DEFAULT_CUDA_DLL "${SA3_CPP_DIR}/build-cuda/bin/Release/*ggml-cuda.dll")
if(EXISTS "${SA3_CPP_DIR}/build-cuda/Release/sa3.lib" AND _SA3_DEFAULT_CUDA_DLL)
  set(_SA3_DEFAULT_BUILD_DIR "${SA3_CPP_DIR}/build-cuda")
endif()
set(SA3_BUILD_DIR "${_SA3_DEFAULT_BUILD_DIR}" CACHE PATH "sa3.cpp build directory containing sa3.lib/sa3.dll")
if(DEFINED ENV{CUDA_PATH})
  file(TO_CMAKE_PATH "$ENV{CUDA_PATH}" _SA3_CUDA_PATH)
else()
  set(_SA3_CUDA_PATH "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8")
endif()
set(SA3_CUDA_RUNTIME_DIR "${_SA3_CUDA_PATH}/bin" CACHE PATH "Optional CUDA runtime DLL directory")
set(IPLUG_DEPLOY_PLUGINS OFF CACHE BOOL "Deploy built plugins to system directories" FORCE)

include("${IPLUG2_DIR}/iPlug2.cmake")
find_package(iPlug2 REQUIRED)

add_library(sa3_headers INTERFACE)
target_include_directories(sa3_headers INTERFACE "${SA3_CPP_DIR}/src")

add_library(sa3_ui_theme INTERFACE)
target_include_directories(sa3_ui_theme INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Shared")
target_sources(sa3_ui_theme INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Shared/SA3UITheme.h")

# The plugin and the REAPER extension run inside a DAW, where Windows binds an import of ggml.dll
# to any ggml.dll already loaded: an older release of this demo, or another vendor's ggml plugin,
# would hand sa3.dll its ggml. Release builds therefore use a sa3.cpp build configured with
# -DSA3_PRIVATE_GGML=ON, whose ggml libraries are named after the backend and ggml commit
# (sa3-vulkan-ca7bcb6f-ggml*.dll), and configure this repo with -DSA3_REQUIRE_PRIVATE_GGML=ON.
option(SA3_REQUIRE_PRIVATE_GGML "Stop if the sa3.cpp build names its ggml libraries plain ggml* (release builds)" OFF)
file(GLOB SA3_RUNTIME_DLLS
  "${SA3_BUILD_DIR}/bin/Release/sa3.dll"
  "${SA3_BUILD_DIR}/bin/Release/*ggml*.dll"
)
if(EXISTS "${SA3_BUILD_DIR}/bin/Release/ggml.dll" OR EXISTS "${SA3_BUILD_DIR}/ggml/src/libggml.dylib")
  set(_SA3_PLAIN_GGML_MESSAGE
    "${SA3_BUILD_DIR} names its ggml libraries plain ggml*, which can bind to another plugin's ggml in "
    "the same DAW. Reconfigure that sa3.cpp build with -DSA3_PRIVATE_GGML=ON for anything you ship.")
  if(SA3_REQUIRE_PRIVATE_GGML)
    message(FATAL_ERROR ${_SA3_PLAIN_GGML_MESSAGE})
  else()
    message(WARNING ${_SA3_PLAIN_GGML_MESSAGE})
  endif()
endif()
if(APPLE)
  file(GLOB_RECURSE SA3_RUNTIME_DYLIBS
    "${SA3_BUILD_DIR}/libsa3*.dylib"
    "${SA3_BUILD_DIR}/ggml/src/lib*ggml*.dylib"
    "${SA3_BUILD_DIR}/ggml/src/ggml-*/*.dylib"
  )
endif()
# Only bundle the CUDA runtime when the selected sa3 build actually uses the CUDA backend
# (a Vulkan / Metal / CPU build ships no cudart/cublas). Detected by the presence of ggml-cuda.dll,
# with or without the SA3_PRIVATE_GGML prefix.
file(GLOB _SA3_CUDA_DLL "${SA3_BUILD_DIR}/bin/Release/*ggml-cuda.dll")
if(_SA3_CUDA_DLL)
  file(GLOB SA3_CUDA_RUNTIME_DLLS
    "${SA3_CUDA_RUNTIME_DIR}/cudart64_*.dll"
    "${SA3_CUDA_RUNTIME_DIR}/cublas64_*.dll"
    "${SA3_CUDA_RUNTIME_DIR}/cublasLt64_*.dll"
  )
  list(APPEND SA3_RUNTIME_DLLS ${SA3_CUDA_RUNTIME_DLLS})
endif()
set(SA3_RUNTIME_LIBS ${SA3_RUNTIME_DLLS} ${SA3_RUNTIME_DYLIBS})
list(REMOVE_DUPLICATES SA3_RUNTIME_LIBS)

add_subdirectory(SA3IPlug2Demo)
add_subdirectory(SA3ReaperExtension)
