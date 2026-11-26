cmake_minimum_required(VERSION 3.20)
project(cocaine LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/viewer/clipboard_guest.py")
file(READ "${CMAKE_SOURCE_DIR}/src/viewer/clipboard_guest.py" clipboard_guest)
file(WRITE "${CMAKE_BINARY_DIR}/clipboard_guest.inc"
  "R\"COCAINE_GUEST(${clipboard_guest})COCAINE_GUEST\"")

add_executable(cocaine
  src/main.cpp
  src/log.cpp
  src/mem/guest_memory.cpp
  src/macho/macho.cpp
  src/cpu/ppc/ppc_interp.cpp
  src/cpu/ppc/ppc_jit.cpp
  src/cpu/ppc/altivec.cpp
  src/cpu/x86/x86_interp.cpp
  src/cpu/x86/x86_jit.cpp
  src/cpu/x86/f80.cpp
  src/cpu/x86/x87.cpp
  src/cpu/x86/sse.cpp
  src/kernel/process.cpp
  src/kernel/syscalls.cpp
  src/kernel/sysctl.cpp
  src/kernel/mach.cpp
  src/kernel/commpage.cpp
  src/kernel/paths.cpp
  src/kernel/files.cpp
  src/kernel/sockets.cpp
  src/kernel/ipc.cpp
  src/kernel/iokit.cpp
  src/session/session.cpp
  src/session/pacing.cpp
  src/session/art.cpp
  src/viewer/viewer.mm
  src/viewer/clipboard.mm
  src/kernel/threads.cpp
  src/kernel/processes.cpp
  src/kernel/signals.cpp
  src/kernel/protect.cpp
  src/kernel/hooks.cpp
  src/kernel/wallpaper.cpp
  src/gl/gl.cpp
  src/al/al.cpp
  src/audio/coreaudio.cpp
  src/audio/midi.cpp
)

target_include_directories(cocaine PRIVATE src "${CMAKE_BINARY_DIR}")
target_compile_options(cocaine PRIVATE
  -Wall -Wextra -Wno-unused-parameter
  -fno-exceptions -fno-rtti -fno-strict-aliasing
)

# The CPU cores do guest floating point with host floating point, so the
# compiler must not fuse multiply-adds or assume the default rounding mode.
set_source_files_properties(
  src/cpu/ppc/ppc_interp.cpp
  src/cpu/ppc/altivec.cpp
  src/cpu/x86/x86_interp.cpp
  src/cpu/x86/x87.cpp
  src/cpu/x86/sse.cpp
  PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-frounding-math")

set_source_files_properties(src/viewer/viewer.mm src/viewer/clipboard.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(cocaine PRIVATE "-framework CoreFoundation" "-framework IOKit" "-framework Cocoa" "-framework QuartzCore" "-framework CoreGraphics" "-framework ImageIO" "-framework OpenGL" "-framework OpenAL" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio")

enable_testing()
add_executable(guest-memory-test tests/guest-memory.cpp src/mem/guest_memory.cpp)
target_include_directories(guest-memory-test PRIVATE src)
add_test(NAME guest-memory COMMAND guest-memory-test)
add_executable(mach-message-test tests/mach-message.cpp)
target_include_directories(mach-message-test PRIVATE src)
add_test(NAME mach-message COMMAND mach-message-test)
add_executable(display-pixels-test tests/display-pixels.cpp)
target_include_directories(display-pixels-test PRIVATE src)
add_test(NAME display-pixels COMMAND display-pixels-test)
add_executable(ui-scale-test tests/ui-scale.cpp)
target_include_directories(ui-scale-test PRIVATE src)
add_test(NAME ui-scale COMMAND ui-scale-test)
add_executable(input-notify-test tests/input-notify.cpp)
target_include_directories(input-notify-test PRIVATE src)
add_test(NAME input-notify COMMAND input-notify-test)
add_executable(vertex-copy-test tests/vertex-copy.cpp)
target_include_directories(vertex-copy-test PRIVATE src)
add_test(NAME vertex-copy COMMAND vertex-copy-test)
add_executable(ppc-fpscr-test tests/ppc-fpscr.cpp src/cpu/ppc/ppc_interp.cpp
  src/cpu/ppc/ppc_jit.cpp src/cpu/ppc/altivec.cpp src/log.cpp)
target_include_directories(ppc-fpscr-test PRIVATE src)
add_test(NAME ppc-fpscr COMMAND ppc-fpscr-test)
set_tests_properties(ppc-fpscr PROPERTIES ENVIRONMENT "COCAINE_JIT=1")
add_executable(viewer-resize-test tests/viewer-resize.mm src/viewer/clipboard.mm src/session/pacing.cpp src/log.cpp)
target_include_directories(viewer-resize-test PRIVATE src "${CMAKE_BINARY_DIR}")
set_source_files_properties(tests/viewer-resize.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(viewer-resize-test PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework CoreGraphics" "-framework ImageIO")
add_test(NAME viewer-resize COMMAND viewer-resize-test)
set_tests_properties(viewer-resize PROPERTIES SKIP_RETURN_CODE 77)
add_executable(clipboard-test tests/clipboard.mm src/viewer/clipboard.mm)
target_include_directories(clipboard-test PRIVATE src "${CMAKE_BINARY_DIR}")
set_source_files_properties(tests/clipboard.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(clipboard-test PRIVATE "-framework Cocoa" "-framework ImageIO")
add_test(NAME clipboard-bridge COMMAND clipboard-test $<TARGET_FILE:cocaine>
  "${CMAKE_SOURCE_DIR}/sysroots/10.5" "${CMAKE_SOURCE_DIR}/src/viewer/clipboard_guest.py")
set_tests_properties(clipboard-bridge PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
  ENVIRONMENT "COCAINE_NO_VIEWER=1")
add_test(NAME preview-clipboard COMMAND clipboard-test $<TARGET_FILE:cocaine>
  "${CMAKE_SOURCE_DIR}/sysroots/10.5" --preview)
set_tests_properties(preview-clipboard PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 160)
# The ARM64 instruction encodings the PowerPC translator emits, against the
# system's assembler.
add_test(NAME arm64-encodings
  COMMAND sh ${CMAKE_SOURCE_DIR}/tests/a64-check.sh ${CMAKE_BINARY_DIR}/a64-work)
add_test(NAME guest-programs
  COMMAND sh ${CMAKE_SOURCE_DIR}/tests/run.sh $<TARGET_FILE:cocaine> ${CMAKE_BINARY_DIR}/test-work)
add_test(NAME guest-network
  COMMAND sh ${CMAKE_SOURCE_DIR}/tests/network.sh $<TARGET_FILE:cocaine>)
set_tests_properties(guest-network PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME guest-pasteboard
  COMMAND sh ${CMAKE_SOURCE_DIR}/tests/pasteboard.sh $<TARGET_FILE:cocaine>)
set_tests_properties(guest-pasteboard PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
