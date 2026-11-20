function(add_surface_pack id variant renderer)
    set(target "wallpaper_${id}")
    add_library(${target} MODULE "${ACTION75_ROOT}/examples/native-wallpapers/surface-scenes/Wallpaper.cpp")
    target_compile_definitions(${target} PRIVATE ACTION75_WALLPAPER_EXPORTS A75_${variant})
    target_link_libraries(${target} PRIVATE ${renderer} action75_settings d3d11 dxguid)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME wallpaper
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/native-wallpapers/${id}>"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/native-wallpapers/${id}>"
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/native-wallpapers/${id}>")
endfunction()
add_surface_pack(stillwater WATER action75_water_surface)
add_surface_pack(night-fireworks FIREWORKS action75_fireworks)
add_surface_pack(glass-shatter GLASS action75_glass_shatter)
