cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(qc_bridge_firmware)

# ETL (https://github.com/ETLCPP/etl) - fixed-capacity containers, no heap
# allocation. See CODING_STANDARDS.md's Firmware/Memory & containers section.
include(FetchContent)
FetchContent_Declare(
  etl
  GIT_REPOSITORY https://github.com/ETLCPP/etl.git
  GIT_TAG        20.49.0
)
FetchContent_MakeAvailable(etl)

# subsys/usb/host's own headers (usbh_class.h, usbh_desc.h, ...) aren't part
# of Zephyr's public API and aren't exported by its build - every USB host
# class driver, in-tree or not, needs this same path. See QcHidBridge.hpp.
zephyr_include_directories(${ZEPHYR_BASE}/subsys/usb/host)

target_sources(app PRIVATE src/Main.cpp)
target_link_libraries(app PRIVATE etl::etl)
