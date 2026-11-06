// The whole client state model for the PoC: one store, fed by a
// reconnecting WebSocket, that components subscribe to.
//
// Why a store rather than per-component fetches: the board is the single
// source of truth and it pushes. A component that owned its own connection
// would have to re-derive the same state on every reconnect, and two open
// tabs would drift. One store means one connection, one place where the
// snapshot/patch protocol lives, and one place to look when the UI shows
// something stale.
//
// The protocol this speaks is the proposal in ADR 0011 (issue #15):
//   server -> client  {"v":1,"t":"snapshot","seq":N, catalog, current, link}
//                     {"v":1,"t":"patch","seq":N, <section>:{...}}   (shallow merge)
//                     {"v":1,"t":"tick","seq":N}                     (liveness only)
//   client -> server  {"v":1,"t":"switchPreset","id":"c3","folder":0,"index":12}
//                     {"v":1,"t":"resync"}   (after a seq gap - send a snapshot)
//                     {"v":1,"t":"ping"}
// `seq` is per-connection and monotonic. A gap means we missed a patch, so
// the client asks for a snapshot instead of silently rendering stale state.

export const PROTOCOL_VERSION = 1;

// Liveness. The board only speaks when something changes, so silence is
// ambiguous: it could be an idle rig or a TCP connection that died without a
// FIN (a Wi-Fi drop looks exactly like that). The server sends a `tick` at a
// known cadence and we treat a missed window as a dead link, which is what
// makes a reconnect happen at all on a half-open socket.
const kTickIntervalMs = 5000;
const kLivenessTimeoutMs = kTickIntervalMs * 2.5;

// Reconnect backoff: fast enough that walking back into Wi-Fi range feels
// instant, capped so a board that is simply off doesn't get hammered. Jitter
// keeps two open tabs from retrying in lockstep.
const kBackoffMs = [250, 500, 1000, 2000, 5000, 10000];

// How long a preset switch may stay unconfirmed before the UI gives up on it.
// Preset switching is *confirmed*, not optimistic (issue #78): tapping a
// preset marks it pending and only the board's own state change moves the
// selection. Optimistically moving it would mean inventing a device state we
// have no evidence for - and on a 2.4 GHz link that lie is visible.
const kPendingTimeoutMs = 4000;

const emptyState = Object.freeze({
  /** 'offline' | 'connecting' | 'online' */
  link: 'offline',
  /** Last error text worth showing, or null. */
  error: null,
  /** True once the board has told us the Quad Cortex itself is attached. */
  qcConnected: false,
  /** { generation, presetCount, incomplete, folders: [{ name, presets }] } */
  catalog: { generation: 0, presetCount: 0, incomplete: true, folders: [] },
  /** { folder, index, name, scene } - the board's own idea of what is loaded. */
  current: { folder: null, index: null, name: null, scene: null },
  /** Preset index we asked for and haven't seen confirmed yet, or null. */
  pending: null,
});

export class Store {
  #state = emptyState;
  #listeners = new Set();
  #socket = null;
  #url;
  #attempt = 0;
  #seq = null;
  #livenessTimer = null;
  #reconnectTimer = null;
  #pendingTimer = null;
  #correlation = 0;
  #closed = false;

  constructor(url) {
    this.#url = url;
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
    this.#open();
    // Coming back from a locked phone or a backgrounded tab is the single
    // most common way this connection dies on iOS. Both events are cheap and
    // land well before the liveness timeout would.
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

  /** Ask the board to load a preset. The selection only moves when it agrees. */
  switchPreset(folder, index) {
    if (this.#state.link !== 'online') return;
    this.#correlation += 1;
    this.#send({
      v: PROTOCOL_VERSION,
      t: 'switchPreset',
      id: `c${this.#correlation}`,
      folder,
      index,
    });
    this.#patchState({ pending: index });
    clearTimeout(this.#pendingTimer);
    this.#pendingTimer = setTimeout(() => {
      if (this.#state.pending === index) {
        this.#patchState({ pending: null, error: 'The board did not confirm that switch' });
      }
    }, kPendingTimeoutMs);
  }

  // --- internals -----------------------------------------------------------

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
        // A frame we can't parse is a protocol bug, not a reason to drop the
        // connection - every other message still works.
        return;
      }
      this.#handle(message);
    };

    socket.onclose = () => {
      clearTimeout(this.#livenessTimer);
      this.#patchState({ link: 'offline', pending: null });
      this.#scheduleReconnect();
    };

    // onerror is always followed by onclose, so the reconnect is handled
    // there; this only exists so the browser console isn't the only record.
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
    // exactly one past what we last saw, or we have missed a change and our
    // state is a guess.
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
        this.#patchState({
          catalog: message.catalog ?? emptyState.catalog,
          current: { ...emptyState.current, ...(message.current ?? {}) },
          qcConnected: Boolean(message.link?.qcConnected),
          pending: null,
        });
        break;
      case 'patch': {
        const next = {};
        if (message.catalog) next.catalog = { ...this.#state.catalog, ...message.catalog };
        if (message.current) {
          next.current = { ...this.#state.current, ...message.current };
          // The board moving to the preset we asked for is the confirmation.
          if (this.#state.pending !== null && next.current.index === this.#state.pending) {
            next.pending = null;
            clearTimeout(this.#pendingTimer);
          }
        }
        if (message.link) next.qcConnected = Boolean(message.link.qcConnected);
        this.#patchState(next);
        break;
      }
      case 'tick':
        break;
      default:
        // Forward compatibility: a v1 client must ignore message types it
        // doesn't know rather than treat them as an error, so the board can
        // add message types without breaking an older page served from an
        // older flash image.
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

// Where the WebSocket lives. Served from the board, that's the same origin.
// Served from a Mac during development it is either the mock server on this
// same origin or, with ?board=<ip>, a real board - which is what makes
// iterating on the UI without reflashing possible (issue #84 question 7).
export function websocketUrl(location) {
  const board = new URLSearchParams(location.search).get('board');
  if (board) return `ws://${board}/ws`;
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${scheme}//${location.host}/ws`;
}
