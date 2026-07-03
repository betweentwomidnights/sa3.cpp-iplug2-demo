cmake_minimum_required(VERSION 3.20)
project(SA3IPlug2DemoRepo VERSION 0.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/iPlug2" CACHE PATH "iPlug2 root directory")
# defaults to a sibling checkout (clone sa3.cpp next to this repo); override with -DSA3_CPP_DIR=...
get_filename_component(_SA3_SIBLING "${CMAKE_CURRENT_SOURCE_DIR}/../sa3.cpp" ABSOLUTE)
set(SA3_CPP_DIR "${_SA3_SIBLING}" CACHE PATH "sa3.cpp repository root")
set(_SA3_DEFAULT_BUILD_DIR "${SA3_CPP_DIR}/build")
if(EXISTS "${SA3_CPP_DIR}/build-cuda/Release/sa3.lib" AND EXISTS "${SA3_CPP_DIR}/build-cuda/bin/Release/ggml-cuda.dll")
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

file(GLOB SA3_RUNTIME_DLLS
  "${SA3_BUILD_DIR}/bin/Release/sa3.dll"
  "${SA3_BUILD_DIR}/bin/Release/ggml*.dll"
)
file(GLOB SA3_CUDA_RUNTIME_DLLS
  "${SA3_CUDA_RUNTIME_DIR}/cudart64_*.dll"
  "${SA3_CUDA_RUNTIME_DIR}/cublas64_*.dll"
  "${SA3_CUDA_RUNTIME_DIR}/cublasLt64_*.dll"
)
list(APPEND SA3_RUNTIME_DLLS ${SA3_CUDA_RUNTIME_DLLS})
list(REMOVE_DUPLICATES SA3_RUNTIME_DLLS)

add_subdirectory(SA3IPlug2Demo)
