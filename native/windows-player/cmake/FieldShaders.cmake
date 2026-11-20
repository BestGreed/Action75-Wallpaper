# Use the Windows SDK shader compiler from the selected targeting kit.
cmake_host_system_information(RESULT field_kits QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10 VIEW 32)
file(GLOB field_sdk_bins "${field_kits}/bin/10.*/x64")
list(SORT field_sdk_bins COMPARE NATURAL ORDER DESCENDING)
find_program(ACTION75_FIELD_FXC fxc HINTS
    "${field_kits}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    ${field_sdk_bins} REQUIRED)

set(field_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/field-shaders")
file(MAKE_DIRECTORY "${field_shader_dir}")
function(compile_field_stage source entry symbol)
    set(profile ps_5_0)
    if(entry STREQUAL "vs")
        set(profile vs_5_0)
    endif()
    set(header "${field_shader_dir}/${symbol}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${ACTION75_FIELD_FXC}" /nologo /O3 /T ${profile} /E ${entry}
            ${ARGN} /Vn ${symbol} /Fh "${header}" "${field_native}/materials/${source}.hlsl"
        DEPENDS "${field_native}/materials/${source}.hlsl" "${field_native}/materials/ProceduralBackground.hlsli" "${field_native}/materials/LayerColor.hlsli" VERBATIM)
    set(field_outputs ${field_outputs} "${header}" PARENT_SCOPE)
endfunction()

set(field_entries vs sim shade composite blurHorizontal blurVertical)
set(field_table "")
set(field_includes "")
foreach(theme RANGE 0 3)
    string(APPEND field_table "  {")
    foreach(entry IN LISTS field_entries)
        set(symbol "Field${theme}_${entry}")
        compile_field_stage(Field ${entry} ${symbol} /D FIELD_THEME=${theme})
        string(APPEND field_includes "#include \"${symbol}.h\"\n")
        string(APPEND field_table "{${symbol}, sizeof(${symbol})},")
    endforeach()
    foreach(entry shade composite)
        set(symbol "Field${theme}_graded_${entry}")
        compile_field_stage(Field ${entry} ${symbol} /D FIELD_THEME=${theme} /D GRADE_BACKGROUND=1)
        string(APPEND field_includes "#include \"${symbol}.h\"\n")
        string(APPEND field_table "{${symbol}, sizeof(${symbol})},")
    endforeach()
    string(APPEND field_table "},\n")
endforeach()
file(GENERATE OUTPUT "${field_shader_dir}/FieldShaders.h" CONTENT
"#pragma once
#include <cstddef>
${field_includes}
struct FieldBytecode { const BYTE *data; std::size_t size; };
struct FieldShaderSet { FieldBytecode vs, sim, shade, composite, blurHorizontal, blurVertical, gradedShade, gradedComposite; };
static const FieldShaderSet FieldShaders[] = {
${field_table}};
")
compile_field_stage(Keycap vs Keycap_vs)
compile_field_stage(Keycap ps Keycap_ps)
compile_field_stage(Keycap ps Keycap_graded /D GRADE_BACKGROUND=1)
file(GENERATE OUTPUT "${field_shader_dir}/KeycapShaders.h" CONTENT
    "#pragma once\n#include \"Keycap_vs.h\"\n#include \"Keycap_ps.h\"\n#include \"Keycap_graded.h\"\n")
add_custom_target(action75_field_shaders DEPENDS ${field_outputs})

function(action75_use_field_shaders target)
    target_include_directories(${target} PRIVATE "${field_shader_dir}")
    add_dependencies(${target} action75_field_shaders)
endfunction()
