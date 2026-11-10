cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wifi_web_http_tests)

include(FetchContent)
FetchContent_Declare(
  etl
  GIT_REPOSITORY https://github.com/ETLCPP/etl.git
  GIT_TAG        20.49.0
)
FetchContent_MakeAvailable(etl)

# Issue #88: this suite's own tests inflate a served asset with
# QcGzip.hpp (to confirm the embed step's round trip, not just its
# headers) - same miniz wiring as ../../CMakeLists.txt and
# ../gzip_inflate/CMakeLists.txt; see either for why only miniz_tinfl.c.
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
  MINIZ_NO_DEFLATE_APIS
  MINIZ_NO_ARCHIVE_APIS
  MINIZ_NO_ZLIB_APIS
)
target_link_libraries(app PRIVATE etl::etl)

# Same per-service iterable-section wiring the app itself needs for
# QcWifiWebTransport.hpp's HTTP_SERVICE_DEFINE(qcWebService, ...) - see
# ../../sections-rom.ld.
zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_qcWebService KVMA RAM_REGION GROUP RODATA_REGION)

# Issue #88 / ADR 0011: this suite links QcWifiWebTransport.hpp too, which
# #includes the generated web asset header unconditionally - see
# ../../cmake/EmbedWebAssets.cmake.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/EmbedWebAssets.cmake)
qc_embed_web_assets(app)
