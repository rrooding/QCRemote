cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gzip_inflate_tests)

include(FetchContent)
FetchContent_Declare(
  etl
  GIT_REPOSITORY https://github.com/ETLCPP/etl.git
  GIT_TAG        20.49.0
)
FetchContent_MakeAvailable(etl)

# See ../../CMakeLists.txt for why this fetches miniz's source tree
# without using its own CMakeLists.txt, and why only miniz_tinfl.c.
FetchContent_Declare(
  miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG        3.0.2
)
FetchContent_Populate(miniz)

target_sources(app PRIVATE
  src/main.cpp
  ${miniz_SOURCE_DIR}/miniz_tinfl.c
)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src
  ${miniz_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/miniz
)
target_compile_definitions(app PRIVATE
  MINIZ_NO_STDIO
  MINIZ_NO_MALLOC
  MINIZ_NO_TIME
  # See ../../CMakeLists.txt for why MINIZ_NO_ARCHIVE_WRITING_APIS isn't
  # also listed here.
  MINIZ_NO_DEFLATE_APIS
  MINIZ_NO_ARCHIVE_APIS
  MINIZ_NO_ZLIB_APIS
)
target_link_libraries(app PRIVATE etl::etl)
