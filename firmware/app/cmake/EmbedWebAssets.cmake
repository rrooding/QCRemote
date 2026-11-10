# Issue #88 / ADR 0011. Shared by firmware/app/CMakeLists.txt and
# firmware/app/tests/wifi_web_http/CMakeLists.txt: both link
# QcWifiWebTransport.hpp, which #includes the generated header below
# unconditionally - keeping the embed step in one place means the real
# build and the Ztest suite that exercises it can never drift apart.
#
# qc_embed_web_assets(<target>) runs the build-time asset pipeline
# (firmware/app/tools/embed_assets.py, over web/ui) into
# <CMAKE_CURRENT_BINARY_DIR>/generated/qc_web_assets_generated.hpp, adds
# that directory to <target>'s include path, and makes <target> depend on
# the generation step - so editing a file under web/ui needs no firmware
# code change at all, just a rebuild.
#
# CMAKE_CURRENT_LIST_DIR inside a function() reflects whichever listfile is
# *calling* the function, not the file the function was defined in (a
# function call doesn't change "the file currently being processed" the
# way include() does) - so this module's own directory is captured once at
# include() time, outside the function, where CMAKE_CURRENT_LIST_DIR really
# is this file's directory.
set(QC_EMBED_WEB_ASSETS_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(qc_embed_web_assets target)
  set(ui_dir "${QC_EMBED_WEB_ASSETS_MODULE_DIR}/../../../web/ui")
  cmake_path(ABSOLUTE_PATH ui_dir NORMALIZE)
  set(script "${QC_EMBED_WEB_ASSETS_MODULE_DIR}/../tools/embed_assets.py")
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/qc_web_assets_generated.hpp")

  # A conservative rebuild trigger (anything under web/ui, not just the
  # files embed_assets.py actually serves) - correct is more important than
  # minimal here, and CONFIGURE_DEPENDS means a new/removed file is picked
  # up on the next build without re-running cmake by hand.
  file(GLOB_RECURSE ui_files CONFIGURE_DEPENDS "${ui_dir}/*")

  add_custom_command(
    OUTPUT "${generated}"
    COMMAND ${PYTHON_EXECUTABLE} "${script}" --ui-dir "${ui_dir}" --out "${generated}"
    DEPENDS "${script}" ${ui_files}
    COMMENT "Embedding web/ui assets as gzipped C++ arrays (ADR 0011)"
  )
  add_custom_target(${target}_qc_web_assets DEPENDS "${generated}")
  add_dependencies(${target} ${target}_qc_web_assets)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endfunction()
