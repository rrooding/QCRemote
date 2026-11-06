// Development mock of the board: serves web/poc over HTTP and speaks the
// ADR 0011 WebSocket protocol on /ws, replaying a recorded-shape session so
// the UI can be iterated on without reflashing (issue #84 question 7).
//
//   node web/poc/tools/mock-server.mjs [--port 8080] [--fixture <file>]
//
// Deliberately dependency-free: Node's standard library only, no npm install,
// no package.json in the repo. That costs about 70 lines of RFC 6455 framing
// below, and buys a mock that keeps working when a dependency doesn't.
//
// This is a dev tool. It implements exactly the subset of RFC 6455 a browser
// needs for text frames, ping and close - not masking of server frames, not
// fragmentation, not permessage-deflate, no backpressure handling - and it
// trusts whatever it is sent. Never expose it beyond localhost.

import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join, normalize, extname } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const docRoot = join(here, '..');

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};
const port = Number(argValue('--port', '8080'));
const fixturePath = argValue('--fixture', join(docRoot, 'fixtures', 'session.json'));
// --drop-every N: silently skip every Nth patch while still burning its
// sequence number, so the client sees a gap. There is no other way to
// exercise the resync path on a link that works, and "the UI quietly shows
// stale state after a dropped message" is exactly the failure that would
// otherwise only show up on a bad Wi-Fi day.
const dropEvery = Number(argValue('--drop-every', '0'));

const fixture = JSON.parse(await readFile(fixturePath, 'utf8'));

const kContentTypes = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.webmanifest': 'application/manifest+json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
};

const kTickIntervalMs = 5000;
// How long the mock pretends the board takes to actually load a preset and
// report it back. Long enough that the "pending" state is visible, which is
// the point of confirming rather than optimistically moving the selection.
const kSwitchLatencyMs = 450;

// --- HTTP ------------------------------------------------------------------

const server = createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  let path = normalize(decodeURIComponent(url.pathname));
  if (path === '/' || path === '\\') path = '/index.html';
  // normalize() collapses `..`, but a leading `..` survives it - reject
  // anything that still tries to climb out of the doc root.
  if (path.includes('..')) {
    res.writeHead(403).end('forbidden');
    return;
  }
  try {
    const body = await readFile(join(docRoot, path));
    res.writeHead(200, {
      'content-type': kContentTypes[extname(path)] ?? 'application/octet-stream',
      // No caching in the mock: the whole point is that an edit shows up on
      // reload. The board's own caching story is ADR 0011's asset pipeline.
      'cache-control': 'no-store',
    });
    res.end(body);
  } catch {
    res.writeHead(404, { 'content-type': 'text/plain' }).end('not found');
  }
});

// --- WebSocket (RFC 6455, the subset a browser needs) ----------------------

const kWsMagic = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

server.on('upgrade', (req, socket) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  if (url.pathname !== '/ws') {
    socket.end('HTTP/1.1 404 Not Found\r\n\r\n');
    return;
  }
  const key = req.headers['sec-websocket-key'];
  if (!key) {
    socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
    return;
  }
  const accept = createHash('sha1')
    .update(key + kWsMagic)
    .digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`
  );
  socket.setNoDelay(true);
  startSession(socket);
});

/** Encodes one unfragmented server frame. opcode 0x1 = text, 0xA = pong. */
function encodeFrame(payload, opcode = 0x1) {
  const data = Buffer.isBuffer(payload) ? payload : Buffer.from(payload, 'utf8');
  let header;
  if (data.length < 126) {
    header = Buffer.from([0x80 | opcode, data.length]);
  } else if (data.length < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode;
    header[1] = 126;
    header.writeUInt16BE(data.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(data.length), 2);
  }
  return Buffer.concat([header, data]);
}

/**
 * Pulls complete frames out of a growing buffer. Returns the frames found and
 * whatever trailing bytes belong to a frame that hasn't fully arrived - TCP
 * gives no message boundaries, so a 2 KB send can arrive in any number of
 * chunks and two small sends can arrive as one.
 */
function decodeFrames(buffer) {
  const frames = [];
  let offset = 0;
  while (buffer.length - offset >= 2) {
    const first = buffer[offset];
    const second = buffer[offset + 1];
    const opcode = first & 0x0f;
    const masked = (second & 0x80) !== 0;
    let length = second & 0x7f;
    let cursor = offset + 2;
    if (length === 126) {
      if (buffer.length < cursor + 2) break;
      length = buffer.readUInt16BE(cursor);
      cursor += 2;
    } else if (length === 127) {
      if (buffer.length < cursor + 8) break;
      length = Number(buffer.readBigUInt64BE(cursor));
      cursor += 8;
    }
    let mask;
    if (masked) {
      if (buffer.length < cursor + 4) break;
      mask = buffer.subarray(cursor, cursor + 4);
      cursor += 4;
    }
    if (buffer.length < cursor + length) break;
    const payload = Buffer.from(buffer.subarray(cursor, cursor + length));
    if (mask) {
      for (let i = 0; i < payload.length; i += 1) payload[i] ^= mask[i % 4];
    }
    frames.push({ opcode, payload });
    offset = cursor + length;
  }
  return { frames, rest: buffer.subarray(offset) };
}

// --- The session: one connected browser ------------------------------------

let sessionCount = 0;

function startSession(socket) {
  const id = ++sessionCount;
  // Each connection gets its own copy of the fixture state, so two open tabs
  // behave independently - which is also how we see that the real board's
  // single shared state must be broadcast to all of them, not per-client.
  const state = structuredClone(fixture.initial);
  let seq = 0;
  let buffer = Buffer.alloc(0);
  let alive = true;
  const timers = [];

  const log = (...parts) => console.log(`[ws ${id}]`, ...parts);

  const send = (message) => {
    if (!alive) return;
    socket.write(encodeFrame(JSON.stringify(message)));
  };

  const sendSnapshot = () => {
    seq += 1;
    send({ v: 1, t: 'snapshot', seq, ...state });
  };

  let patchCount = 0;
  const sendPatch = (partial) => {
    seq += 1;
    patchCount += 1;
    if (dropEvery > 0 && patchCount % dropEvery === 0) {
      log(`dropped patch seq=${seq} (--drop-every ${dropEvery})`);
      return;
    }
    send({ v: 1, t: 'patch', seq, ...partial });
  };

  const applyCurrent = (current) => {
    state.current = { ...state.current, ...current };
    sendPatch({ current: state.current });
  };

  const close = () => {
    if (!alive) return;
    alive = false;
    for (const t of timers) clearTimeout(t);
    socket.destroy();
    log('closed');
  };

  log('connected');
  sendSnapshot();

  // Liveness, same cadence the store expects (store.js kTickIntervalMs).
  const tick = setInterval(() => {
    if (!alive) {
      clearInterval(tick);
      return;
    }
    seq += 1;
    send({ v: 1, t: 'tick', seq });
  }, kTickIntervalMs);
  timers.push(tick);

  // Scripted events: what the board would have pushed on its own, e.g. the
  // player stepping on a footswitch at the unit itself.
  for (const event of fixture.script ?? []) {
    timers.push(
      setTimeout(() => {
        log(`script: ${event.note ?? event.t}`);
        if (event.current) applyCurrent(event.current);
        if (event.catalog) {
          state.catalog = { ...state.catalog, ...event.catalog };
          sendPatch({ catalog: state.catalog });
        }
        if (event.link) {
          state.link = { ...state.link, ...event.link };
          sendPatch({ link: state.link });
        }
      }, event.atMs ?? 0)
    );
  }

  socket.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    const { frames, rest } = decodeFrames(buffer);
    buffer = rest;
    for (const frame of frames) {
      if (frame.opcode === 0x8) {
        close();
        return;
      }
      if (frame.opcode === 0x9) {
        socket.write(encodeFrame(frame.payload, 0xa));
        continue;
      }
      if (frame.opcode !== 0x1) continue;
      let message;
      try {
        message = JSON.parse(frame.payload.toString('utf8'));
      } catch {
        log('unparseable frame, ignored');
        continue;
      }
      handleClientMessage(message);
    }
  });

  socket.on('error', close);
  socket.on('close', close);

  function handleClientMessage(message) {
    switch (message.t) {
      case 'switchPreset': {
        log(`switchPreset folder=${message.folder} index=${message.index} (id=${message.id})`);
        // The board doesn't acknowledge the request: it reports its own new
        // state once the Quad Cortex has actually moved. That delay is the
        // whole reason the UI shows a pending state (issue #78).
        timers.push(
          setTimeout(() => {
            const folder = state.catalog.folders[message.folder];
            const preset = folder?.presets.find((p) => p.index === message.index);
            applyCurrent({
              folder: message.folder,
              index: message.index,
              name: preset?.name ?? null,
              scene: 0,
            });
          }, kSwitchLatencyMs)
        );
        break;
      }
      case 'resync':
        log('resync requested');
        sendSnapshot();
        break;
      case 'ping':
        seq += 1;
        send({ v: 1, t: 'tick', seq });
        break;
      default:
        log(`ignored message type "${message.t}"`);
        break;
    }
  }
}

server.listen(port, () => {
  console.log(`mock board on http://localhost:${port}/  (ws://localhost:${port}/ws)`);
  console.log(`serving ${docRoot}`);
  console.log(`fixture ${fixturePath}`);
});
