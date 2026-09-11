cmake_minimum_required(VERSION 3.20)
project(SA3ReaperExtension VERSION 0.1.0)

if(NOT WIN32 AND NOT APPLE)
  message(STATUS "SA3ReaperExtension is only supported on Windows and macOS. Skipping.")
  return()
endif()

include(${IPLUG2_CMAKE_DIR}/ReaperExt.cmake)

set(SA3_REAPER_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

set(SA3_REAPER_SOURCES
  SA3ReaperExtension.cpp
  SA3ReaperExtension.h
  config.h
  resources/resource.h
  ${IPLUG2_DIR}/Examples/IPlugReaperExtension/resources/roboto.cpp
)

if(WIN32)
  list(APPEND SA3_REAPER_SOURCES resources/main.rc)
  set_source_files_properties(resources/main.rc PROPERTIES
    COMPILE_FLAGS "/I\"${CMAKE_SOURCE_DIR}/SA3IPlug2Demo/resources/fonts\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/resources\""
  )
endif()

add_library(${PROJECT_NAME} MODULE ${SA3_REAPER_SOURCES})

iplug_add_target(${PROJECT_NAME} PUBLIC
  INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SA3_REAPER_RESOURCES_DIR}
    ${IPLUG2_DIR}/Examples/IPlugReaperExtension/resources
  LINK
    ${IGRAPHICS_LIB}
    sa3_ui_theme
)

iplug_configure_reaperext(${PROJECT_NAME} ${PROJECT_NAME})
