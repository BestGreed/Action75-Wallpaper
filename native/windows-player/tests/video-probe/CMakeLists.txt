# Included on demand by the parent Windows player project. It does not register
# an automatic test: the hardware decoder probe is explicitly invoked by the caller.
add_executable(Action75VideoProbe VideoProbe.cpp ../../src/VideoBackground.cpp)
target_include_directories(Action75VideoProbe PRIVATE ../../include)
target_link_libraries(Action75VideoProbe PRIVATE action75_json d3d11 dxgi ole32
  mfplat mfuuid mf oleaut32 shlwapi)
target_compile_options(Action75VideoProbe PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4>)
