# Assemble the package-owned renderer in a separate project. This keeps both
# Effekseer configurations and all package outputs independent of the player.
include_guard(GLOBAL)
include(ExternalProject)

get_filename_component(atlas_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(atlas_source "${atlas_root}/examples/native-wallpapers/atlas-packs")
set(atlas_build "${CMAKE_CURRENT_BINARY_DIR}/atlas-native")
set(ACTION75_ATLAS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-dx12-packs" CACHE PATH
    "Destination for the assembled native DX12 atlas packs")
set(atlas_configuration "")
if(CMAKE_CONFIGURATION_TYPES)
    set(atlas_configuration --config "$<CONFIG>")
endif()

ExternalProject_Add(action75_build_atlas_native
    SOURCE_DIR "${atlas_source}"
    BINARY_DIR "${atlas_build}"
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
        "-DBUILD_TESTING:BOOL=OFF"
        "-DACTION75_EFFEKSEER_ROOT:PATH=${ACTION75_EFFEKSEER_ROOT}"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> ${atlas_configuration}
        --target action75_atlas_packs
    BUILD_ALWAYS TRUE
    EXCLUDE_FROM_ALL TRUE
    INSTALL_COMMAND "")

add_custom_target(action75_atlas_packs)
foreach(pack afterimage carrier folio tidal-glass meridian paintball-native)
    add_custom_target(stage_atlas_${pack}
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${atlas_build}/packs/${pack}"
            "${ACTION75_ATLAS_OUTPUT}/${pack}"
        DEPENDS action75_build_atlas_native VERBATIM)
    add_dependencies(action75_atlas_packs stage_atlas_${pack})
endforeach()
