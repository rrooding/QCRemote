#!/usr/bin/env python3
"""Talk to a running Board's /ws WebSocket from the Mac, standard library only.

    python3 firmware/tools/ws_probe.py <board-ip> --smoke     # contract smoke test, exit 1 on failure
    python3 firmware/tools/ws_probe.py <board-ip> --listen 60 # print every message for 60 s
    python3 firmware/tools/ws_probe.py <board-ip> --send '{"t":"resync"}' --listen 5

The transport is a direct socket when this Python may reach the LAN, else `/usr/bin/nc`
(macOS Local Network privacy often blocks Python and Node but lets Apple's nc and curl
through; `--nc` forces it). Messages are printed one per line, with the Catalog
summarised. The contract itself is docs/api/README.md.
"""

import argparse
import base64
import json
import os
import queue
import socket
import struct
import subprocess
import sys
import threading
import time


def handshake(host):
    key = base64.b64encode(os.urandom(16)).decode()
    return (f"GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode()


def text_frame(text):
    payload, mask = text.encode(), os.urandom(4)
    if len(payload) > 125:
        sys.exit("ws_probe: frames over 125 B aren't supported (the Board takes 256 B at most anyway)")
    return bytes([0x81, 0x80 | len(payload)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


class Link:
    """Bytes to and from the Board, over a socket or an nc child process."""

    def __init__(self, host, port, force_nc):
        self.sock = self.proc = None
        if not force_nc:
            try:
                self.sock = socket.create_connection((host, port), timeout=3)
                self.sock.settimeout(None)
                self.via = "socket"
                return
            except OSError:
                pass
        self.proc = subprocess.Popen(["/usr/bin/nc", host, str(port)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.via = "nc"

    def write(self, data):
        if self.sock:
            self.sock.sendall(data)
        else:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()

    def read(self):
        return self.sock.recv(4096) if self.sock else os.read(self.proc.stdout.fileno(), 4096)

    def close(self):
        if self.sock:
            self.sock.close()
        else:
            self.proc.kill()


def split_frame(buf):
    """(opcode, payload, rest) for the unmasked server frame at the start of `buf`, or None
    while it is incomplete."""
    if len(buf) < 2:
        return None
    ln, off = buf[1] & 127, 2
    if ln == 126:
        if len(buf) < 4:
            return None
        ln, off = struct.unpack(">H", buf[2:4])[0], 4
    elif ln == 127:
        if len(buf) < 10:
            return None
        ln, off = struct.unpack(">Q", buf[2:10])[0], 10
    if len(buf) < off + ln:
        return None
    return buf[0] & 15, buf[off:off + ln], buf[off + ln:]


def reader(link, out):
    """Puts the status line, then ('text', dict) / ('close', code) / ('eof', None) on `out`."""
    buf = b""
    try:
        while b"\r\n\r\n" not in buf:
            chunk = link.read()
            if not chunk:
                out.put(("eof", None))
                return
            buf += chunk
        head, buf = buf.split(b"\r\n\r\n", 1)
        out.put(("status", head.split(b"\r\n")[0].decode()))
        while True:
            while (frame := split_frame(buf)) is None:
                chunk = link.read()
                if not chunk:
                    out.put(("eof", None))
                    return
                buf += chunk
            op, payload, buf = frame
            if op == 8:
                out.put(("close", struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else None))
            elif op == 1:
                out.put(("text", json.loads(payload)))
    except (OSError, ValueError) as e:
        out.put(("eof", str(e)))


def show(kind, value):
    if kind == "text":
        m = dict(value)
        if "catalog" in m:
            c = m["catalog"]
            m["catalog"] = (f"generation {c['generation']}, {c['presetCount']} presets in "
                            f"{len(c['folders'])} folders, incomplete={c['incomplete']}")
        print("<-", json.dumps(m, ensure_ascii=False), flush=True)
    elif kind == "close":
        print("<- close frame, code", value, flush=True)
    elif kind == "status":
        print(value, flush=True)
    else:
        print("<- connection ended", value or "", flush=True)


class Session:
    def __init__(self, args):
        self.link = Link(args.host, args.port, args.nc)
        self.inbox = queue.Queue()
        self.link.write(handshake(args.host))
        threading.Thread(target=reader, args=(self.link, self.inbox), daemon=True).start()
        print(f"connected to {args.host}:{args.port} via {self.link.via}", flush=True)

    def send(self, text):
        print("->", text, flush=True)
        self.link.write(text_frame(text))

    def next(self, timeout):
        """The next event, or None after `timeout` seconds."""
        try:
            event = self.inbox.get(timeout=timeout)
        except queue.Empty:
            return None
        show(*event)
        return event

    def drain(self, seconds):
        end = time.monotonic() + seconds
        while (left := end - time.monotonic()) > 0:
            if (e := self.next(left)) and e[0] in ("close", "eof"):
                return


def smoke(s, expect_build):
    """Walks the contract's basics; returns the list of failures."""
    failures, seqs = [], []

    def expect(what, ok):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failures.append(what)

    def message(timeout=8):
        while (e := s.next(timeout)) and e[0] == "status":
            expect("handshake is 101", e[1].startswith("HTTP/1.1 101"))
        if e and e[0] == "text":
            seqs.append(e[1].get("seq"))
            return e[1]
        return None

    snap = message()
    expect("first message is a snapshot with seq 0",
           bool(snap) and snap.get("t") == "snapshot" and snap.get("seq") == 0)
    if not snap:
        return failures
    expect("snapshot carries a build id", bool(snap.get("build")))
    if expect_build:
        expect(f"build is {expect_build}", snap.get("build") == expect_build)

    s.send('{"t":"bogus"}')
    m = message()
    expect("an unknown type gets error badRequest",
           bool(m) and m.get("t") == "error" and m.get("code") == "badRequest")

    s.send('{"t":"resync"}')
    while (m := message()) and m.get("t") != "snapshot":
        pass
    expect("resync gets a fresh snapshot with seq 0", bool(m) and m.get("seq") == 0)
    seqs = [0]

    while (m := message(timeout=7)) and m.get("t") != "tick":
        pass
    expect("a tick arrives within 7 s", bool(m))
    expect("seq counts up by one with no gaps", seqs == list(range(len(seqs))))
    return failures


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("host")
    p.add_argument("--port", type=int, default=80)
    p.add_argument("--nc", action="store_true", help="always go through /usr/bin/nc")
    p.add_argument("--smoke", action="store_true", help="run the contract smoke test")
    p.add_argument("--expect-build", help="with --smoke: the build id the snapshot must carry")
    p.add_argument("--send", action="append", default=[], help="a page -> Board JSON message, in order")
    p.add_argument("--listen", type=float, default=0, help="seconds to keep printing messages")
    args = p.parse_args()

    s = Session(args)
    try:
        if args.smoke:
            failures = smoke(s, args.expect_build)
            print("smoke test:", "FAILED: " + "; ".join(failures) if failures else "all passed")
            if failures:
                sys.exit(1)
        else:
            s.drain(1.5)
            for text in args.send:
                s.send(text)
                s.drain(1)
        s.drain(args.listen)
    except KeyboardInterrupt:
        pass
    finally:
        s.link.close()


if __name__ == "__main__":
    main()
