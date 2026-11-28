# Issue #88 / ADR 0011. Shared by firmware/app/CMakeLists.txt and
# firmware/app/tests/wifi_web_http/CMakeLists.txt: both link
# QcWifiWebTransport.hpp, which #includes the generated header below
# unconditionally - keeping the embed step in one place means the real
# build and the Ztest suite that exercises it can never drift apart.
#
# qc_embed_web_assets(<target>) runs the build-time asset pipeline
# (firmware/app/tools/embed_assets.py, over web/ui) into
# <CMAKE_CURRENT_BINARY_DIR>/generated/qc_web_assets_generated.hpp (and the
# build id, issue #99, into qc_build_id_generated.hpp next to it), adds
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
  set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(generated "${generated_dir}/qc_web_assets_generated.hpp")
  set(build_id "${generated_dir}/qc_build_id_generated.hpp")

  # Issue #99: runs on every build, not only when a file under web/ui
  # changes, because the build id it stamps into the page and the firmware
  # (qc_build_id_generated.hpp, web/ui/src/build.js) is the git commit
  # (plus -dirty), and a new commit or an edit elsewhere touches nothing here. The script only rewrites a header
  # whose content changed, and BYPRODUCTS lets Ninja see that (restat), so
  # an unchanged build recompiles nothing.
  add_custom_target(${target}_qc_web_assets
    COMMAND ${PYTHON_EXECUTABLE} "${script}" --ui-dir "${ui_dir}" --out "${generated}"
            --build-id-out "${build_id}"
    BYPRODUCTS "${generated}" "${build_id}"
    COMMENT "Embedding web/ui assets as gzipped C++ arrays (ADR 0011) and stamping the build id"
  )
  add_dependencies(${target} ${target}_qc_web_assets)
  target_include_directories(${target} PRIVATE "${generated_dir}")
endfunction()
