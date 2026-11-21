cmake_minimum_required(VERSION 3.24)
project(Action75SurfaceWallpapers LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Action75 surface wallpapers require an x64 MSVC toolchain")
endif()

get_filename_component(surface_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(ACTION75_SURFACE_SDK "${surface_root}/skills/action75-wallpaper-author/sdk"
    CACHE PATH "Public Action75 wallpaper SDK headers")
set(ACTION75_SURFACE_JSON "${surface_root}/vendor-source/windows-native"
    CACHE PATH "Directory containing nlohmann/json.hpp")
set(ACTION75_SURFACE_PACKS "${surface_root}/wallpapers-native"
    CACHE PATH "Directory containing the surface wallpaper packages")

cmake_host_system_information(RESULT surface_kits QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10 VIEW 32)
file(GLOB surface_sdk_bins "${surface_kits}/bin/10.*/x64")
list(SORT surface_sdk_bins COMPARE NATURAL ORDER DESCENDING)
find_program(ACTION75_SURFACE_FXC fxc HINTS
    "${surface_kits}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    ${surface_sdk_bins} REQUIRED)

set(surface_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${surface_shader_dir}")

add_library(action75_surface_gpu STATIC Gpu.cpp)
target_compile_features(action75_surface_gpu PUBLIC cxx_std_17)
target_compile_definitions(action75_surface_gpu PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(action75_surface_gpu PUBLIC /utf-8)
target_link_libraries(action75_surface_gpu PUBLIC d3d12)
set_property(TARGET action75_surface_gpu PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(add_surface_pack id variant scene material symbol_prefix)
    set(target "wallpaper_${id}")
    add_library(${target} MODULE Wallpaper.cpp "${scene}.cpp")
    target_compile_definitions(${target} PRIVATE ACTION75_WALLPAPER12_EXPORTS "A75_${variant}")
    target_include_directories(${target} PRIVATE "${ACTION75_SURFACE_SDK}"
        "${ACTION75_SURFACE_JSON}" "${surface_shader_dir}")
    target_link_libraries(${target} PRIVATE action75_surface_gpu)
    target_link_options(${target} PRIVATE /Brepro)
    set(output_dir "$<1:${CMAKE_BINARY_DIR}/native-wallpapers/${id}>")
    set_target_properties(${target} PROPERTIES OUTPUT_NAME wallpaper
        RUNTIME_OUTPUT_DIRECTORY "${output_dir}"
        LIBRARY_OUTPUT_DIRECTORY "${output_dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${output_dir}"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    set(source "${CMAKE_CURRENT_SOURCE_DIR}/materials/${material}.hlsl")
    foreach(stage IN LISTS ARGN)
        string(REPLACE ":" ";" stage "${stage}")
        list(GET stage 0 profile)
        list(GET stage 1 entry)
        list(GET stage 2 suffix)
        set(symbol "${symbol_prefix}${suffix}")
        set(header "${surface_shader_dir}/${symbol}.h")
        add_custom_command(OUTPUT "${header}"
            COMMAND "${ACTION75_SURFACE_FXC}" /nologo /O3 /T "${profile}_5_0" /E "${entry}"
                /Vn "${symbol}" /Fh "${header}" "${source}"
            DEPENDS "${source}" "${CMAKE_CURRENT_SOURCE_DIR}/materials/LayerColor.hlsli"
            VERBATIM)
        target_sources(${target} PRIVATE "${header}")
    endforeach()

    if(PROJECT_IS_TOP_LEVEL)
        install(TARGETS ${target}
            LIBRARY DESTINATION "${id}" COMPONENT "${id}"
            RUNTIME DESTINATION "${id}" COMPONENT "${id}")
        install(FILES "${ACTION75_SURFACE_PACKS}/${id}/manifest.json"
                      "${ACTION75_SURFACE_PACKS}/${id}/settings.json"
                      "${ACTION75_SURFACE_PACKS}/${id}/preview.png"
            DESTINATION "${id}" COMPONENT "${id}")
    endif()
endfunction()

add_surface_pack(stillwater WATER WaterSurface WaterSurface Water
    vs:ScreenVertex:ScreenVertex ps:WaterPixel:WaterPixel
    vs:ImpactVertex:ImpactVertex ps:ImpactPixel:ImpactPixel
    cs:StepWave:StepWave cs:BuildSlope:BuildSlope cs:BakeOptics:BakeOptics
    cs:DownsampleOptics:DownsampleOptics)
add_surface_pack(night-fireworks FIREWORKS FireworksScene Fireworks Fireworks
    vs:sparkVS:SparkVS ps:sparkPS:SparkPS vs:screenVS:ScreenVS
    ps:blurPS:BlurPS ps:compositePS:CompositePS ps:idlePS:IdlePS)
add_surface_pack(glass-shatter GLASS GlassShatter GlassShatter Glass
    vs:backdropVS:BackdropVS ps:backdropPS:BackdropPS vs:crackVS:CrackVS
    ps:crackPS:CrackPS vs:paneVS:PaneVS ps:panePS:PanePS)

option(ACTION75_SURFACE_REPLAY "Build the offscreen surface comparison driver" OFF)
if(ACTION75_SURFACE_REPLAY)
    add_executable(surface_replay tests/Replay.cpp)
    target_compile_features(surface_replay PRIVATE cxx_std_17)
    target_compile_definitions(surface_replay PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(surface_replay PRIVATE "${ACTION75_SURFACE_SDK}" "${ACTION75_SURFACE_JSON}")
    target_link_libraries(surface_replay PRIVATE d3d11 d3d12 dxgi)
endif()
