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

target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(app PRIVATE etl::etl)

# Same per-service iterable-section wiring the app itself needs for
# QcWifiWebTransport.hpp's HTTP_SERVICE_DEFINE(qcWebService, ...) - see
# ../../sections-rom.ld.
zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_qcWebService KVMA RAM_REGION GROUP RODATA_REGION)
