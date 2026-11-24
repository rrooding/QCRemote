// Development mock of the Board: serves web/ui over HTTP and speaks the
// contract in docs/api/ (README, schemas, examples; ADR 0012) on /ws,
// replaying a scripted session so the UI can be iterated on without
// reflashing. test/mock-server.test.js checks what it sends against
// docs/api/examples/.
//
//   node web/ui/tools/mock-server.mjs [--port 8080] [--fixture <file>] [--drop-every N]
//
// Deliberately dependency-free: Node's standard library only, no npm
// install, no package.json in the repo.
//
// This is a dev tool. It implements exactly the subset of RFC 6455 a
// browser needs for text frames, ping and close - not masking of server
// frames, not fragmentation, not permessage-deflate, no backpressure
// handling - and it trusts whatever it is sent. Never expose it beyond
// localhost.

import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join, normalize, extname } from 'node:path';

import { BUILD } from '../src/build.js';

const here = dirname(fileURLToPath(import.meta.url));
const docRoot = join(here, '..');

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};
const port = Number(argValue('--port', '8080'));
const fixturePath = argValue('--fixture', join(docRoot, 'fixtures', 'session.json'));
// --drop-every N: silently skip every Nth message after the snapshot while
// still burning its seq, so the page sees a gap. There is no other way to
// exercise the resync path on a link that works, and "the UI quietly shows
// stale state after a dropped message" is otherwise a bug that only shows
// up on a bad Wi-Fi day.
const dropEvery = Number(argValue('--drop-every', '0'));

const fixture = JSON.parse(await readFile(fixturePath, 'utf8'));

const kContentTypes = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.webmanifest': 'application/manifest+json; charset=utf-8',
};

// docs/api/README.md, "Liveness": at least one message every 5 s.
const kTickIntervalMs = 5000;
// How long the mock's pretend Quad Cortex takes to report a preset load.
const kSwitchDelayMs = 300;
// docs/api/README.md, "One page at a time".
const kReplacedCloseCode = 4001;

// --- HTTP --------------------------------------------------------------

// The WebSocket is the only API (ADR 0012 decision 8): no JSON over HTTP.
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
      // Same as the Board (docs/api/README.md, "HTTP"): revalidate every
      // asset, so an edit shows up on reload.
      'cache-control': 'no-cache',
    });
    res.end(body);
  } catch {
    res.writeHead(404, { 'content-type': 'text/plain' }).end('not found');
  }
});

// --- WebSocket (RFC 6455, the subset a browser needs) -------------------

const kWsMagic = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
// The Board serves one live WebSocket at a time, and the newest wins.
let liveSession = null;

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

  liveSession?.replace();
  const session = startSession(socket, () => {
    if (liveSession === session) liveSession = null;
  });
  liveSession = session;
});

/** Encodes one unfragmented server frame. opcode 0x1 = text, 0x8 = close, 0xA = pong. */
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

/** A close frame carrying a status code. */
function encodeClose(code) {
  const payload = Buffer.alloc(2);
  payload.writeUInt16BE(code, 0);
  return encodeFrame(payload, 0x8);
}

/**
 * Pulls complete frames out of a growing buffer. Returns the frames found
 * and whatever trailing bytes belong to a frame that hasn't fully arrived -
 * TCP gives no message boundaries, so a 2 KB send can arrive in any number
 * of chunks and two small sends can arrive as one.
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

// --- The Board's state ----------------------------------------------------

// Shared by every connection, like the real Board's: a switch made from
// one page is what the next page's snapshot shows.
const board = structuredClone(fixture.initial);
const findPreset = (index) =>
  board.catalog.folders.flatMap((f) => f.presets).find((p) => p.index === index);

let sessionCount = 0;

// --- The session: one connected page --------------------------------------

function startSession(socket, onClosed) {
  const id = ++sessionCount;
  let seq = 0;
  let sentSinceSnapshot = 0;
  let buffer = Buffer.alloc(0);
  let alive = true;
  let tickTimer = null;
  const timers = [];

  const log = (...parts) => console.log(`[ws ${id}]`, ...parts);

  // Liveness: a tick only when nothing else went out for the interval.
  const armTick = () => {
    clearTimeout(tickTimer);
    tickTimer = setTimeout(() => send({ t: 'tick' }), kTickIntervalMs);
  };

  // Every Board -> page message gets the next seq, in the examples' key
  // order: t, seq, then the message's own fields.
  const send = ({ t, ...fields }) => {
    if (!alive) return;
    seq += 1;
    armTick();
    sentSinceSnapshot += 1;
    if (dropEvery > 0 && sentSinceSnapshot % dropEvery === 0) {
      log(`dropped ${t} seq=${seq} (--drop-every ${dropEvery})`);
      return;
    }
    socket.write(encodeFrame(JSON.stringify({ t, seq, ...fields })));
  };

  const sendSnapshot = () => {
    if (!alive) return;
    seq = 0;
    sentSinceSnapshot = 0;
    armTick();
    const { catalog, current, link } = board;
    socket.write(
      encodeFrame(JSON.stringify({ t: 'snapshot', seq, build: BUILD, catalog, current, link }))
    );
  };

  const sendError = (code, message, commandId) =>
    send(commandId === undefined ? { t: 'error', code, message } : { t: 'error', id: commandId, code, message });

  const close = () => {
    if (!alive) return;
    alive = false;
    clearTimeout(tickTimer);
    for (const t of timers) clearTimeout(t);
    socket.destroy();
    onClosed();
    log('closed');
  };

  log('connected');
  sendSnapshot();

  // Scripted events: what the Board would have pushed on its own, e.g. a
  // footswitch preset change at the unit. `once` events only happen on the
  // first connection, so a take-over after a scripted replace sticks.
  for (const event of fixture.script ?? []) {
    if (event.once && id !== 1) continue;
    timers.push(
      setTimeout(() => {
        log(`script: ${event.note ?? Object.keys(event).join(',')}`);
        if (event.patch) {
          Object.assign(board, event.patch);
          send({ t: 'patch', ...event.patch });
        }
        if (event.catalog) {
          // The Catalog never changes by patch: a new one is a new snapshot.
          board.catalog = event.catalog;
          sendSnapshot();
        }
        if (event.error) sendError(event.error.code, event.error.message, event.error.id);
        if (event.replace) replace();
      }, event.atMs ?? 0)
    );
  }

  // Another page connected: close this one with 4001, as the Board does.
  function replace() {
    if (!alive) return;
    log('replaced (4001)');
    socket.write(encodeClose(kReplacedCloseCode));
    close();
  }

  const switchPreset = (command) => {
    if (!board.link.qcConnected) return sendError('qcDisconnected', 'link down', command.id);
    if (command.generation !== board.catalog.generation) {
      return sendError(
        'staleCatalog',
        `generation ${command.generation} != ${board.catalog.generation}`,
        command.id
      );
    }
    const preset = findPreset(command.index);
    if (!preset) return sendError('badIndex', `no preset ${command.index}`, command.id);
    timers.push(
      setTimeout(() => {
        board.current = { index: preset.index, name: preset.name, scene: null };
        // Always sent once the switch completes, even if nothing changed.
        send({ t: 'patch', current: board.current });
      }, kSwitchDelayMs)
    );
  };

  const receive = (text) => {
    let command;
    try {
      command = JSON.parse(text);
    } catch {
      return sendError('badRequest', 'unparseable');
    }
    if (command.t === 'resync') {
      log('resync requested');
      return sendSnapshot();
    }
    if (command.t === 'switchPreset') {
      if (
        typeof command.id !== 'string' ||
        !Number.isInteger(command.index) ||
        !Number.isInteger(command.generation)
      ) {
        return sendError('badRequest', 'malformed switchPreset', command.id);
      }
      log(`switchPreset ${command.index} (id ${command.id})`);
      return switchPreset(command);
    }
    sendError('badRequest', `unknown t ${command.t}`, command.id);
  };

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
      if (frame.opcode === 0x1) receive(frame.payload.toString('utf8'));
    }
  });

  socket.on('error', close);
  socket.on('close', close);

  return { replace };
}

server.listen(port, () => {
  const actualPort = server.address().port;
  console.log(`mock board on http://localhost:${actualPort}/  (ws://localhost:${actualPort}/ws)`);
  console.log(`serving ${docRoot}, build ${BUILD}`);
  console.log(`fixture ${fixturePath}`);
});
