cmake_minimum_required(VERSION 3.24)
project(Action75FieldWallpapers LANGUAGES CXX)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(repository "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(ACTION75_SDK_INCLUDE "${repository}/skills/action75-wallpaper-author/sdk" CACHE PATH "Public Action75 ABI headers")
set(ACTION75_SDK_VENDOR "${repository}/vendor-source/windows-native" CACHE PATH "Directory containing nlohmann/json.hpp")
set(ACTION75_FIELD_PACKS "${repository}/wallpapers-native" CACHE PATH "Root of the four wallpaper resource folders")
if(NOT EXISTS "${ACTION75_SDK_INCLUDE}/Action75Wallpaper12.h" OR
   NOT EXISTS "${ACTION75_SDK_INCLUDE}/Action75Wallpaper.h" OR
   NOT EXISTS "${ACTION75_SDK_VENDOR}/nlohmann/json.hpp")
    message(FATAL_ERROR "Set ACTION75_SDK_INCLUDE to the public SDK and ACTION75_SDK_VENDOR to the JSON include directory")
endif()

cmake_host_system_information(RESULT windows_kits QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10 VIEW 32)
file(GLOB sdk_bins "${windows_kits}/bin/10.*/x64")
list(SORT sdk_bins COMPARE NATURAL ORDER DESCENDING)
find_program(ACTION75_FIELD12_FXC fxc HINTS
    "${windows_kits}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64" ${sdk_bins} REQUIRED)

add_library(action75_field_gpu12 STATIC src/Gpu12.cpp)
target_compile_features(action75_field_gpu12 PUBLIC cxx_std_17)
target_compile_definitions(action75_field_gpu12 PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(action75_field_gpu12 PUBLIC "${ACTION75_SDK_INCLUDE}" "${ACTION75_SDK_VENDOR}")
target_link_libraries(action75_field_gpu12 PUBLIC d3d12)

function(field12_shader target source entry symbol profile)
    set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    set(shader "${shader_dir}/${source}.hlsl")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${target}/${symbol}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${ACTION75_FIELD12_FXC}" /nologo /O3 /T ${profile} /E ${entry} ${ARGN}
            /Vn ${symbol} /Fh "${header}" "${shader}"
        DEPENDS "${shader}" "${shader_dir}/ProceduralBackground.hlsli" "${shader_dir}/LayerColor.hlsli"
        VERBATIM)
    target_sources(${target} PRIVATE "${header}")
endfunction()

function(add_field12_pack pack scene)
    set(target "wallpaper_${pack}")
    add_library(${target} MODULE src/Wallpaper.cpp "src/${scene}.cpp")
    target_compile_definitions(${target} PRIVATE ACTION75_WALLPAPER12_EXPORTS)
    target_compile_options(${target} PRIVATE /utf-8)
    target_link_libraries(${target} PRIVATE action75_field_gpu12)
    set(shader_output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${target}")
    file(MAKE_DIRECTORY "${shader_output}")
    target_include_directories(${target} PRIVATE "${shader_output}")
    if(scene STREQUAL "FieldScene")
        target_compile_definitions(${target} PRIVATE FIELD_THEME=${ARGV2})
        field12_shader(${target} Field vs FieldVertex vs_5_0 /D FIELD_THEME=${ARGV2})
        foreach(entry IN ITEMS sim shade composite blurHorizontal blurVertical)
            field12_shader(${target} Field ${entry} ${entry}Shader ps_5_0 /D FIELD_THEME=${ARGV2})
        endforeach()
        foreach(entry IN ITEMS shade composite)
            field12_shader(${target} Field ${entry} graded_${entry}Shader ps_5_0 /D FIELD_THEME=${ARGV2} /D GRADE_BACKGROUND=1)
        endforeach()
    else()
        field12_shader(${target} Keycap vs KeycapVertex vs_5_0)
        field12_shader(${target} Keycap ps KeycapPixel ps_5_0)
        field12_shader(${target} Keycap ps KeycapGraded ps_5_0 /D GRADE_BACKGROUND=1)
    endif()
    set_target_properties(${target} PROPERTIES OUTPUT_NAME wallpaper
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/native-wallpapers/${pack}>"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/native-wallpapers/${pack}>"
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/native-wallpapers/${pack}>")
    install(TARGETS ${target} RUNTIME DESTINATION "${pack}" LIBRARY DESTINATION "${pack}")
    install(FILES "${ACTION75_FIELD_PACKS}/${pack}/manifest.json" "${ACTION75_FIELD_PACKS}/${pack}/settings.json"
        "${ACTION75_FIELD_PACKS}/${pack}/layout.json" "${ACTION75_FIELD_PACKS}/${pack}/preview.png"
        DESTINATION "${pack}")
endfunction()

add_field12_pack(silk-nebula FieldScene 0)
add_field12_pack(gilded-synapse FieldScene 1)
add_field12_pack(harmonic-textile FieldScene 3)
add_field12_pack(keycap-glow KeycapScene)

option(ACTION75_FIELD_COMPARE "Build the old/new offscreen pixel comparator" OFF)
if(ACTION75_FIELD_COMPARE)
    add_executable(action75_field_compare tests/Compare.cpp)
    target_include_directories(action75_field_compare PRIVATE src)
    target_link_libraries(action75_field_compare PRIVATE action75_field_gpu12 d3d11 dxgi)
endif()
