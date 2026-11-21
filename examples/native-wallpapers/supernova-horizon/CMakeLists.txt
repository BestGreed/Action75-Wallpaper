cmake_minimum_required(VERSION 3.24)
project(SupernovaHorizon LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build this Action75 package with MSVC for Windows x64.")
endif()

find_program(FXC fxc HINTS
  "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
  REQUIRED)

add_library(supernova-horizon SHARED source/wallpaper.cpp)
target_compile_features(supernova-horizon PRIVATE cxx_std_20)
target_compile_definitions(supernova-horizon PRIVATE
  ACTION75_WALLPAPER12_EXPORTS NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(supernova-horizon PRIVATE sdk)
target_compile_options(supernova-horizon PRIVATE /W4 /utf-8)
target_link_libraries(supernova-horizon PRIVATE d3d12 runtimeobject)
set_property(TARGET supernova-horizon PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(add_shader file entry profile)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${entry}.cso")
  add_custom_command(OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${FXC}" /nologo /O3 /Ges /WX /T "${profile}" /E "${entry}"
            /Fo "${output}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${file}"
    DEPENDS "shaders/${file}" "shaders/common.hlsli" VERBATIM)
  set_property(GLOBAL APPEND PROPERTY shader_outputs "${output}")
endfunction()

add_shader(sky.hlsl ScreenVS vs_5_1)
add_shader(sky.hlsl SkyPS ps_5_1)
add_shader(sky.hlsl CopyPS ps_5_1)
add_shader(supernova.hlsl NovaVS vs_5_1)
add_shader(supernova.hlsl NovaPS ps_5_1)
add_shader(supernova.hlsl SparkVS vs_5_1)
add_shader(supernova.hlsl SparkPS ps_5_1)
get_property(shader_outputs GLOBAL PROPERTY shader_outputs)
add_custom_target(supernova-shaders DEPENDS ${shader_outputs})
add_dependencies(supernova-horizon supernova-shaders)

install(TARGETS supernova-horizon RUNTIME DESTINATION .)
install(FILES ${shader_outputs} DESTINATION shaders)
install(FILES metadata/manifest.json metadata/settings.json metadata/cover.jpg DESTINATION .)
