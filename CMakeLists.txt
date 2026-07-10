cmake_minimum_required(VERSION 3.20)
project(tmuxw VERSION 0.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Statically link the CRT so the released tmuxw.exe runs with no VC++ redist.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT WIN32)
  message(FATAL_ERROR "tmuxw is a native Windows application (requires ConPTY, Win10 1809+).")
endif()

# Target Windows 10 1809 — minimum for the ConPTY Pseudo Console API.
# NTDDI_WIN10_RS5 (0x0A000006) gates PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE.
add_compile_definitions(
  _WIN32_WINNT=0x0A00
  WINVER=0x0A00
  NTDDI_VERSION=0x0A000006
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
  TMUXW_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
endif()

# --- vendored libvterm (VT parser + screen grid) -----------------------------
set(LIBVTERM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libvterm)
if(NOT EXISTS ${LIBVTERM_DIR}/src/vterm.c)
  message(FATAL_ERROR
    "vendor/libvterm is missing. Run: git submodule update --init --recursive")
endif()

add_library(vterm STATIC
  ${LIBVTERM_DIR}/src/encoding.c
  ${LIBVTERM_DIR}/src/keyboard.c
  ${LIBVTERM_DIR}/src/mouse.c
  ${LIBVTERM_DIR}/src/parser.c
  ${LIBVTERM_DIR}/src/pen.c
  ${LIBVTERM_DIR}/src/screen.c
  ${LIBVTERM_DIR}/src/state.c
  ${LIBVTERM_DIR}/src/unicode.c
  ${LIBVTERM_DIR}/src/vterm.c
)
target_include_directories(vterm
  PUBLIC  ${LIBVTERM_DIR}/include
  PRIVATE ${LIBVTERM_DIR}/src
)
# libvterm is third-party: silence its warnings and CRT deprecations.
target_compile_definitions(vterm PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(vterm PRIVATE /w)
endif()

# --- tmuxw -------------------------------------------------------------------
add_executable(tmuxw
  src/main.c
  src/platform/conpty.c
  src/platform/winterm.c
  src/platform/ipc.c
  src/platform/clipboard.c
  src/util/strbuf.c
  src/emu/screen.c
  src/model/pane.c
  src/model/window.c
  src/layout.c
  src/render.c
  src/copymode.c
  src/status.c
  src/cmd.c
  src/session.c
  src/server.c
  src/client.c
)
target_include_directories(tmuxw PRIVATE src)
target_link_libraries(tmuxw PRIVATE vterm)

# Also expose the familiar `tmux` command as a copy of the same binary, so
# `tmux new` works out of the box alongside `tmuxw`.
add_custom_command(TARGET tmuxw POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:tmuxw> $<TARGET_FILE_DIR:tmuxw>/tmux.exe
  COMMENT "Creating tmux.exe alias for tmuxw.exe"
  VERBATIM)

# --- tests -------------------------------------------------------------------
enable_testing()

add_executable(test_emu
  tests/test_emu.c
  src/util/strbuf.c
  src/emu/screen.c
  src/model/pane.c
  src/platform/conpty.c
  src/render.c
  src/copymode.c
)
target_include_directories(test_emu PRIVATE src)
target_link_libraries(test_emu PRIVATE vterm)
add_test(NAME emu COMMAND test_emu)

add_executable(test_copymode
  tests/test_copymode.c
  src/copymode.c
  src/emu/screen.c
  src/model/pane.c
  src/platform/conpty.c
  src/util/strbuf.c
)
target_include_directories(test_copymode PRIVATE src)
target_link_libraries(test_copymode PRIVATE vterm)
add_test(NAME copymode COMMAND test_copymode)

add_executable(test_layout
  tests/test_layout.c
  src/layout.c
  src/model/pane.c
  src/platform/conpty.c
  src/util/strbuf.c
  src/emu/screen.c
)
target_include_directories(test_layout PRIVATE src)
target_link_libraries(test_layout PRIVATE vterm)
add_test(NAME layout COMMAND test_layout)

add_executable(test_ipc
  tests/test_ipc.c
  src/platform/ipc.c
  src/util/strbuf.c
)
target_include_directories(test_ipc PRIVATE src)
add_test(NAME ipc COMMAND test_ipc)

add_executable(test_status
  tests/test_status.c
  src/status.c
  src/util/strbuf.c
)
target_include_directories(test_status PRIVATE src)
add_test(NAME status COMMAND test_status)

add_executable(test_cmd
  tests/test_cmd.c
  src/cmd.c
)
target_include_directories(test_cmd PRIVATE src)
add_test(NAME cmd COMMAND test_cmd)
