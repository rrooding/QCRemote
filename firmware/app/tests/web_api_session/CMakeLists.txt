cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(web_api_session_tests)

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

# Issue #99: the contract's example messages (docs/api/examples) are this
# suite's expected values, byte for byte. They are read straight from the
# docs at build time rather than copied into the test, so a change to an
# example breaks this suite in the same commit.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ApiExamples.cmake)
qc_embed_api_examples(app)
