# cmake -DSRC=<built launcher> -DDST=<copy> -P copy_launcher.cmake
# Like "cmake -E copy_if_different", but a destination that is in use is left as it is instead of failing the
# build: the launcher's own Setup page runs the build while that launcher is running from DST.
file(COPY_FILE "${SRC}" "${DST}" ONLY_IF_DIFFERENT RESULT result)
if(result)
  message(STATUS "Launcher not updated at ${DST} (it is probably running): ${result}")
endif()
