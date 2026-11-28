# Issue #99: turns docs/api/examples/**/*.json into a generated header of
# std::string_view constants, so Ztests can compare encoder output byte for
# byte with the documented examples (ADR 0012, decision 1). Test-only: the
# firmware image never includes it.
#
# qc_embed_api_examples(<target>) writes
# <CMAKE_CURRENT_BINARY_DIR>/generated/qc_api_examples_generated.hpp with one
# constant per file, named after its folder and file name:
#   examples/server/patch-current.json   -> kServerPatchCurrent
#   examples/invalid/client/foo-bar.json -> kInvalidClientFooBar
# The trailing newline each file ends with is not part of the message.
set(QC_API_EXAMPLES_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(qc_embed_api_examples target)
  set(examples_dir "${QC_API_EXAMPLES_MODULE_DIR}/../../../docs/api/examples")
  cmake_path(ABSOLUTE_PATH examples_dir NORMALIZE)
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/qc_api_examples_generated.hpp")

  file(GLOB_RECURSE example_files CONFIGURE_DEPENDS "${examples_dir}/*.json")
  list(SORT example_files)

  set(content "#pragma once\n\n// Generated from docs/api/examples by cmake/ApiExamples.cmake - do not edit.\n\n")
  string(APPEND content "#include <string_view>\n\nnamespace qcbridge::apiexamples {\n\n")
  foreach(file IN LISTS example_files)
    file(RELATIVE_PATH relative "${examples_dir}" "${file}")
    string(REGEX REPLACE "\\.json$" "" stem "${relative}")
    string(REGEX REPLACE "[/-]" ";" parts "${stem}")
    set(name "k")
    foreach(part IN LISTS parts)
      string(SUBSTRING "${part}" 0 1 first)
      string(SUBSTRING "${part}" 1 -1 rest)
      string(TOUPPER "${first}" first)
      string(APPEND name "${first}${rest}")
    endforeach()
    file(READ "${file}" text)
    string(REGEX REPLACE "\n$" "" text "${text}")
    string(APPEND content "// ${relative}\ninline constexpr std::string_view ${name} = R\"qcjson(${text})qcjson\";\n\n")
  endforeach()
  string(APPEND content "}  // namespace qcbridge::apiexamples\n")

  file(CONFIGURE OUTPUT "${generated}" CONTENT "${content}" @ONLY)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endfunction()
