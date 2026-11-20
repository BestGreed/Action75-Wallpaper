# Each pack gets a self-contained DLL, with the renderer and theme selected at build time.
set(field_source "${ACTION75_ROOT}/examples/native-wallpapers/field-keycap/src")
set(field_native "${ACTION75_ROOT}/native/windows-player")
include("${CMAKE_CURRENT_LIST_DIR}/FieldShaders.cmake")
add_library(action75_field_pack_renderers STATIC
    "${field_native}/src/FieldEffect.cpp" "${field_native}/src/KeycapEffect.cpp")
action75_use_field_shaders(action75_field_pack_renderers)
target_include_directories(action75_field_pack_renderers PUBLIC
    "${field_native}/include" "${ACTION75_ROOT}/vendor-source/windows-native")
target_compile_features(action75_field_pack_renderers PUBLIC cxx_std_17)
target_compile_definitions(action75_field_pack_renderers PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(action75_field_pack_renderers PRIVATE /utf-8)
target_link_libraries(action75_field_pack_renderers PUBLIC d3d11 windowscodecs ole32)
function(add_field_pack pack)
    set(target "wallpaper_${pack}")
    add_library(${target} MODULE "${field_source}/Wallpaper.cpp")
    target_compile_definitions(${target} PRIVATE ACTION75_WALLPAPER_EXPORTS)
    if(ARGC GREATER 1)
        target_compile_definitions(${target} PRIVATE FIELD_THEME=${ARGV1})
    endif()
    target_compile_options(${target} PRIVATE /utf-8)
    target_link_libraries(${target} PRIVATE action75_field_pack_renderers)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME wallpaper
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/native-wallpapers/${pack}>"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/native-wallpapers/${pack}>"
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/native-wallpapers/${pack}>")
endfunction()
add_field_pack(silk-nebula 0)
add_field_pack(gilded-synapse 1)
add_field_pack(harmonic-textile 3)
add_field_pack(keycap-glow)
