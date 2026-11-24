// The whole client state model for the page: one store, fed by a
// reconnecting WebSocket, that components subscribe to.
//
// It speaks the contract in docs/api/ (README, schemas, examples) and
// ADR 0012 - that is the source of truth for every message shape, rule and
// number below; this comment only says how the page applies them.
//
//   Board -> page  snapshot  {t,seq:0,build,catalog,current,link}  complete state
//                  patch     {t,seq,current?,link?}                 replaces sections
//                  tick      {t,seq}                                liveness only
//                  error     {t,seq,id?,code,message}               a rejected command
//   page -> Board  resync    {t}                                    after a seq gap
//
// There is no protocol version: a snapshot whose `build` differs from the
// page's own BUILD (src/build.js, rewritten by the firmware's embed step)
// means this tab is running old JavaScript against new firmware, so it
// reloads instead of guessing.

// Liveness: the Board sends something at least every 5 s, and 12.5 s of
// silence (2.5 intervals) is a dead Connection. Both numbers are the
// contract's (docs/api/README.md, "Liveness") - change them there first.
const kTickIntervalMs = 5000;
const kLivenessTimeoutMs = kTickIntervalMs * 2.5;

// Reconnect backoff: fast enough that walking back into Wi-Fi range feels
// instant, capped so a Board that is simply off doesn't get hammered.
// Jitter keeps two open tabs from retrying in lockstep.
const kBackoffMs = [250, 500, 1000, 2000, 5000, 10000];

// The Board closes an older page's socket with this code when a newer
// page connects (docs/api/README.md, "One page at a time").
const kReplacedCloseCode = 4001;
const kReplacedText = 'Opened on another device';

// sessionStorage key holding the Board build this tab last reloaded for.
const kReloadedForBuildKey = 'qcRemote.reloadedForBuild';
const kPageOutdatedText = "This page doesn't match the Board's firmware. Reload it to update.";

// The page's own wording for each error `code` (docs/api/README.md,
// "Errors"). The Board's `message` is English detail for the console and
// logs, and is never shown.
const kErrorText = Object.freeze({
  qcDisconnected: "The Quad Cortex isn't connected to the Board.",
  badIndex: "That preset isn't on the Quad Cortex any more.",
  staleCatalog: 'The preset list just changed. Pick the preset again.',
  busy: 'Still switching to the last preset.',
  badRequest: "The Board didn't understand the page's request.",
});
const kUnknownErrorText = 'The Board refused that.';

const emptyState = Object.freeze({
  /**
   * This page's Connection to the Board: 'offline' | 'connecting' |
   * 'online' | 'replaced' (another page took over; see takeOver()).
   */
  connection: 'offline',
  /** Last problem worth showing, in the page's own words, or null. */
  error: null,
  /** { generation, presetCount, incomplete, folders: [{ name, presets: [{ index, name }] }] } */
  catalog: Object.freeze({ generation: 0, presetCount: 0, incomplete: true, folders: [] }),
  /** The Current preset: { index, name, scene }, each null when unknown. */
  current: Object.freeze({ index: null, name: null, scene: null }),
  /** The Link to the Quad Cortex: { qcConnected }. */
  link: Object.freeze({ qcConnected: false }),
  /**
   * The switch the page asked for and the Board hasn't confirmed or
   * rejected yet: { id, index }, or null. Confirmed, not optimistic
   * (ADR 0011 decision 5): the selection only moves with `current`.
   */
  pending: null,
});

export class Store {
  #state = emptyState;
  #listeners = new Set();
  #url;
  #build;
  #WebSocket;
  #location;
  #sessionStorage;
  #socket = null;
  #attempt = 0;
  #seq = null;
  #livenessTimer = null;
  #reconnectTimer = null;
  #closed = false;
  #commandCount = 0;
  #listening = false;

  /**
   * @param {object} options
   * @param {string} options.url             the Board's WebSocket URL
   * @param {string} options.build           this page's build id (src/build.js)
   * @param {typeof WebSocket} options.WebSocket
   * @param {{ reload(): void }} options.location
   * @param {Storage} options.sessionStorage
   */
  constructor({ url, build, WebSocket, location, sessionStorage }) {
    this.#url = url;
    this.#build = build;
    this.#WebSocket = WebSocket;
    this.#location = location;
    this.#sessionStorage = sessionStorage;
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
    // most common way this Connection dies on iOS. Both events are cheap
    // and land well before the liveness timeout would. (Absent outside a
    // browser, e.g. under node --test.)
    if (!this.#listening && typeof globalThis.addEventListener === 'function') {
      this.#listening = true;
      globalThis.addEventListener('online', () => this.#retryNow());
      globalThis.addEventListener('visibilitychange', () => {
        if (globalThis.document?.visibilityState === 'visible') this.#retryNow();
      });
    }
  }

  /**
   * Connect again after another page replaced this one (close code 4001).
   * The Board then closes that other page's socket with 4001 in turn.
   */
  takeOver() {
    if (this.#state.connection !== 'replaced') return;
    this.#attempt = 0;
    this.#open();
  }

  /**
   * Ask the Board to load the Preset with this Catalog-wide index. Marks it
   * pending; the Board's `current` patch or an `error` with the same id
   * settles it. (The 4 s give-up and refusing a tap while pending are #78.)
   */
  switchPreset(index) {
    this.#commandCount += 1;
    const id = `c${this.#commandCount}`;
    this.#send({ t: 'switchPreset', id, index, generation: this.#state.catalog.generation });
    this.#setState({ pending: { id, index }, error: null });
  }

  close() {
    this.#closed = true;
    clearTimeout(this.#reconnectTimer);
    this.#socket?.close();
  }

  // --- internals -----------------------------------------------------------

  #open() {
    this.#setState({ connection: 'connecting' });
    const socket = new this.#WebSocket(this.#url);
    this.#socket = socket;

    socket.onopen = () => {
      this.#attempt = 0;
      // The page sends nothing on connect: the Board sends a snapshot as
      // soon as the socket opens, and until it arrives nothing else applies.
      this.#seq = null;
      this.#setState({ connection: 'online', error: null });
      this.#armLiveness();
    };

    socket.onmessage = (event) => {
      if (socket !== this.#socket) return;
      this.#armLiveness();
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        // A frame we can't parse is a Board bug, not a reason to drop the
        // Connection - every other message still works.
        return;
      }
      this.#handle(message);
    };

    socket.onclose = (event) => {
      // A socket this store already replaced (liveness timeout, take-over)
      // can still report its close late; only the current one counts.
      if (socket !== this.#socket) return;
      clearTimeout(this.#livenessTimer);
      // Either way, a switch in flight on a dead socket is never answered,
      // so `pending` clears.
      if (event?.code === kReplacedCloseCode) {
        // A newer page took the Board's one live socket. Reconnecting on
        // our own would just replace it back, so the two would fight;
        // only the user's take-over connects again.
        this.#setState({ connection: 'replaced', error: kReplacedText, pending: null });
        return;
      }
      this.#setState({ connection: 'offline', pending: null });
      this.#scheduleReconnect();
    };

    // onerror is always followed by onclose, which handles the reconnect.
    socket.onerror = () => {};
  }

  #armLiveness() {
    clearTimeout(this.#livenessTimer);
    this.#livenessTimer = setTimeout(() => {
      // A half-open socket after a Wi-Fi drop looks exactly like an idle
      // Board, except that the Board promises a message every tick
      // interval. Treat the socket as dead and reconnect now rather than
      // wait for TCP to notice.
      const socket = this.#socket;
      this.#socket = null;
      socket?.close();
      this.#setState({ connection: 'offline', pending: null });
      this.#scheduleReconnect();
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
    const { connection } = this.#state;
    if (this.#closed || connection === 'online' || connection === 'replaced') return;
    this.#attempt = 0;
    clearTimeout(this.#reconnectTimer);
    this.#open();
  }

  #handle(message) {
    // `seq` counts every Board -> page message on this connection, ticks
    // and errors included. A snapshot is always 0 and resets the count;
    // anything else must be exactly one past the last. `#seq === null`
    // means "waiting for a snapshot": right after connecting, and after a
    // gap. In that state every other message is discarded, because patches
    // already on their way apply to a state this page no longer has.
    if (message.t !== 'snapshot') {
      if (this.#seq === null) return;
      if (message.seq !== this.#seq + 1) {
        this.#seq = null;
        this.#send({ t: 'resync' });
        return;
      }
      this.#seq = message.seq;
    }

    switch (message.t) {
      case 'snapshot':
        if (message.build !== this.#build && this.#reloadForBuild(message.build)) return;
        this.#seq = message.seq;
        this.#setState({
          catalog: message.catalog,
          current: message.current,
          link: message.link,
        });
        break;
      case 'patch': {
        // Each section present replaces that section whole - no merging
        // inside it. Only `current` and `link` can change by patch: the
        // Catalog only ever arrives in a snapshot, so a `catalog` here is
        // ignored like any other unknown field.
        const sections = {};
        if (message.current) sections.current = message.current;
        if (message.link) sections.link = message.link;
        // The Board always sends a `current` patch when a switch completes,
        // even if the Current preset didn't change.
        if (message.current && message.current.index === this.#state.pending?.index) {
          sections.pending = null;
        }
        this.#setState(sections);
        break;
      }
      case 'error': {
        const update = { error: kErrorText[message.code] ?? kUnknownErrorText };
        if (message.id !== undefined && message.id === this.#state.pending?.id) {
          update.pending = null;
        }
        this.#setState(update);
        break;
      }
      default:
        // Both sides ignore message types they don't know: that is the
        // contract's extension point.
        break;
    }
  }

  // This tab is running JavaScript from another build than the Board's
  // firmware - typically a page loaded before a reflash that has since
  // reconnected on its own. Reload to fetch the Board's own page, but at
  // most once per Board build per tab: if the reload brings back the same
  // old page (a cache that ignores no-cache), reloading again would loop
  // forever. Returns whether it reloaded; if not, the caller applies the
  // snapshot and the page says it may be out of date.
  #reloadForBuild(boardBuild) {
    let alreadyReloaded = false;
    try {
      alreadyReloaded = this.#sessionStorage.getItem(kReloadedForBuildKey) === boardBuild;
      if (!alreadyReloaded) this.#sessionStorage.setItem(kReloadedForBuildKey, boardBuild);
    } catch {
      // Storage blocked (a private window, say). Without a record there is
      // no loop guard, so don't reload at all.
      alreadyReloaded = true;
    }
    if (alreadyReloaded) {
      this.#setState({ error: kPageOutdatedText });
      return false;
    }
    this.#location.reload();
    return true;
  }

  #send(message) {
    if (this.#socket?.readyState === this.#WebSocket.OPEN) {
      this.#socket.send(JSON.stringify(message));
    }
  }

  #setState(partial) {
    this.#state = { ...this.#state, ...partial };
    for (const listener of this.#listeners) listener(this.#state);
  }
}

// Where the WebSocket lives. Served from the Board, that's the same
// origin. Served from a Mac during development it is either the mock
// server on this same origin or, with ?board=<ip>, a real Board - which is
// what makes iterating on the UI without reflashing possible (ADR 0011
// question 7).
export function websocketUrl(location) {
  const board = new URLSearchParams(location.search).get('board');
  if (board) return `ws://${board}/ws`;
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${scheme}//${location.host}/ws`;
}
