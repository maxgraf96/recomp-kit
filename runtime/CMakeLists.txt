# The guest runtime. Object libraries, so every source is linked into every
# consumer exactly as the shell scripts linked every object.
set(POP_RUNTIME_SOURCES
  memory.cpp loader.cpp imports.cpp import_argc.cpp cpu.cpp thunks.cpp seh.cpp profile.cpp kernel32.cpp kernel32_wide.cpp
  resources.cpp oleaut32.cpp mods_seam.cpp user32.cpp user32_wide.cpp user32_vcl.cpp user32_static.cpp user32_activation.cpp user32_ansi.cpp user32_cursor.cpp user32_clipboard.cpp
  misc.cpp misc_dlls.cpp winsock.cpp iphlpapi.cpp net_adapters.cpp comctl32.cpp codepages.cpp imm32.cpp gdi32.cpp gdi32_draw.cpp gdi32_text.cpp gdi32_truetype.cpp gdi32_fontdb.cpp msimg32.cpp
  media_foundation.cpp layout.cpp interp.cpp discovery.cpp dispatch.cpp
  msvcrt.cpp msvcrt_heap.cpp msvcrt_locale.cpp msvcrt_string.cpp msvcrt_format.cpp msvcrt_stdio.cpp
  msvcrt_math.cpp msvcrt_float.cpp msvcrt_time.cpp msvcp60.cpp msvc_eh.cpp
  x87.c ${POP_ROOT}/third_party/softfloat/softfloat_x87.c)

# The exact x87's out-of-line half and the part of SoftFloat 3e it uses
# (third_party/softfloat/README.md). Floating-point contraction would fuse a
# multiply and an add into one rounding, which x87 never does, so it is off
# here as in the generated code (cmake/Translate.cmake); SoftFloat itself is
# integer code and builds freestanding.
set_source_files_properties(x87.c PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
set_source_files_properties(${POP_ROOT}/third_party/softfloat/softfloat_x87.c
  PROPERTIES COMPILE_OPTIONS "-ffreestanding;-ffp-contract=off")

# The faces the runtime's GDI draws Windows' own UI fonts with
# (third_party/fonts/opensans/README.md), embedded as byte arrays.
set(POP_BUNDLED_FONTS_DIR ${POP_ROOT}/third_party/fonts/opensans)
set(POP_BUNDLED_FONTS_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/bundled_fonts.cpp)
add_custom_command(
  OUTPUT ${POP_BUNDLED_FONTS_CPP}
  COMMAND ${CMAKE_COMMAND} -DOUT=${POP_BUNDLED_FONTS_CPP} -DNAMESPACE=gdi
          "-DENTRIES=bundled_sans_regular=${POP_BUNDLED_FONTS_DIR}/OpenSans-Regular.ttf|bundled_sans_semibold=${POP_BUNDLED_FONTS_DIR}/OpenSans-Semibold.ttf"
          -P ${POP_ROOT}/cmake/EmbedFiles.cmake
  DEPENDS ${POP_ROOT}/cmake/EmbedFiles.cmake
          ${POP_BUNDLED_FONTS_DIR}/OpenSans-Regular.ttf
          ${POP_BUNDLED_FONTS_DIR}/OpenSans-Semibold.ttf
  COMMENT "Embedding the bundled fonts"
  VERBATIM)
list(APPEND POP_RUNTIME_SOURCES ${POP_BUNDLED_FONTS_CPP})

add_library(recomp_runtime OBJECT ${POP_RUNTIME_SOURCES})
target_include_directories(recomp_runtime PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_runtime PUBLIC recomp_platform)
target_compile_options(recomp_runtime PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_runtime 2)

# POPM_TESTING compiles the test-only entry points (loader reset, owner
# counter seam, scheduler latch reset). None exists in a real build.
if(NOT IOS AND NOT ANDROID)
add_library(recomp_runtime_testing OBJECT ${POP_RUNTIME_SOURCES})
target_include_directories(recomp_runtime_testing PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_runtime_testing PUBLIC recomp_platform)
target_compile_definitions(recomp_runtime_testing PUBLIC POPM_TESTING=1)
target_compile_options(recomp_runtime_testing PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_runtime_testing 1)
endif()

# The interpreter for heap code, on its own: it needs guest memory and
# recomp_call, which the test provides.
if(NOT IOS AND NOT ANDROID)
add_executable(interp_tests tests/interp_tests.cpp interp.cpp)
target_include_directories(interp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(interp_tests PRIVATE ${POP_WARN_HOST})
pop_test_binary(interp_tests)
add_test(NAME interp_tests COMMAND interp_tests WORKING_DIRECTORY ${POP_ROOT})
# TIMEOUT because these checks assert that a guest routine cannot run
# forever: a regression in them is a hang, not a failure, and a hung run
# reports nothing at all. The suite takes under a second.
set_tests_properties(interp_tests PROPERTIES LABELS nogame TIMEOUT 120)
endif()

# Linked by the hosts, the fixture and the mod tests, not by the unit tests.
add_library(recomp_snapshot OBJECT snapshot.cpp)
target_link_libraries(recomp_snapshot PUBLIC recomp_runtime)
target_compile_options(recomp_snapshot PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_snapshot 2)

# The suite spawns itself for the abort check; portable since sub-project 3.
if(NOT IOS AND NOT ANDROID)
add_executable(seh_tests tests/seh_tests.cpp)
target_link_libraries(seh_tests PRIVATE recomp_platform recomp_runtime_testing)
target_compile_options(seh_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(seh_tests 1)
pop_test_binary(seh_tests)
add_test(NAME seh_tests COMMAND seh_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(seh_tests PROPERTIES LABELS nogame ENVIRONMENT RECOMP_PROFILE=1)
# MSVC C++ and structured exception handling (msvc_eh.cpp) over synthetic
# guest frames; spawns itself for the terminate and abort paths.
add_executable(eh_tests tests/eh_tests.cpp)
target_link_libraries(eh_tests PRIVATE recomp_platform recomp_runtime_testing)
target_compile_options(eh_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(eh_tests 1)
pop_test_binary(eh_tests)
add_test(NAME eh_tests COMMAND eh_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(eh_tests PROPERTIES LABELS nogame TIMEOUT 120)

add_executable(runtime_tests tests/runtime_tests.cpp tests/stub_recomp_call.cpp)
target_link_libraries(runtime_tests PRIVATE recomp_platform recomp_runtime recomp_dx)
target_compile_options(runtime_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(runtime_tests 1)
pop_test_binary(runtime_tests)
add_test(NAME runtime_tests COMMAND runtime_tests WORKING_DIRECTORY ${POP_ROOT})
# TIMEOUT for the reason interp_tests has one: this suite also runs guest
# code through the interpreter. It takes two seconds when that returns.
set_tests_properties(runtime_tests PROPERTIES LABELS game TIMEOUT 600
                     ENVIRONMENT RECOMP_PYTHON=${Python3_EXECUTABLE})
# The multi-module loader, the guarded arena, strict mode and import argument
# counts, on synthetic PE images the suite writes itself: no game needed.
add_executable(loader_tests tests/loader_tests.cpp tests/stub_recomp_call.cpp)
target_link_libraries(loader_tests PRIVATE recomp_platform recomp_runtime)
target_compile_options(loader_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(loader_tests 1)
pop_test_binary(loader_tests)
add_test(NAME loader_tests COMMAND loader_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(loader_tests PROPERTIES LABELS nogame TIMEOUT 120)
add_executable(gdi_tests tests/gdi_tests.cpp tests/stub_recomp_call.cpp)
target_link_libraries(gdi_tests PRIVATE recomp_platform recomp_runtime)
target_compile_options(gdi_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(gdi_tests 1)
pop_test_binary(gdi_tests)
add_test(NAME gdi_tests COMMAND gdi_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(gdi_tests PROPERTIES LABELS nogame)
add_test(NAME gdi_model_tests COMMAND gdi_tests model WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(gdi_model_tests PROPERTIES LABELS nogame)
add_test(NAME gdi_draw_tests COMMAND gdi_tests draw WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(gdi_draw_tests PROPERTIES LABELS nogame)
# Text metrics against the numbers a Windows 96-dpi screen DC reports for the
# same requests (tools/gdi_metrics: capture_win32.c there, gdi_tests --capture
# here, compare.py's tolerances). Only once those numbers are committed, and
# only on macOS, which installs the Windows faces the cases name.
set(POP_GDI_WINDOWS_METRICS ${POP_ROOT}/runtime/tests/data/gdi_metrics_windows.jsonl)
if(APPLE AND EXISTS ${POP_GDI_WINDOWS_METRICS})
  add_test(NAME gdi_metrics_reference
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/gdi_metrics/compare.py
            ${POP_GDI_WINDOWS_METRICS} --gdi-tests $<TARGET_FILE:gdi_tests>
    WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(gdi_metrics_reference PROPERTIES LABELS nogame)
endif()
# The line metrics and face choice against a Windows 11 capture of the faces
# macOS also installs (tools/gdi_metrics/cases_windows11.txt says what is
# held and why), on macOS only.
if(APPLE)
  add_test(NAME gdi_metrics_windows11
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/gdi_metrics/compare.py
            ${POP_ROOT}/runtime/tests/data/gdi_metrics_windows11.jsonl
            --gdi-tests $<TARGET_FILE:gdi_tests>
            --cases ${POP_ROOT}/tools/gdi_metrics/cases_windows11.txt
            --only face,height,ascent,descent,internal,external,weight,italic,pitch_family,charset
    WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(gdi_metrics_windows11 PROPERTIES LABELS nogame)
  # Tahoma, the face a Windows interface of the time draws with: every
  # advance (U+007F apart) and string extent as well.
  add_test(NAME gdi_metrics_windows11_tahoma
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/gdi_metrics/compare.py
            ${POP_ROOT}/runtime/tests/data/gdi_metrics_windows11.jsonl
            --gdi-tests $<TARGET_FILE:gdi_tests>
            --cases ${POP_ROOT}/tools/gdi_metrics/cases_windows11.txt --faces Tahoma
            --only face,height,ascent,descent,internal,external,ave,weight,italic,pitch_family,charset,widths,extents
            --skip-chars 7f
    WORKING_DIRECTORY ${POP_ROOT})
  set_tests_properties(gdi_metrics_windows11_tahoma PROPERTIES LABELS nogame)
endif()
# Winsock (WSOCK32, WS2_32), the IP helper API and Netbios through the guest
# ABI, on real sockets on the loopback interface and on fake interface lists.
add_executable(winsock_tests tests/winsock_tests.cpp tests/stub_recomp_call.cpp)
target_link_libraries(winsock_tests PRIVATE recomp_platform recomp_runtime)
target_compile_options(winsock_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(winsock_tests 1)
pop_test_binary(winsock_tests)
add_test(NAME winsock_tests COMMAND winsock_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(winsock_tests PROPERTIES LABELS nogame TIMEOUT 120)
# The msvcrt.dll / msvcp60.dll HLE, against synthetic guest memory.
add_executable(crt_tests tests/crt_tests.cpp tests/stub_recomp_call.cpp)
target_link_libraries(crt_tests PRIVATE recomp_platform recomp_runtime)
target_compile_options(crt_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(crt_tests 1)
pop_test_binary(crt_tests)
add_test(NAME crt_tests COMMAND crt_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(crt_tests PROPERTIES LABELS nogame TIMEOUT 120)
# codepage_tables.h is gen_tables.py's output for the committed Windows
# capture (tools/codepages), which crt_tests also replays in full.
add_test(NAME codepage_tables_current
  COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/codepages/gen_tables.py --check
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(codepage_tables_current PROPERTIES LABELS nogame)

endif()

if(POP_REAL_GEN AND NOT IOS AND NOT ANDROID)
  # The headless parity fixture: null host, real generated code, core mods installed.
  add_executable(pop_fixture fixture.cpp)
  target_link_libraries(pop_fixture PRIVATE recomp_platform recomp_runtime recomp_snapshot recomp_dx_null recomp_mods lua)
  target_compile_options(pop_fixture PRIVATE ${POP_WARN_HOST})
  pop_optimize(pop_fixture 1)
  pop_link_gen(pop_fixture)
  if(TARGET core_mods)
    add_dependencies(pop_fixture core_mods)
  endif()

  # CALL_FN takes an address token (without 0x), not a numeric expression.
  # Select the first generated entry; define the suite only when one exists.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${POP_GEN_DIR}/funcs.h)
  file(STRINGS ${POP_GEN_DIR}/funcs.h PROFILE_FIRST_INDEX
    REGEX "^#define FIDX_[0-9a-f]+ 0u$" LIMIT_COUNT 1)
  if(PROFILE_FIRST_INDEX)
    add_executable(profile_tests tests/profile_tests.cpp)
    string(REGEX REPLACE "^#define FIDX_([0-9a-f]+) 0u$" "\\1"
      PROFILE_TARGET_TOKEN "${PROFILE_FIRST_INDEX}")
    target_compile_definitions(profile_tests PRIVATE RECOMP_PROFILE_TARGET=${PROFILE_TARGET_TOKEN})
    target_link_libraries(profile_tests PRIVATE recomp_platform recomp_runtime recomp_dx_null)
    target_compile_options(profile_tests PRIVATE ${POP_WARN_HOST})
    pop_optimize(profile_tests 1)
    pop_link_gen(profile_tests)
    pop_test_binary(profile_tests)
    add_test(NAME profile_tests_disabled COMMAND profile_tests disabled WORKING_DIRECTORY ${POP_ROOT})
    set_tests_properties(profile_tests_disabled PROPERTIES LABELS game ENVIRONMENT RECOMP_PROFILE=0)
    add_test(NAME profile_tests_enabled COMMAND profile_tests enabled WORKING_DIRECTORY ${POP_ROOT})
    set_tests_properties(profile_tests_enabled PROPERTIES LABELS game ENVIRONMENT RECOMP_PROFILE=1)
  else()
    message(STATUS "profile_tests: the translation has no FIDX entry; suite not defined")
  endif()
endif()

if(POP_TRACE_DIR AND NOT IOS AND NOT ANDROID)
  # Caller-supplied trace wrappers over a snapshot of gen/: the override header
  # is force-included ahead of funcs.h so every FN_<addr> macro is already set.
  file(GLOB POP_TRACE_SOURCES CONFIGURE_DEPENDS ${POP_TRACE_DIR}/gen/chunk_*.c ${POP_TRACE_DIR}/gen/table.c)
  add_library(recomp_gen_trace OBJECT ${POP_TRACE_SOURCES} ${POP_TRACE_DIR}/trace_wrappers.c)
  target_include_directories(recomp_gen_trace PRIVATE ${POP_TRACE_DIR} ${POP_TRACE_DIR}/gen ${POP_ROOT} ${POP_ROOT}/runtime)
  target_compile_options(recomp_gen_trace PRIVATE ${POP_WARN_GEN} -include ${POP_TRACE_DIR}/trace_override.h)
  pop_optimize(recomp_gen_trace 1)
  add_executable(pop_fixture_trace fixture.cpp)
  target_link_libraries(pop_fixture_trace PRIVATE recomp_platform recomp_runtime recomp_snapshot recomp_dx_null recomp_mods lua recomp_gen_trace)
  target_include_directories(pop_fixture_trace PRIVATE ${POP_TRACE_DIR}/gen)
  target_compile_options(pop_fixture_trace PRIVATE ${POP_WARN_HOST})
  pop_optimize(pop_fixture_trace 1)
endif()
