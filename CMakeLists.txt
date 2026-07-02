cmake_minimum_required(VERSION 3.20)
project(SA3IPlug2DemoRepo VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IPLUG2_DIR "C:/dev/gary-in-the-plug/vendor/iPlug2" CACHE PATH "iPlug2 root directory")
set(SA3_CPP_DIR "C:/dev/sa3.cpp" CACHE PATH "sa3.cpp repository root")
set(SA3_BUILD_DIR "${SA3_CPP_DIR}/build" CACHE PATH "sa3.cpp build directory containing sa3.lib/sa3.dll")
set(IPLUG_DEPLOY_PLUGINS OFF CACHE BOOL "Deploy built plugins to system directories" FORCE)

include("${IPLUG2_DIR}/iPlug2.cmake")
find_package(iPlug2 REQUIRED)

add_library(sa3_shared_import SHARED IMPORTED GLOBAL)
set_target_properties(sa3_shared_import PROPERTIES
  IMPORTED_IMPLIB "${SA3_BUILD_DIR}/Release/sa3.lib"
  IMPORTED_LOCATION "${SA3_BUILD_DIR}/bin/Release/sa3.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${SA3_CPP_DIR}/src"
)

file(GLOB SA3_RUNTIME_DLLS
  "${SA3_BUILD_DIR}/bin/Release/sa3.dll"
  "${SA3_BUILD_DIR}/bin/Release/ggml*.dll"
)

add_subdirectory(SA3IPlug2Demo)
