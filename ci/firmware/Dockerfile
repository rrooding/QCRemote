# Firmware CI/dev image for issue #46: a pinned Zephyr v4.5.0 workspace (the
# same narrowed `west update` project list as .github/workflows/firmware.yml,
# issue #86) plus the two toolchains this project's 3 boards actually need,
# baked in so CI and local builds reinstall nothing per run.
#
# Built and pushed by .github/workflows/publish-firmware-ci-image.yml, never
# by firmware.yml itself - rebuild only when ZEPHYR_VERSION/TOOLCHAINS below
# change, or the image drifts from what firmware.yml expects.
FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852

ARG DEBIAN_FRONTEND=noninteractive
ARG ZEPHYR_VERSION=v4.5.0

# Same trimmed package list as firmware.yml's "Install Zephyr host
# dependencies" step (issue #86), plus two packages that list dropped because
# ubuntu-latest runners already carry them: ca-certificates (needed for the
# https clones/fetches below, confirmed missing on this bare base image) and
# wget (confirmed required - not by `west sdk install`'s own download, which
# is pure Python, but by the Zephyr SDK's bundled setup.sh, which shells out
# to wget and fails with "Zephyr SDK setup requires 'wget'" without it).
RUN apt-get update \
    && apt-get install --no-install-recommends -y \
         ca-certificates wget git cmake ninja-build gperf ccache device-tree-compiler \
         python3-dev python3-venv python3-pip python3-setuptools python3-wheel \
         xz-utils file make gcc gcc-multilib g++-multilib libmagic1 \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04's system pip refuses installs outside a venv (PEP 668); a venv
# also matches firmware/README.md's local one-time setup instructions.
RUN python3 -m venv /opt/venv
ENV PATH="/opt/venv/bin:${PATH}"
RUN pip install --no-cache-dir west

ENV ZEPHYR_WORKSPACE=/opt/zephyrproject

# Same narrowed project list as firmware.yml's "Update Zephyr workspace and
# SDK" step (issue #86) - keep both in sync if it ever changes.
RUN west init -m https://github.com/zephyrproject-rtos/zephyr --mr "${ZEPHYR_VERSION}" "${ZEPHYR_WORKSPACE}" \
    && cd "${ZEPHYR_WORKSPACE}" \
    && west update --narrow -o=--depth=1 \
         hal_nxp hal_espressif hal_xtensa cmsis cmsis_6 \
         mbedtls tf-psa-crypto tf-psa-crypto-drivers picolibc \
    && west packages pip --install --ignore-venv-check \
    && west sdk install --install-base /opt --gnu-toolchains arm-zephyr-eabi xtensa-espressif_esp32s3_zephyr-elf

# Issue #71's Wi-Fi/BLE radio firmware blobs - same --allow-regex scope as
# firmware.yml's "Fetch Wi-Fi/BLE radio firmware blobs" step (see that
# step's comment for why: hal_nxp's full blob list needs an interactive
# license click-through this regex avoids).
RUN cd "${ZEPHYR_WORKSPACE}" \
    && west blobs fetch hal_nxp --allow-regex '^rw61x/rw61x_sb_(wifi|ble)_a2'

# Issue #76/ADR 0009's ESP32-S3 DWC2 patch - applied once here instead of
# firmware.yml's per-run "already applied?" check, since this image's
# workspace never changes between builds.
COPY firmware/patches/zephyr/0001-uhc_dwc2-start-late-periodic-channels.patch /tmp/dwc2.patch
RUN git -C "${ZEPHYR_WORKSPACE}/zephyr" apply /tmp/dwc2.patch \
    && rm /tmp/dwc2.patch

WORKDIR ${ZEPHYR_WORKSPACE}
