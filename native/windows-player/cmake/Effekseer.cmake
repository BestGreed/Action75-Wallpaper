# Only the pinned C++ CPU-particle runtime and D3D11 renderer are built.
# No editor, samples, window/input library, sound system or secondary GPU API.
set(ACTION75_EFFEKSEER_VERSION "1.80.8")
set(ACTION75_EFFEKSEER_TAG "1808")
set(ACTION75_EFFEKSEER_SHA256 "4206c2b3704fee9f84331e18c7f80d061f7989910b1fada82c7857b2b5efc035")
get_filename_component(_action75_project_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_action75_vendor_root "${_action75_project_root}/vendor-source/windows-native")
set(ACTION75_EFFEKSEER_ROOT
    "${_action75_vendor_root}/effekseer/EffekseerForCpp${ACTION75_EFFEKSEER_VERSION}"
    CACHE PATH "Pinned Effekseer C++ SDK source root")

if(NOT EXISTS "${ACTION75_EFFEKSEER_ROOT}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${_action75_vendor_root}")
    set(_action75_archive "${_action75_vendor_root}/EffekseerForCpp${ACTION75_EFFEKSEER_VERSION}.zip")
    if(EXISTS "${_action75_archive}")
        file(SHA256 "${_action75_archive}" _action75_archive_sha256)
        if(NOT _action75_archive_sha256 STREQUAL ACTION75_EFFEKSEER_SHA256)
            message(FATAL_ERROR "Bundled Effekseer SDK archive SHA256 mismatch")
        endif()
    else()
        file(DOWNLOAD
            "https://github.com/effekseer/Effekseer/releases/download/${ACTION75_EFFEKSEER_TAG}/EffekseerForCpp${ACTION75_EFFEKSEER_VERSION}.zip"
            "${_action75_archive}"
            EXPECTED_HASH "SHA256=${ACTION75_EFFEKSEER_SHA256}"
            TLS_VERIFY ON
            STATUS _action75_download_status)
        list(GET _action75_download_status 0 _action75_download_code)
        if(NOT _action75_download_code EQUAL 0)
            message(FATAL_ERROR "Cannot acquire the pinned Effekseer SDK: ${_action75_download_status}")
        endif()
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_action75_archive}" DESTINATION "${_action75_vendor_root}/effekseer")
endif()

foreach(_action75_option BUILD_EXAMPLES BUILD_EDITOR BUILD_VIEWER BUILD_TEST
        BUILD_GL BUILD_DX9 BUILD_DX12 BUILD_VULKAN BUILD_METAL BUILD_WEBGPU
        USE_OPENAL USE_XAUDIO2 USE_DSOUND USE_OSM USE_LIBPNG_LOADER)
    set(${_action75_option} OFF CACHE BOOL "Disabled for Action75 native runtime" FORCE)
endforeach()
set(BUILD_DX11 ON CACHE BOOL "Build the Action75 D3D11 renderer" FORCE)
set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "Use the parent's static MSVC runtime" FORCE)
add_subdirectory("${ACTION75_EFFEKSEER_ROOT}" "${CMAKE_BINARY_DIR}/effekseer-runtime" EXCLUDE_FROM_ALL)

get_filename_component(_action75_native_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_library(action75_effekseer_backend STATIC
    "${_action75_native_root}/src/EffekseerBackend.cpp"
    "${_action75_native_root}/include/EffekseerBackend.h")
target_compile_features(action75_effekseer_backend PUBLIC cxx_std_17)
target_compile_definitions(action75_effekseer_backend PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_property(TARGET action75_effekseer_backend PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(action75_effekseer_backend PUBLIC "${_action75_native_root}/include")
target_link_libraries(action75_effekseer_backend PRIVATE
    EffekseerRendererDX11 d3d11 d3dcompiler dxgi dxguid)
set(ACTION75_EFFEKSEER_DIAGNOSTIC_RESOURCES "${ACTION75_EFFEKSEER_ROOT}/Examples/Resources")
