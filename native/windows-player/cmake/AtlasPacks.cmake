# Optional author targets, independent of player selection or runtime dispatch.
include_guard(GLOBAL)
get_filename_component(atlas_native "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(atlas_root "${atlas_native}/../.." ABSOLUTE)
set(atlas_sources "${atlas_root}/examples/native-wallpapers/atlas-packs")
function(atlas_module target source)
    add_library(${target} MODULE "${atlas_sources}/${source}" ${ARGN})
    target_compile_definitions(${target} PRIVATE ACTION75_WALLPAPER_EXPORTS)
    target_link_libraries(${target} PRIVATE action75_settings action75_pack_backdrop)
endfunction()
set(atlas_shader_header "${CMAKE_CURRENT_BINARY_DIR}/AtlasShaderBytes.h")
add_custom_command(OUTPUT "${atlas_shader_header}"
    COMMAND action75_compile_fireworks_shaders "${atlas_native}/src/SpriteBurstShader.h"
        "${atlas_shader_header}" atlas
    DEPENDS action75_compile_fireworks_shaders "${atlas_native}/src/SpriteBurstShader.h" VERBATIM)
atlas_module(action75_atlas_pack Atlas.cpp "${atlas_native}/src/SpriteBurst.cpp" "${atlas_shader_header}")
target_compile_definitions(action75_atlas_pack PRIVATE ACTION75_PACKED_ATLAS)
target_include_directories(action75_atlas_pack PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
atlas_module(action75_meridian_pack Meridian.cpp
    "${atlas_native}/src/ProceduralBurst.cpp" "${atlas_native}/src/PixelFluid.cpp")
atlas_module(action75_paintball_pack Paintball.cpp)

add_executable(action75_prepare_paintball EXCLUDE_FROM_ALL "${atlas_sources}/PreparePaintball.cpp")
target_include_directories(action75_prepare_paintball PRIVATE
    "${ACTION75_EFFEKSEER_ROOT}/src/EffekseerMaterialCompiler")
target_link_libraries(action75_prepare_paintball PRIVATE EffekseerRendererDX11)

add_custom_target(action75_atlas_packs)
function(stage_atlas_pack id module)
    add_custom_target(stage_atlas_${id}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${module}>"
            "${atlas_root}/wallpapers-native/${id}/wallpaper.dll"
        DEPENDS ${module} VERBATIM)
    add_dependencies(action75_atlas_packs stage_atlas_${id})
endfunction()
foreach(id afterimage carrier folio tidal-glass)
    stage_atlas_pack(${id} action75_atlas_pack)
endforeach()
stage_atlas_pack(meridian action75_meridian_pack)
stage_atlas_pack(paintball-native action75_paintball_pack)

set(paintball_source "${atlas_root}/wallpapers-native/paintball-native")
file(GLOB paintball_materials CONFIGURE_DEPENDS "${paintball_source}/materials/*.efkmat")
set(paintball_cache_dir "${CMAKE_CURRENT_BINARY_DIR}/paintball-cache")
foreach(material IN LISTS paintball_materials)
    get_filename_component(name "${material}" NAME)
    list(APPEND paintball_cache_files "${paintball_cache_dir}/materials/${name}d")
endforeach()
add_custom_command(OUTPUT ${paintball_cache_files}
    COMMAND action75_prepare_paintball "${paintball_source}" "${paintball_cache_dir}"
    DEPENDS action75_prepare_paintball ${paintball_materials} VERBATIM)
add_custom_target(stage_paintball_materials
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${paintball_cache_files} "${paintball_source}/materials/"
    DEPENDS ${paintball_cache_files} VERBATIM)
add_dependencies(stage_atlas_paintball-native stage_paintball_materials)
