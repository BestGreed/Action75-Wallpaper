include_guard(GLOBAL)
get_filename_component(native_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_executable(action75_compile_fireworks_shaders EXCLUDE_FROM_ALL "${native_root}/cmake/CompileFireworksShaders.cpp")
target_compile_features(action75_compile_fireworks_shaders PRIVATE cxx_std_17)
target_link_libraries(action75_compile_fireworks_shaders PRIVATE d3dcompiler)
set(background_shader "${native_root}/materials/Background.hlsl")
set(background_header "${CMAKE_CURRENT_BINARY_DIR}/BackgroundShaders.h")
add_custom_command(OUTPUT "${background_header}"
    COMMAND action75_compile_fireworks_shaders "${background_shader}" "${background_header}" background
    DEPENDS action75_compile_fireworks_shaders "${background_shader}" VERBATIM)
add_library(action75_pack_backdrop STATIC "${background_header}"
    "${native_root}/src/NativeRenderer.cpp" "${native_root}/src/VideoBackground.cpp")
target_include_directories(action75_pack_backdrop PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" PUBLIC "${native_root}/include")
target_compile_definitions(action75_pack_backdrop PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(action75_pack_backdrop PUBLIC action75_effekseer_backend
    d3d11 dxgi windowscodecs ole32 mfplat mfuuid mf oleaut32 shlwapi)
