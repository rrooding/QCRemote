---
name: board-smoke-test
description: Build, flash and smoke-test QCRemote firmware on the real FRDM-RW612 Board. Use when asked to flash the Board, run the firmware on hardware, or check a change against the Board's web page or /ws WebSocket.
---

# Board smoke test

Each step ends on the result it names. Stop and report on the first one that doesn't appear.

## 1. Put the Wi-Fi config in the worktree

`firmware/app/wifi_credentials.conf` is gitignored, so a fresh worktree lacks it. The kept copy lives in the main checkout:

```sh
cp -n /Users/ralph/Code/QCRemote/firmware/app/wifi_credentials.conf firmware/app/
```

It holds the network passphrase: copy it, and never print, cat, grep or commit it.
**Done when** the file exists in this worktree.

## 2. Build

The Zephyr workspace is `~/zephyrproject` (v4.5.0) and its venv is `~/.venv/zephyr`. Builds take a few minutes, so run this in the background:

```sh
source ~/.venv/zephyr/bin/activate && cd ~/zephyrproject && \
  west build -b frdm_rw612 <worktree>/firmware/app -d build-<branch-slug> --pristine
```

The build id baked into the page and firmware is `git describe --always --dirty --abbrev=7 --exclude=*` of the worktree. Commit first if you want a clean id.
**Done when** the build ends with the memory-usage table and no error. Note the RAM line (it was 77.6 % at `538a68b`), since RAM is the scarce budget.

## 3. Flash

```sh
source ~/.venv/zephyr/bin/activate && cd ~/zephyrproject && west flash -d build-<branch-slug>
```

The onboard probe is J-Link, so the default `jlink` runner is correct. A serial console the user has open (`tio`) doesn't block flashing.
**Done when** `west flash` exits 0 with no `ERROR` line.

## 4. Find the Board's IP

The console prints `Startup summary: mode=STA iface=up ip=<ip> ... url=http://<ip>/` after it joins Wi-Fi. Read it from the user's `tio` log file if they run one, otherwise ask the user for the IP. The last known address was `192.168.1.228` (a DHCP lease, so check it with step 5 before trusting it).
**Done when** `curl -sI http://<ip>/` answers.

## 5. Check HTTP

```sh
curl -sI http://<ip>/ ; curl -s --compressed http://<ip>/src/build.js ; curl -so /dev/null -w '%{http_code}\n' http://<ip>/api/presets.json
```

**Done when** all of these hold:
- `/` is `200` with `Content-Encoding: gzip` and `Cache-Control: no-cache`.
- `build.js` carries the step-2 build id.
- `/api/presets.json` is `404`, since it was removed in favour of `/ws`.

## 6. Smoke-test the WebSocket

```sh
python3 firmware/tools/ws_probe.py <ip> --smoke --expect-build <build-id>
```

This checks the following:
- the snapshot at seq 0
- `badRequest` for an unknown type
- a fresh snapshot on `resync`
- a tick within 7 s
- no seq gaps

The script falls back to `/usr/bin/nc` when macOS Local Network privacy blocks Python, so a `No route to host` from Python alone isn't a Board fault.
**Done when** the script prints `smoke test: all passed` and exits 0.

## 7. Live check with the user (when the change touches Presets or the Link)

Start `python3 firmware/tools/ws_probe.py <ip> --listen 60` in the background, then ask the user to change Presets on the Quad Cortex.
**Done when** each change shows up as a `patch` with the new `current`, and seq has no gaps. For page changes, also ask the user to open `http://<ip>/` in a browser and confirm what they see.

Report each step's result, including the build id and RAM figure.
