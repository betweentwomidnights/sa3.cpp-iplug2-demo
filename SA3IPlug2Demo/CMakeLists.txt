cmake_minimum_required(VERSION 3.20)
project(SA3IPlug2Demo VERSION 0.1.0)

set(SA3_DEMO_PLATFORM_LIBS)
if(WIN32)
  list(APPEND SA3_DEMO_PLATFORM_LIBS shell32 ole32)
  set_source_files_properties(resources/main.rc PROPERTIES
    COMPILE_FLAGS "/I\"${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/resources\""
  )
endif()

iplug_add_plugin(${PROJECT_NAME}
  SOURCES
    SA3IPlug2Demo.cpp
    SA3IPlug2Demo.h
    DemoAudioFileStore.cpp
    DemoAudioFileStore.h
    DemoUIPrimitives.h
    config.h
    resources/main.rc
    resources/resource.h
  RESOURCES
    resources/fonts/Roboto-Regular.ttf
  LINK
    sa3_shared_import
    ${SA3_DEMO_PLATFORM_LIBS}
  DEFINES
    SA3_DEMO_DEFAULT_MODELS_DIR="${SA3_CPP_DIR}/models"
  FORMATS APP VST3
)

function(sa3_demo_copy_runtime target_name)
  if(TARGET ${target_name} AND SA3_RUNTIME_DLLS)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SA3_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target_name}>"
      COMMENT "Copying sa3/ggml runtime DLLs for ${target_name}"
    )
  endif()
endfunction()

sa3_demo_copy_runtime(${PROJECT_NAME}-app)
sa3_demo_copy_runtime(${PROJECT_NAME}-vst3)
