cmake_minimum_required(VERSION 3.20)
project(Action75AtlasPacks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(atlas_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(ACTION75_SDK_INCLUDE "${atlas_repo}/skills/action75-wallpaper-author/sdk" CACHE PATH "Public Action75 ABI header directory")
set(ACTION75_SDK_VENDOR "${atlas_repo}/vendor-source/windows-native" CACHE PATH "Third-party source directory")
set(ACTION75_PACK_ASSETS "${atlas_repo}/wallpapers-native" CACHE PATH "Directory containing the original pack assets")
set(ACTION75_EFFEKSEER_ROOT "${ACTION75_SDK_VENDOR}/effekseer/EffekseerForCpp1.80.8" CACHE PATH "Pinned Effekseer C++ SDK source")
option(ACTION75_ATLAS_PAINTBALL "Build the native DX12 Paintball pack" ON)
set(ACTION75_ATLAS_STAGING "${CMAKE_CURRENT_BINARY_DIR}/packs" CACHE PATH "Output directory for importable packs")

if(NOT EXISTS "${ACTION75_SDK_INCLUDE}/Action75Wallpaper12.h" OR
   NOT EXISTS "${ACTION75_SDK_INCLUDE}/Action75Wallpaper.h" OR
   NOT EXISTS "${ACTION75_SDK_VENDOR}/nlohmann/json.hpp")
  message(FATAL_ERROR "Set ACTION75_SDK_INCLUDE and ACTION75_SDK_VENDOR to the public ABI headers and nlohmann/json.hpp.")
endif()

function(atlas_target target)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_include_directories(${target} PRIVATE "${ACTION75_SDK_INCLUDE}" "${ACTION75_SDK_VENDOR}")
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8)
  endif()
endfunction()

add_executable(compile_atlas_shaders CompileShaders12.cpp)
target_link_libraries(compile_atlas_shaders PRIVATE d3dcompiler)
atlas_target(compile_atlas_shaders)
function(atlas_shader label output)
  set(header "${CMAKE_CURRENT_BINARY_DIR}/${label}Shaders12.h")
  set(shader "${CMAKE_CURRENT_SOURCE_DIR}/${label}12.hlsl")
  add_custom_command(OUTPUT "${header}"
    COMMAND compile_atlas_shaders "${shader}" "${header}" ${label}
    DEPENDS compile_atlas_shaders "${shader}" VERBATIM)
  set(${output} "${header}" PARENT_SCOPE)
endfunction()

atlas_shader(Backdrop backdrop_shader)
atlas_shader(Atlas atlas_shader_header)
atlas_shader(Meridian meridian_shader)
add_library(atlas_package_support STATIC Graphics.cpp Settings.cpp Backdrop12.cpp "${backdrop_shader}")
atlas_target(atlas_package_support)
target_include_directories(atlas_package_support PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(atlas_package_support PUBLIC d3d12 windowscodecs ole32)

function(add_atlas_module target)
  add_library(${target} MODULE ${ARGN})
  atlas_target(${target})
  target_compile_definitions(${target} PRIVATE ACTION75_WALLPAPER12_EXPORTS)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(${target} PRIVATE atlas_package_support)
endfunction()

function(stage_atlas_module id target)
  add_custom_target(stage_atlas_${id}
    COMMAND "${CMAKE_COMMAND}"
      "-DASSETS=${ACTION75_PACK_ASSETS}/${id}" "-DDESTINATION=${ACTION75_ATLAS_STAGING}/${id}"
      "-DPROJECT_LICENSE=${atlas_repo}/LICENSE" "-DJSON_LICENSE=${atlas_repo}/Licenses/nlohmann-json-LICENSE.txt"
      "-DMODULE=$<TARGET_FILE:${target}>" ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/StagePack.cmake"
    DEPENDS ${target} VERBATIM)
  add_dependencies(action75_atlas_packs stage_atlas_${id})
endfunction()

add_custom_target(action75_atlas_packs)
add_atlas_module(action75_atlas_pack Atlas12.cpp "${atlas_shader_header}")
foreach(id afterimage carrier folio tidal-glass)
  stage_atlas_module(${id} action75_atlas_pack)
endforeach()
add_atlas_module(action75_meridian_pack Meridian12.cpp "${meridian_shader}")
stage_atlas_module(meridian action75_meridian_pack)

if(ACTION75_ATLAS_PAINTBALL)
  # The package is built in its own CMake project, independently of any player targets.
  if(NOT EXISTS "${ACTION75_EFFEKSEER_ROOT}/CMakeLists.txt")
    set(effekseer_archive "${ACTION75_SDK_VENDOR}/EffekseerForCpp1.80.8.zip")
    set(effekseer_sha "4206c2b3704fee9f84331e18c7f80d061f7989910b1fada82c7857b2b5efc035")
    if(NOT EXISTS "${effekseer_archive}")
      set(effekseer_archive "${CMAKE_CURRENT_BINARY_DIR}/EffekseerForCpp1.80.8.zip")
      file(DOWNLOAD https://github.com/effekseer/Effekseer/releases/download/1808/EffekseerForCpp1.80.8.zip
        "${effekseer_archive}" TLS_VERIFY ON EXPECTED_HASH "SHA256=${effekseer_sha}")
    endif()
    file(SHA256 "${effekseer_archive}" archive_sha)
    if(NOT archive_sha STREQUAL effekseer_sha)
      message(FATAL_ERROR "Effekseer C++ SDK archive SHA-256 mismatch")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${effekseer_archive}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/vendor")
    set(ACTION75_EFFEKSEER_ROOT "${CMAKE_CURRENT_BINARY_DIR}/vendor/EffekseerForCpp1.80.8")
  endif()
  foreach(option BUILD_EXAMPLES BUILD_EDITOR BUILD_VIEWER BUILD_TEST BUILD_GL BUILD_DX9 BUILD_DX11
                 BUILD_VULKAN BUILD_METAL BUILD_WEBGPU USE_OPENAL USE_XAUDIO2 USE_DSOUND USE_OSM
                 USE_LIBPNG_LOADER USE_MSVC_RUNTIME_LIBRARY_DLL)
    set(${option} OFF CACHE BOOL "Disabled in the native Paintball renderer" FORCE)
  endforeach()
  set(BUILD_DX12 ON CACHE BOOL "Build the native Effekseer DX12 renderer" FORCE)
  include(PaintballCopyQueue.cmake)
  prepare_paintball_copy_sdk("${ACTION75_EFFEKSEER_ROOT}" paintball_effekseer)
  add_subdirectory("${paintball_effekseer}" effekseer EXCLUDE_FROM_ALL)
  add_atlas_module(action75_paintball_pack Paintball12.cpp PaintballImages12.cpp)
  target_include_directories(action75_paintball_pack PRIVATE "${paintball_effekseer}/src/3rdParty/LLGI/src")
  target_link_libraries(action75_paintball_pack PRIVATE EffekseerRendererDX12)
  add_executable(action75_prepare_paintball_dx12 PreparePaintball12.cpp)
  atlas_target(action75_prepare_paintball_dx12)
  target_include_directories(action75_prepare_paintball_dx12 PRIVATE "${paintball_effekseer}/src/EffekseerMaterialCompiler")
  target_link_libraries(action75_prepare_paintball_dx12 PRIVATE EffekseerRendererDX12)
  set(material_source "${ACTION75_PACK_ASSETS}/paintball-native/materials")
  set(material_cache "${CMAKE_CURRENT_BINARY_DIR}/material-cache")
  file(GLOB authored_materials CONFIGURE_DEPENDS "${material_source}/*.efkmat")
  file(GLOB previous_material_caches CONFIGURE_DEPENDS "${material_source}/*.efkmatd")
  if(NOT authored_materials)
    message(FATAL_ERROR "Paintball authored materials not found in ${material_source}")
  endif()
  foreach(material IN LISTS authored_materials)
    get_filename_component(filename "${material}" NAME)
    list(APPEND native_materials "${material_cache}/${filename}d")
  endforeach()
  add_custom_command(OUTPUT ${native_materials}
    COMMAND action75_prepare_paintball_dx12 "${material_source}" "${material_cache}"
    DEPENDS action75_prepare_paintball_dx12 ${authored_materials} ${previous_material_caches} VERBATIM)
  add_custom_target(paintball_materials_dx12 DEPENDS ${native_materials})
  stage_atlas_module(paintball-native action75_paintball_pack "-DMATERIALS=${material_cache}"
    "-DEFFEKSEER_LICENSE_ROOT=${ACTION75_EFFEKSEER_ROOT}")
  add_dependencies(stage_atlas_paintball-native paintball_materials_dx12)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(verify_atlas_dx12 tests/Verify.cpp)
  atlas_target(verify_atlas_dx12)
  target_include_directories(verify_atlas_dx12 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(verify_atlas_dx12 PRIVATE atlas_package_support d3d11 dxgi)
endif()
