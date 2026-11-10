// The whole client state model for the real UI: one store, fed by a
// reconnecting WebSocket, that components subscribe to (same shape as
// web/poc/src/store-controller.js's ReactiveController glue - reused as-is).
//
// This consumes exactly the subset of ADR 0011's envelope issue #75 ships
// on the board today (QcLiveUpdateEnvelope.hpp, firmware/README.md's
// "Live WebSocket updates (issue #75)" section) - NOT web/poc's research
// protocol, which assumed a `current` preset and a `link.qcConnected`
// field the board does not actually send:
//
//   server -> client  {"v":1,"t":"snapshot","seq":N,"catalog":{...}}
//                     {"v":1,"t":"patch","seq":N,"lastChange":{"command":N,"count":M}}
//                     {"v":1,"t":"tick","seq":N}                          (liveness only)
//   client -> server  {"v":1,"t":"resync"}        (after a seq gap - send a snapshot)
//
// The board has no notion of "the currently loaded preset" on this wire:
// RecallPreset and friends carry no preset identity, just a command id and
// a running count (QcPresetChangeEvent) - a transport-agnostic tally, not
// decoded preset content (see QcPresetTransport.hpp). So the most "live"
// state this UI can show is the catalog plus the last preset-change
// event's command/count - the same information the inline test page this
// replaces showed as its status line. Preset identity by index/name, and
// preset switching (#78), are later issues once the board emits them.
//
// `seq` is per-connection and monotonic. A gap means we missed a message,
// so the client asks for a fresh snapshot instead of silently rendering
// stale state.

export const PROTOCOL_VERSION = 1;

// Liveness. The board only speaks when something changes, so silence is
// ambiguous: it could be an idle rig or a TCP connection that died without
// a FIN (a Wi-Fi drop looks exactly like that). The board sends a `tick`
// at a known cadence (CONFIG_QC_WIFI_WEB_TICK_INTERVAL_MS, 5000ms by
// default) and a missed window here is treated as a dead link, which is
// what makes a reconnect happen at all on a half-open socket.
const kTickIntervalMs = 5000;
const kLivenessTimeoutMs = kTickIntervalMs * 2.5;

// Reconnect backoff: fast enough that walking back into Wi-Fi range feels
// instant, capped so a board that is simply off doesn't get hammered.
// Jitter keeps two open tabs from retrying in lockstep. This is also what
// a second browser's WebSocket does forever under the board's one-client
// policy (CONFIG_WEBSOCKET_MAX_CONTEXTS=1) - its upgrade completes at the
// HTTP level then the connection is torn down immediately, so this loop
// just keeps trying until the first browser disconnects.
const kBackoffMs = [250, 500, 1000, 2000, 5000, 10000];

const emptyState = Object.freeze({
  /** 'offline' | 'connecting' | 'online' - this WebSocket's own state. */
  link: 'offline',
  /** Last error text worth showing, or null. */
  error: null,
  /** { generation, presetCount, incomplete, folders: [{ name, presets }] } */
  catalog: { generation: 0, presetCount: 0, incomplete: true, folders: [] },
  /** { command, count } from the last patch, or null before the first one. */
  lastChange: null,
});

export class Store {
  #state = emptyState;
  #listeners = new Set();
  #socket = null;
  #url;
  #httpOrigin;
  #attempt = 0;
  #seq = null;
  #gotSnapshot = false;
  #livenessTimer = null;
  #reconnectTimer = null;
  #closed = false;

  constructor(url, httpOrigin) {
    this.#url = url;
    this.#httpOrigin = httpOrigin;
  }

  get state() {
    return this.#state;
  }

  /** Subscribe to state changes. Returns an unsubscribe function. */
  subscribe(listener) {
    this.#listeners.add(listener);
    return () => this.#listeners.delete(listener);
  }

  connect() {
    this.#closed = false;
    this.#fetchInitialCatalog();
    this.#open();
    // Coming back from a locked phone or a backgrounded tab is the single
    // most common way this connection dies on iOS. Both events are cheap
    // and land well before the liveness timeout would.
    addEventListener('online', () => this.#retryNow());
    addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') this.#retryNow();
    });
  }

  close() {
    this.#closed = true;
    clearTimeout(this.#reconnectTimer);
    this.#socket?.close();
  }

  // --- internals -----------------------------------------------------------

  // A second open browser never gets a live WebSocket at all under the
  // board's one-client policy: Zephyr completes the upgrade at the HTTP
  // level and then tears the connection down immediately
  // (firmware/README.md's "What a second browser sees under the one-client
  // policy"), so #open()'s reconnect loop below just retries on backoff
  // forever. Without this fetch that second tab would show an empty list
  // the whole time - this is the same one-shot GET the old inline page
  // made on load (QcWifiWebTransport.hpp's kIndexHtml, before this issue),
  // now made by the store instead of relying on a page reload.
  async #fetchInitialCatalog() {
    try {
      const response = await fetch(`${this.#httpOrigin}/api/presets.json`);
      if (!response.ok) return;
      const catalog = await response.json();
      // A WebSocket snapshot is always current; don't let a slower HTTP
      // response clobber it once one has arrived.
      if (!this.#gotSnapshot) this.#patchState({ catalog });
    } catch {
      // No link yet - a WebSocket snapshot will supersede this once the
      // connection comes up, and #open()'s own reconnect loop is already
      // handling that side.
    }
  }

  #open() {
    this.#patchState({ link: 'connecting' });
    let socket;
    try {
      socket = new WebSocket(this.#url);
    } catch (err) {
      // Constructing a WebSocket throws synchronously for a malformed URL,
      // which is a bug on our side, not a dropped link - don't retry it.
      this.#patchState({ link: 'offline', error: String(err) });
      return;
    }
    this.#socket = socket;

    socket.onopen = () => {
      this.#attempt = 0;
      this.#seq = null;
      this.#patchState({ link: 'online', error: null });
      this.#armLiveness();
    };

    socket.onmessage = (event) => {
      this.#armLiveness();
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        // A frame we can't parse is a protocol bug, not a reason to drop
        // the connection - every other message still works.
        return;
      }
      this.#handle(message);
    };

    socket.onclose = () => {
      clearTimeout(this.#livenessTimer);
      this.#patchState({ link: 'offline' });
      this.#scheduleReconnect();
    };

    // onerror is always followed by onclose, so the reconnect is handled
    // there; this only exists so the browser console isn't the only
    // record.
    socket.onerror = () => this.#patchState({ error: 'WebSocket error' });
  }

  #handle(message) {
    if (message.v !== PROTOCOL_VERSION) {
      this.#patchState({
        error: `Board speaks protocol v${message.v}, this page speaks v${PROTOCOL_VERSION}`,
      });
      return;
    }

    // Gap detection. A snapshot resets the sequence; anything else must be
    // exactly one past what we last saw, or we have missed a change and
    // our state is a guess.
    if (message.t === 'snapshot') {
      this.#seq = message.seq ?? 0;
    } else if (typeof message.seq === 'number') {
      if (this.#seq !== null && message.seq !== this.#seq + 1) {
        this.#seq = null;
        this.#send({ v: PROTOCOL_VERSION, t: 'resync' });
        return;
      }
      this.#seq = message.seq;
    }

    switch (message.t) {
      case 'snapshot':
        this.#gotSnapshot = true;
        this.#patchState({
          catalog: message.catalog ?? emptyState.catalog,
          // A fresh snapshot already reports the full current state; a
          // lastChange from before it would describe a now-superseded
          // moment.
          lastChange: null,
        });
        break;
      case 'patch':
        if (message.lastChange) this.#patchState({ lastChange: message.lastChange });
        break;
      case 'tick':
        break;
      default:
        // Forward compatibility: a v1 client must ignore message types it
        // doesn't know rather than treat them as an error, so the board
        // can add message types (the full schema is #15) without breaking
        // an older page served from older flash.
        break;
    }
  }

  #armLiveness() {
    clearTimeout(this.#livenessTimer);
    this.#livenessTimer = setTimeout(() => {
      // Nothing for 2.5 tick intervals: assume a half-open socket. close()
      // fires onclose, which schedules the reconnect.
      this.#socket?.close();
    }, kLivenessTimeoutMs);
  }

  #scheduleReconnect() {
    if (this.#closed) return;
    const base = kBackoffMs[Math.min(this.#attempt, kBackoffMs.length - 1)];
    this.#attempt += 1;
    const delay = base + Math.random() * base * 0.3;
    clearTimeout(this.#reconnectTimer);
    this.#reconnectTimer = setTimeout(() => this.#open(), delay);
  }

  #retryNow() {
    if (this.#closed || this.#state.link === 'online') return;
    this.#attempt = 0;
    clearTimeout(this.#reconnectTimer);
    this.#open();
  }

  #send(message) {
    if (this.#socket?.readyState === WebSocket.OPEN) {
      this.#socket.send(JSON.stringify(message));
    }
  }

  #patchState(partial) {
    this.#state = { ...this.#state, ...partial };
    for (const listener of this.#listeners) listener(this.#state);
  }
}

// Where the WebSocket lives. Served from the board, that's the same
// origin. Served from a Mac during development it is either the mock
// server on this same origin or, with ?board=<ip>, a real board - which is
// what makes iterating on the UI without reflashing possible (ADR 0011
// question 7).
export function websocketUrl(location) {
  const board = new URLSearchParams(location.search).get('board');
  if (board) return `ws://${board}/ws`;
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${scheme}//${location.host}/ws`;
}

// Same seam as websocketUrl(), for the plain HTTP GET #fetchInitialCatalog()
// makes: '' (same origin as the page) normally, or the real board's origin
// under ?board=<ip> so that fetch still reaches the board while the page
// itself is served from the Mac.
export function httpOrigin(location) {
  const board = new URLSearchParams(location.search).get('board');
  return board ? `http://${board}` : '';
}
