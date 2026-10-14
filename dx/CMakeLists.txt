set(POP_DX_SOURCES
  com.cpp dx.cpp host_api.cpp display_stubs.cpp ddraw.cpp d3d.cpp ddraw7.cpp d3d7.cpp d3d7_soft.cpp d3d11.cpp dxgi.cpp d3dcompiler.cpp
  d3dx10.cpp dsound.cpp dshow.cpp mp3_source.cpp dinput.cpp dinput_joystick.cpp xinput.cpp qmixer.cpp
  mss32.cpp redbook.cpp avi.cpp waveout.cpp fmod.cpp soundlib.cpp galaxy_stub.cpp riff.cpp bink.cpp video_frame.cpp mf_media.cpp
  mf.cpp weanetr.cpp d3d9.cpp d3d9_raster.cpp d3d9_shader.cpp d3d9_shadergen.cpp host_d9_default.cpp
  d3dx9.cpp dmusic.cpp dmusic_reverb.cpp)

# GothicKit/dmusic (third_party/dmusic/README.md): the DirectMusic composer and
# synthesizer behind dmusic.cpp. Its sources are compiled into the DX object
# libraries themselves, so every binary that links the shims has them: vendored
# C11 built as it is, with no kit warnings, its own C11-threads shim, and every
# external name it does not export as Dm* renamed by recomp/dm_prefix.h,
# because the hosts link the kit's own TinySoundFont beside it.
set(POP_DMUSIC_DIR ${POP_ROOT}/third_party/dmusic)
set(POP_DMUSIC_SOURCES
  src/Array.c src/Band.c src/Common.c src/Composer.c src/Dls.c src/Loader.c src/Logger.c
  src/Memory.c src/Message.c src/Performance.c src/Riff.c src/Rng.c src/Segment.c src/Style.c
  src/Synth.c src/io/Band.c src/io/Common.c src/io/Dls.c src/io/Segment.c src/io/Style.c
  src/thread/Thread.Posix.c src/thread/Thread.Win32.c src/util/Tsf.c vendor/TinySoundFont/tsf.c)
list(TRANSFORM POP_DMUSIC_SOURCES PREPEND ${POP_DMUSIC_DIR}/)
set_source_files_properties(${POP_DMUSIC_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-w;-include;${POP_DMUSIC_DIR}/recomp/dm_prefix.h"
  COMPILE_DEFINITIONS "DM_BUILD=1"
  INCLUDE_DIRECTORIES "${POP_DMUSIC_DIR}/src;${POP_DMUSIC_DIR}/vendor/TinySoundFont")
list(APPEND POP_DX_SOURCES ${POP_DMUSIC_SOURCES})

add_library(recomp_dx OBJECT ${POP_DX_SOURCES})
target_include_directories(recomp_dx PUBLIC ${POP_DMUSIC_DIR}/include)
target_compile_definitions(recomp_dx PUBLIC DM_STATIC=1)
target_link_libraries(recomp_dx PUBLIC recomp_runtime)
pop_link_video(recomp_dx)
target_compile_options(recomp_dx PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_dx 2)

# The same shims with every host callback a strong no-op: a binary linking
# these can neither draw nor play sound. The parity fixture and the profile
# tests use it.
add_library(recomp_dx_null OBJECT ${POP_DX_SOURCES})
target_include_directories(recomp_dx_null PUBLIC ${POP_DMUSIC_DIR}/include)
target_compile_definitions(recomp_dx_null PUBLIC DM_STATIC=1)
target_link_libraries(recomp_dx_null PUBLIC recomp_runtime)
pop_link_video(recomp_dx_null)
target_compile_definitions(recomp_dx_null PUBLIC RECOMP_NULL_HOST)
target_compile_options(recomp_dx_null PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_dx_null 1)

if(NOT IOS AND NOT ANDROID)
# host_api.h is a C header too. The C11 consumer is an object the DX tests
# link; the header is also parsed as C++17 on its own.
add_library(host_api_header_test OBJECT tests/host_api_header_test.c)
target_include_directories(host_api_header_test PRIVATE ${POP_ROOT})
target_compile_options(host_api_header_test PRIVATE -Wall -Wextra)
pop_optimize(host_api_header_test 1)
add_test(NAME host_api_header_check
  COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter
          -I${POP_ROOT} -fsyntax-only -x c++ ${CMAKE_CURRENT_SOURCE_DIR}/host_api.h)
set_tests_properties(host_api_header_check PROPERTIES LABELS nogame)

add_executable(dx_tests tests/dx_tests.cpp
  ${POP_ROOT}/runtime/tests/stub_recomp_call.cpp
  $<TARGET_OBJECTS:host_api_header_test>)
target_link_libraries(dx_tests PRIVATE recomp_platform recomp_dx recomp_runtime)
target_compile_options(dx_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(dx_tests 1)
pop_test_binary(dx_tests)
add_test(NAME dx_tests COMMAND dx_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(dx_tests PROPERTIES LABELS nogame)

# DirectDraw 7 / Direct3D 7: the object model, surfaces, Blt and Flip on the
# CPU and on the software reference renderer, and synthetic scenes checked
# against hand-computed pixels. Headless; host/tests/d3d7_gpu_tests.cpp runs
# the same scenes on the GPU.
add_executable(d3d7_tests tests/d3d7_tests.cpp
  ${POP_ROOT}/runtime/tests/stub_recomp_call.cpp)
target_link_libraries(d3d7_tests PRIVATE recomp_platform recomp_dx recomp_runtime)
target_compile_options(d3d7_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(d3d7_tests 1)
pop_test_binary(d3d7_tests)
add_test(NAME d3d7_tests COMMAND d3d7_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(d3d7_tests PROPERTIES LABELS nogame)

# The virtual pad through the DirectInput joystick and XInput. Its own binary
# because it defines the host_pad_* callbacks strongly, which dx_tests must not.
add_executable(pad_tests tests/pad_tests.cpp
  ${POP_ROOT}/runtime/tests/stub_recomp_call.cpp)
target_link_libraries(pad_tests PRIVATE recomp_platform recomp_dx recomp_runtime)
target_compile_options(pad_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(pad_tests 1)
pop_test_binary(pad_tests)
add_test(NAME pad_tests COMMAND pad_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(pad_tests PROPERTIES LABELS nogame)

# The window system a game sees: per-thread message queues, sent messages,
# activation and focus, keyboard translation, and the DirectInput keyboard
# and mouse that hang off activation. Its own binary because it defines
# host_input_state strongly and runs guest threads.
add_executable(win32_ui_tests tests/win32_ui_tests.cpp
  ${POP_ROOT}/runtime/tests/stub_recomp_call.cpp)
target_link_libraries(win32_ui_tests PRIVATE recomp_platform recomp_dx recomp_runtime)
target_compile_options(win32_ui_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(win32_ui_tests 1)
pop_test_binary(win32_ui_tests)
add_test(NAME win32_ui_tests COMMAND win32_ui_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(win32_ui_tests PROPERTIES LABELS nogame TIMEOUT 120)

# No strong host callback anywhere: the weak defaults must carry the link.
# page_overlay.cpp because every host links it beside boot.cpp; the dispatch
# stub because no generated code is present.
add_executable(null_host_link tests/null_host_link.cpp
  ${POP_ROOT}/host/page_overlay.cpp
  ${POP_ROOT}/runtime/tests/stub_recomp_call.cpp)
target_include_directories(null_host_link PRIVATE ${POP_ROOT})
target_link_libraries(null_host_link PRIVATE recomp_platform recomp_runtime recomp_dx host_common)
target_compile_options(null_host_link PRIVATE ${POP_WARN_HOST})
pop_optimize(null_host_link 1)
pop_test_binary(null_host_link)
add_test(NAME null_host_link COMMAND null_host_link WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(null_host_link PROPERTIES LABELS nogame)
endif()
