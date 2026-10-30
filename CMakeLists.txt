# recomp-launcher: settings window and launcher for statically recompiled Xbox / Xbox 360 games.
#
# Standalone:   cmake -B build && cmake --build build --config Release
# In a game:    add_subdirectory(launcher)
#               recomp_add_launcher(GAME_TARGET mygame OUTPUT_NAME mygame_launcher [COPY_TO_SOURCE_DIR])
cmake_minimum_required(VERSION 3.21)
project(recomp_launcher LANGUAGES CXX)

if(NOT WIN32)
  message(STATUS "recomp-launcher: Windows only, skipped")
  function(recomp_add_launcher)
  endfunction()
  return()
endif()

# CMake hands .manifest sources to the linker (/MANIFESTINPUT): common controls v6 and per-monitor DPI.
add_executable(recomp_launcher WIN32 src/main.cpp src/launcher.manifest)
target_compile_features(recomp_launcher PRIVATE cxx_std_17)
target_compile_definitions(recomp_launcher PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(recomp_launcher PRIVATE comctl32 shell32 user32 gdi32 advapi32 ole32 uuid bcrypt)
if(MSVC)
  target_compile_options(recomp_launcher PRIVATE /W3 /utf-8)
endif()
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  # Static CRT: the launcher is a single self-contained exe.
  set_property(TARGET recomp_launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  # MinGW
  target_link_options(recomp_launcher PRIVATE -municode -static)
endif()

set(RECOMP_LAUNCHER_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")

# Copies the launcher next to GAME_TARGET's executable as <OUTPUT_NAME>.exe on every build, and with
# COPY_TO_SOURCE_DIR also to the calling project's source directory (next to its launcher.ini).
function(recomp_add_launcher)
  cmake_parse_arguments(ARG "COPY_TO_SOURCE_DIR" "GAME_TARGET;OUTPUT_NAME" "" ${ARGN})
  if(NOT ARG_GAME_TARGET)
    message(FATAL_ERROR "recomp_add_launcher: GAME_TARGET is required")
  endif()
  if(NOT ARG_OUTPUT_NAME)
    set(ARG_OUTPUT_NAME launcher)
  endif()
  set(copy ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE:recomp_launcher>)
  set(script -P ${RECOMP_LAUNCHER_DIR}/cmake/copy_launcher.cmake)
  set(cmds COMMAND ${copy} -DDST=$<TARGET_FILE_DIR:${ARG_GAME_TARGET}>/${ARG_OUTPUT_NAME}.exe ${script})
  if(ARG_COPY_TO_SOURCE_DIR)
    list(APPEND cmds COMMAND ${copy} -DDST=${CMAKE_CURRENT_SOURCE_DIR}/${ARG_OUTPUT_NAME}.exe ${script})
  endif()
  add_custom_target(${ARG_GAME_TARGET}_launcher ALL ${cmds}
                    COMMENT "Copying launcher to ${ARG_OUTPUT_NAME}.exe" VERBATIM)
  add_dependencies(${ARG_GAME_TARGET}_launcher recomp_launcher ${ARG_GAME_TARGET})
endfunction()
