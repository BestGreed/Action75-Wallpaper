# The D3D11 mode is kept for the historical pixel probe.
function(action75_black_hole_shaders target)
    set(source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src")
    if(ARGN STREQUAL "DX12")
        set(api dx12)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/BlackHoleDx12Shaders.h")
    else()
        set(api dx11)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/BlackHoleDx11.h")
    endif()
    add_executable(action75_compile_black_hole EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CompileBlackHole.cpp")
    target_compile_features(action75_compile_black_hole PRIVATE cxx_std_17)
    target_include_directories(action75_compile_black_hole PRIVATE "${source}")
    target_compile_definitions(action75_compile_black_hole PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(action75_compile_black_hole PRIVATE d3dcompiler)
    add_custom_command(OUTPUT "${output}"
        COMMAND action75_compile_black_hole "${source}/BlackHole.hlsl" "${output}" "${api}"
        DEPENDS action75_compile_black_hole "${source}/BlackHole.hlsl" VERBATIM)
    target_sources(${target} PRIVATE "${source}/BlackHoleSimulation.cpp" "${output}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
