# Apply the DX12 changes only to this package's copy of the pinned SDK. The
# player and other plugins may build against the same original SDK directory.
function(prepare_paintball_copy_sdk source output)
  find_package(Git REQUIRED)
  set(patch "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PaintballCopyQueue.patch")
  set(sdk "${CMAKE_CURRENT_BINARY_DIR}/paintball-copy-sdk")
  set(stamp "${CMAKE_CURRENT_BINARY_DIR}/paintball-copy-sdk.stamp")
  file(SHA256 "${patch}" fingerprint)
  file(STRINGS "${patch}" headers REGEX "^--- a/")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
  foreach(header IN LISTS headers)
    string(SUBSTRING "${header}" 6 -1 path)
    list(APPEND patched_files "${path}")
    get_filename_component(filename "${path}" NAME)
    list(APPEND exclusions PATTERN "${filename}" EXCLUDE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}/${path}")
    file(SHA256 "${source}/${path}" hash)
    string(APPEND fingerprint "\n${hash}")
  endforeach()

  file(COPY "${source}/" DESTINATION "${sdk}" ${exclusions})
  if(EXISTS "${stamp}")
    file(READ "${stamp}" previous)
  endif()
  foreach(path IN LISTS patched_files)
    if(NOT EXISTS "${sdk}/${path}")
      unset(previous)
    endif()
  endforeach()
  if(NOT "${fingerprint}" STREQUAL "${previous}")
    file(REMOVE "${stamp}")
    foreach(path IN LISTS patched_files)
      configure_file("${source}/${path}" "${sdk}/${path}" COPYONLY)
    endforeach()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}"
              "${GIT_EXECUTABLE}" -c core.autocrlf=false apply --ignore-space-change
              --whitespace=nowarn "${patch}"
      WORKING_DIRECTORY "${sdk}"
      RESULT_VARIABLE patch_result
      ERROR_VARIABLE patch_error)
    if(NOT patch_result EQUAL 0)
      message(FATAL_ERROR "Could not prepare the Paintball DX12 SDK: ${patch_error}")
    endif()
    file(WRITE "${stamp}" "${fingerprint}")
  endif()
  set(${output} "${sdk}" PARENT_SCOPE)
endfunction()
