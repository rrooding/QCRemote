import { LitElement, html, css, classMap } from '../vendor/lit.js';
import { StoreController } from './store-controller.js';
import { connectionStatus } from './store.js';
import './qc-status.js';
import './qc-preset-list.js';

// The shell: a fixed header carrying the connection state (Board
// unreachable / Quad Cortex disconnected / connected, see
// connectionStatus()), a fixed status panel, and the scrolling preset list
// under it.
//
// The safe-area padding is on this element rather than on <body> because in
// iOS standalone mode the status bar and home indicator overlap the page,
// and the header is the thing that has to clear them. `100dvh` (not `vh`)
// is what keeps the layout from being sized for a viewport that includes
// Safari's collapsing toolbars.
export class QcApp extends LitElement {
  static styles = css`
    :host {
      display: grid;
      grid-template-rows: auto auto 1fr;
      height: 100dvh;
      padding-top: env(safe-area-inset-top);
      padding-bottom: env(safe-area-inset-bottom);
      background: var(--qc-bg);
    }

    header {
      display: flex;
      align-items: center;
      gap: 0.6rem;
      padding: 0.7rem 1rem;
      background: var(--qc-surface);
      border-bottom: 1px solid var(--qc-line);
    }

    .title {
      flex: 1;
      font-size: 0.85rem;
      font-weight: 600;
      letter-spacing: 0.02em;
      color: var(--qc-text);
    }

    .link {
      display: flex;
      align-items: center;
      gap: 0.4rem;
      font-size: 0.7rem;
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--qc-text-dim);
    }

    .dot {
      width: 0.5rem;
      height: 0.5rem;
      border-radius: 50%;
      background: var(--qc-text-dim);
    }

    .dot.connected {
      background: var(--qc-ok);
    }

    .dot.qcDisconnected {
      background: var(--qc-warn);
    }

    .dot.connecting,
    .dot.unreachable {
      animation: blink 1s steps(2, end) infinite;
    }

    .dot.connecting {
      background: var(--qc-warn);
    }

    .dot.unreachable,
    .dot.replaced {
      background: var(--qc-bad);
    }

    @keyframes blink {
      50% {
        opacity: 0.2;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .dot.connecting,
      .dot.unreachable {
        animation: none;
      }
    }

    .scroll {
      min-height: 0;
      overflow-y: auto;
      /* Keeps a flick at the end of the list from rubber-banding the whole
         page, which in standalone mode looks like the app coming unstuck. */
      overscroll-behavior: contain;
      -webkit-overflow-scrolling: touch;
    }

    /* What is shown while the Board is unreachable is the last state it
       reported, not its current one. */
    .scroll.stale {
      opacity: 0.55;
      transition: opacity 0.2s;
    }

    .error {
      display: flex;
      align-items: center;
      gap: 0.75rem;
      margin: 0;
      padding: 0.5rem 1rem;
      background: var(--qc-bad-dim);
      color: var(--qc-text);
      font-size: 0.75rem;
    }

    .error span {
      flex: 1;
    }

    button {
      min-height: 44px;
      padding: 0 1rem;
      border: 1px solid var(--qc-line);
      border-radius: 8px;
      background: var(--qc-surface-2);
      color: var(--qc-text);
      font: inherit;
      font-weight: 600;
    }
  `;

  #store = new StoreController(this, window.qcStore);

  render() {
    const { connection, error } = this.#store.state;
    const status = connectionStatus(this.#store.state);
    return html`
      <header>
        <span class="title">QC Remote</span>
        <span class="link" role="status">
          <span class=${classMap({ dot: true, [status.kind]: true })}></span>
          ${status.label}
        </span>
      </header>
      ${error
        ? html`<p class="error" role="status">
            <span>${error}</span>
            ${connection === 'replaced'
              ? html`<button @click=${() => this.#store.takeOver()}>Use here</button>`
              : null}
          </p>`
        : html`<span></span>`}
      <div class=${classMap({ scroll: true, stale: status.kind === 'unreachable' })}>
        <qc-status></qc-status>
        <qc-preset-list></qc-preset-list>
      </div>
    `;
  }
}

customElements.define('qc-app', QcApp);
