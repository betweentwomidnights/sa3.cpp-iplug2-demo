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
  ${CMAKE_SOURCE_DIR}/Shared/SA3RenderService.cpp
  ${CMAKE_SOURCE_DIR}/Shared/SA3RenderService.h
  ${CMAKE_SOURCE_DIR}/SA3IPlug2Demo/DemoAudioFileStore.cpp
  ${CMAKE_SOURCE_DIR}/SA3IPlug2Demo/DemoAudioFileStore.h
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
    ${CMAKE_SOURCE_DIR}/SA3IPlug2Demo
    ${IPLUG2_DIR}/Examples/IPlugReaperExtension/resources
  LINK
    ${IGRAPHICS_LIB}
    sa3_headers
    sa3_ui_theme
)

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE shell32 ole32)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
  SA3_DEMO_DEFAULT_MODELS_DIR="${SA3_CPP_DIR}/models"
  SA3_DEMO_SA3_CPP_DIR="${SA3_CPP_DIR}"
)

iplug_configure_reaperext(${PROJECT_NAME} ${PROJECT_NAME})

if(SA3_RUNTIME_LIBS)
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SA3ReaperExtension"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SA3_RUNTIME_LIBS} "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SA3ReaperExtension"
    COMMENT "Copying sa3/ggml runtime libraries for ${PROJECT_NAME}"
  )
endif()
