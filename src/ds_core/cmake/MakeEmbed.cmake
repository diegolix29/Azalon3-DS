# melonDS's OpenGL_shaders/CMakeLists.txt runs ${CMAKE_SOURCE_DIR}/cmake/MakeEmbed.cmake,
# which is this project's folder when melonDS is built as a subdirectory. Forward to
# melonDS's own script (script mode: INPUT_FILE / OUTPUT_FILE / VAR_NAME are passed with -D).
include("${CMAKE_CURRENT_LIST_DIR}/../../../externals/melonDS/cmake/MakeEmbed.cmake")
