#pragma once

// miniz's own headers (vendored source only, see ../../CMakeLists.txt's
// FetchContent_Populate(miniz) - we don't use miniz's CMakeLists.txt,
// which would normally run CMake's generate_export_header() to produce
// this file for a shared-library build) #include "miniz_export.h"
// unconditionally. We only ever build miniz_tinfl.c into a static
// archive linked straight into the firmware image, so there is no
// shared-library boundary to export symbols across - MINIZ_EXPORT can
// just expand to nothing.
#define MINIZ_EXPORT
