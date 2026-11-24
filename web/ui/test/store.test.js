// Store tests at the seam the #15 spec agreed: the Store's public
// interface (state, subscribe, connect, close, switchPreset, takeOver),
// driven by a fake WebSocket, location and sessionStorage, fed the
// Board -> page messages in docs/api/examples/server/ byte for byte.
//
//   node --test web/ui/test/
//
// Assertions are only on store.state and the frames the store sends.

import { test, beforeEach, afterEach, mock } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { Store } from '../src/store.js';

const examplesDir = join(
  dirname(fileURLToPath(import.meta.url)),
  '..', '..', '..', 'docs', 'api', 'examples'
);

/** One example message, as the raw text frame the Board sends. */
const example = (name) => readFileSync(join(examplesDir, 'server', `${name}.json`), 'utf8').trim();
const clientExample = (name) =>
  readFileSync(join(examplesDir, 'client', `${name}.json`), 'utf8').trim();

/** The examples' build id, so a store built with it accepts their snapshots. */
const kExampleBuild = JSON.parse(example('snapshot')).build;

/** Re-stamps an example's seq, for scripting longer sequences. */
const withSeq = (text, seq) => JSON.stringify({ ...JSON.parse(text), seq });

class FakeWebSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSING = 2;
  static CLOSED = 3;
  static instances = [];

  readyState = FakeWebSocket.CONNECTING;
  sent = [];

  constructor(url) {
    this.url = url;
    FakeWebSocket.instances.push(this);
  }

  send(text) {
    this.sent.push(text);
  }

  close() {
    if (this.readyState === FakeWebSocket.CLOSED) return;
    this.readyState = FakeWebSocket.CLOSED;
    this.onclose?.({ code: 1005 });
  }

  // --- what the Board does --------------------------------------------
  open() {
    this.readyState = FakeWebSocket.OPEN;
    this.onopen?.({});
  }

  receive(text) {
    this.onmessage?.({ data: text });
  }

  serverClose(code) {
    this.readyState = FakeWebSocket.CLOSED;
    this.onclose?.({ code });
  }
}

class FakeStorage {
  #items = new Map();
  getItem(key) {
    return this.#items.has(key) ? this.#items.get(key) : null;
  }
  setItem(key, value) {
    this.#items.set(key, String(value));
  }
}

let location;
let sessionStorage;

beforeEach(() => {
  FakeWebSocket.instances = [];
  location = { reloads: 0, reload() { this.reloads += 1; } };
  sessionStorage = new FakeStorage();
  mock.timers.enable({ apis: ['setTimeout'] });
});

afterEach(() => {
  mock.timers.reset();
});

function makeStore({ build = kExampleBuild } = {}) {
  const store = new Store({
    url: 'ws://board.test/ws',
    build,
    WebSocket: FakeWebSocket,
    location,
    sessionStorage,
  });
  store.connect();
  return store;
}

/** The socket the store opened most recently. */
const socket = () => FakeWebSocket.instances.at(-1);

/** A store that is online and has applied the full example snapshot. */
function onlineStore(options) {
  const store = makeStore(options);
  socket().open();
  socket().receive(example('snapshot'));
  return store;
}

test('a snapshot sets the Catalog, Current preset and Link', () => {
  const store = onlineStore();

  assert.equal(store.state.connection, 'online');
  assert.deepEqual(store.state.current, { index: 2, name: 'Opener', scene: null });
  assert.deepEqual(store.state.link, { qcConnected: true });
  assert.equal(store.state.catalog.generation, 7);
  assert.deepEqual(
    store.state.catalog.folders.map((f) => f.name),
    ['My Presets', 'Live Set']
  );
});

test('a patch replaces the whole section it carries', () => {
  const store = onlineStore();
  // A field a later contract adds to `current` (the extension point):
  // a merge would leave it behind after the next patch, a replace drops it.
  socket().receive('{"t":"patch","seq":1,"current":{"index":2,"name":"Opener","scene":null,"tempo":120}}');

  socket().receive(withSeq(example('patch-current'), 2));

  assert.deepEqual(store.state.current, { index: 3, name: 'Verse 1', scene: null });
  assert.deepEqual(store.state.link, { qcConnected: true });
});

test('a patch never changes the Catalog', () => {
  const store = onlineStore();

  socket().receive(
    '{"t":"patch","seq":1,"catalog":{"generation":8,"presetCount":0,"incomplete":false,"folders":[]}}'
  );

  assert.equal(store.state.catalog.generation, 7);
});

test('a seq gap sends resync and discards every message until the next snapshot', () => {
  const store = onlineStore();

  // seq 1 was dropped: 2 is a gap.
  socket().receive(withSeq(example('patch-current'), 2));
  // Already on its way, and consecutive with the gap - still stale.
  socket().receive(
    withSeq('{"t":"patch","seq":0,"link":{"qcConnected":false}}', 3)
  );

  assert.deepEqual(socket().sent, [clientExample('resync')]);
  assert.deepEqual(store.state.current, { index: 2, name: 'Opener', scene: null });
  assert.deepEqual(store.state.link, { qcConnected: true });

  socket().receive(example('snapshot-before-listing'));
  socket().receive(example('patch-current'));

  assert.equal(store.state.catalog.generation, 0);
  assert.deepEqual(store.state.current, { index: 3, name: 'Verse 1', scene: null });
  assert.equal(socket().sent.length, 1);
});

test('a snapshot from another build reloads the page without applying it', () => {
  const store = onlineStore({ build: 'old-build' });

  assert.equal(location.reloads, 1);
  assert.equal(store.state.catalog.generation, 0);
  assert.deepEqual(store.state.current, { index: null, name: null, scene: null });
});

test('the page reloads at most once per Board build per tab', () => {
  onlineStore({ build: 'old-build' });
  // The reload came back with the same old page (a cache ignoring no-cache).
  const store = onlineStore({ build: 'old-build' });

  assert.equal(location.reloads, 1);
  assert.equal(store.state.catalog.generation, 7);
  assert.notEqual(store.state.error, null);
});

test('a snapshot from the same build never reloads', () => {
  onlineStore();

  assert.equal(location.reloads, 0);
});

/** Sends a switch and returns the frame the store put on the wire. */
function switchTo(store, index) {
  store.switchPreset(index);
  return JSON.parse(socket().sent.at(-1));
}

/** The error example, re-addressed to one of this page's own command ids. */
const errorFor = (name, id, seq) => JSON.stringify({ ...JSON.parse(example(name)), id, seq });

test('switching a Preset sends switchPreset and marks it pending', () => {
  const store = onlineStore();

  const frame = switchTo(store, 3);

  // Same shape as the contract's example; only the page-chosen id differs.
  assert.deepEqual(frame, { ...JSON.parse(clientExample('switch-preset')), id: frame.id });
  assert.match(frame.id, /^[0-9A-Za-z_-]{1,16}$/);
  assert.deepEqual(store.state.pending, { id: frame.id, index: 3 });
});

test('an error clears the pending state for its id, in the page\'s own words', () => {
  const store = onlineStore();
  const { id } = switchTo(store, 3);

  socket().receive(errorFor('error-stale-catalog', id, 1));

  assert.equal(store.state.pending, null);
  assert.equal(typeof store.state.error, 'string');
  assert.ok(!store.state.error.includes(JSON.parse(example('error-stale-catalog')).message));
});

test('an error for another id leaves the pending state alone', () => {
  const store = onlineStore();
  const { id } = switchTo(store, 3);

  socket().receive(withSeq(example('error-bad-request'), 1));

  assert.deepEqual(store.state.pending, { id, index: 3 });
  assert.equal(typeof store.state.error, 'string');
});

test('a current patch for the pending Preset clears the pending state', () => {
  const store = onlineStore();
  switchTo(store, 3);

  socket().receive(example('patch-current'));

  assert.equal(store.state.pending, null);
  assert.equal(store.state.current.index, 3);
});

test('ticks and errors count toward seq', () => {
  const store = onlineStore();

  socket().receive(withSeq(example('tick'), 1));
  socket().receive(withSeq(example('error-bad-request'), 2));
  socket().receive(withSeq(example('patch-current'), 3));

  assert.deepEqual(socket().sent, []);
  assert.equal(store.state.current.index, 3);
});
