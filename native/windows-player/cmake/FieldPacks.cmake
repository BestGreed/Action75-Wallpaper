# The player and legacy render probes still compile their own field shaders.
set(field_native "${CMAKE_CURRENT_LIST_DIR}/..")
include("${CMAKE_CURRENT_LIST_DIR}/FieldShaders.cmake")

# The shipped DLLs build against the standalone author project and public ABI.
add_subdirectory("${ACTION75_ROOT}/examples/native-wallpapers/field-keycap"
                 "${CMAKE_CURRENT_BINARY_DIR}/field-keycap")
