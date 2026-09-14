# Firmware (NXP FRDM-RW612)

Target board: `frdm_rw612` (see [ADR 0002](../docs/adr/0002-devkit-selection.md) for why).
Zephyr, C++23 (see [CODING_STANDARDS.md](../CODING_STANDARDS.md)).

## Layout

```
firmware/
└── app/            # the actual Zephyr application (out-of-tree, "freestanding")
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/main.cpp
```

This repo does **not** vendor its own `west.yml`/manifest. `firmware/app` is a standard
out-of-tree Zephyr application that builds against a Zephyr workspace you set up separately
(the usual pattern for a monorepo that also holds non-firmware code, like `CODING_STANDARDS.md`
and the future `app/` JUCE client — a nested west manifest here would otherwise turn the
whole `west update` into siblings of this repo's *parent* directory, not a subfolder of it).
If that becomes annoying, revisit — this is a convention, not an ADR-level decision.

## One-time setup

Follow Zephyr's own [Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
to install the toolchain and SDK, then create a workspace pinned to the release this project
targets (v4.4.2, current stable as of writing — confirmed `frdm_rw612` and `CONFIG_STD_CPP23`
both exist in this release):

```bash
python3 -m venv ~/.venv/zephyr
source ~/.venv/zephyr/bin/activate
pip install west

west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.2 ~/zephyrproject
cd ~/zephyrproject
west update
west packages pip --install
west sdk install
```

BLE (for MVP2) needs NXP's binary blobs — fetch them now so they're not a surprise later:

```bash
west blobs fetch hal_nxp
```

## Build

```bash
west build -b frdm_rw612 /path/to/QCRemote/firmware/app --pristine
```

Drop `--pristine` on subsequent builds once the build directory exists.

## Flash

The FRDM-RW612's onboard debug probe supports both runners Zephyr knows about for this board:

```bash
west flash                    # tries the board's default runner
west flash --runner jlink     # explicit J-Link
west flash --runner linkserver  # explicit NXP LinkServer (install LinkServer separately)
```

## Verify

Console output (`zephyr,console` is UART, `flexcomm3`) should show:

```
QC Bridge firmware skeleton up (C++202302)
```

and the onboard green LED should blink at ~1Hz. This confirms the toolchain, board target,
and C++23 configuration all work end-to-end before any real protocol code lands (issues
[#3](https://github.com/rrooding/QCRemote/issues/3) onward).

## Not yet done

- Hardware-in-the-loop verification of the above (needs the physical board) — this repo
  content is unverified against real hardware; flag any discrepancy found when you run it.
- ETL is wired in via CMake `FetchContent` (see `app/CMakeLists.txt`) but not yet used by
  any code.
