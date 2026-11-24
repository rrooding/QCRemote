// The dev loop's mock Board, checked against the contract: runs
// tools/mock-server.mjs on a free port with a short scripted fixture, talks
// to it with Node's built-in WebSocket client, and compares what it sends
// with docs/api/examples/ (same keys, same order, same seq rules).
//
//   node --test web/ui/test/

import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { BUILD } from '../src/build.js';

const here = dirname(fileURLToPath(import.meta.url));
const examplesDir = join(here, '..', '..', '..', 'docs', 'api', 'examples');
const example = (dir, name) => JSON.parse(readFileSync(join(examplesDir, dir, `${name}.json`), 'utf8'));

/** The key order of an object and of each of its object-valued sections. */
function shape(message) {
  const sections = {};
  for (const [key, value] of Object.entries(message)) {
    if (value && typeof value === 'object' && !Array.isArray(value)) {
      sections[key] = Object.keys(value);
    }
  }
  return { keys: Object.keys(message), sections };
}

const fixture = {
  initial: {
    catalog: example('server', 'snapshot').catalog,
    current: { index: 2, name: 'Opener', scene: null },
    link: { qcConnected: true },
  },
  script: [
    { atMs: 100, patch: { current: { index: 3, name: 'Verse 1', scene: null } } },
    { atMs: 200, error: { code: 'badRequest', message: 'unparseable' } },
    { atMs: 300, patch: { link: { qcConnected: true } } },
    { atMs: 600, once: true, replace: true },
  ],
};

let server;
let port;
let workDir;

before(async () => {
  workDir = mkdtempSync(join(tmpdir(), 'qc-mock-'));
  const fixturePath = join(workDir, 'session.json');
  writeFileSync(fixturePath, JSON.stringify(fixture));
  server = spawn(process.execPath, [
    join(here, '..', 'tools', 'mock-server.mjs'),
    '--port', '0',
    '--fixture', fixturePath,
  ]);
  port = await new Promise((resolve, reject) => {
    let out = '';
    server.stdout.on('data', (chunk) => {
      out += chunk;
      const match = out.match(/localhost:(\d+)/);
      if (match) resolve(Number(match[1]));
    });
    server.on('exit', (code) => reject(new Error(`mock server exited with ${code}`)));
  });
});

after(() => {
  server?.kill();
  rmSync(workDir, { recursive: true, force: true });
});

/** Opens a page-side socket and records everything it receives. */
function connect() {
  const ws = new WebSocket(`ws://localhost:${port}/ws`);
  const client = { ws, messages: [], closeCode: null };
  const waiters = [];
  ws.onmessage = (event) => {
    client.messages.push(JSON.parse(event.data));
    for (const w of [...waiters]) w();
  };
  ws.onclose = (event) => {
    client.closeCode = event.code;
    for (const w of [...waiters]) w();
  };
  client.until = (predicate, timeoutMs = 3000) =>
    new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('timed out')), timeoutMs);
      const check = () => {
        if (predicate(client)) {
          clearTimeout(timer);
          waiters.splice(waiters.indexOf(check), 1);
          resolve();
        }
      };
      waiters.push(check);
      check();
    });
  return client;
}

test('the mock replays contract-shaped messages with continuous seq, then replaces with 4001', async () => {
  const page = connect();
  await page.until((c) => c.closeCode !== null);

  const [snapshot, ...rest] = page.messages;
  assert.equal(snapshot.t, 'snapshot');
  assert.equal(snapshot.seq, 0);
  assert.equal(snapshot.build, BUILD, 'the dev page must not reload on the mock');
  assert.deepEqual(shape(snapshot), shape(example('server', 'snapshot')));

  rest.forEach((message, i) => assert.equal(message.seq, i + 1));
  const patch = rest.find((m) => m.t === 'patch' && m.current);
  assert.deepEqual(shape(patch), shape(example('server', 'patch-current')));
  const error = rest.find((m) => m.t === 'error');
  assert.deepEqual(shape(error), shape(example('server', 'error-bad-request')));
  assert.equal(page.closeCode, 4001);
});

test('resync answers with a fresh snapshot at seq 0', async () => {
  const page = connect();
  await page.until((c) => c.messages.length >= 1);

  page.ws.send(JSON.stringify(example('client', 'resync')));
  await page.until((c) => c.messages.length >= 2 && c.messages.at(-1).t === 'snapshot');

  assert.equal(page.messages.at(-1).seq, 0);
  page.ws.close();
});

test('switchPreset is confirmed by a current patch', async () => {
  const page = connect();
  await page.until((c) => c.messages.length >= 1);

  // Not index 3: the fixture's own script already patches to that one.
  page.ws.send(JSON.stringify({ ...example('client', 'switch-preset'), index: 1 }));
  await page.until((c) =>
    c.messages.some((m) => m.t === 'patch' && m.current?.index === 1 && m.current.name === 'Edge Crunch')
  );
  page.ws.close();
});

test('a rejected switchPreset gets an error with its id', async () => {
  const page = connect();
  await page.until((c) => c.messages.length >= 1);

  page.ws.send('{"t":"switchPreset","id":"x1","index":99,"generation":7}');
  page.ws.send('{"t":"switchPreset","id":"x2","index":1,"generation":6}');
  await page.until((c) => c.messages.filter((m) => m.t === 'error' && m.id).length >= 2);

  const errors = page.messages.filter((m) => m.t === 'error' && m.id);
  assert.deepEqual(errors.map((e) => [e.id, e.code]), [['x1', 'badIndex'], ['x2', 'staleCatalog']]);
  assert.deepEqual(shape(errors[1]), shape(example('server', 'error-stale-catalog')));
  page.ws.close();
});

test('a newer page replaces the older one with close code 4001', async () => {
  const older = connect();
  await older.until((c) => c.messages.length >= 1);

  const newer = connect();
  await older.until((c) => c.closeCode !== null);
  await newer.until((c) => c.messages.length >= 1);

  assert.equal(older.closeCode, 4001);
  assert.equal(newer.messages[0].t, 'snapshot');
  newer.ws.close();
});
