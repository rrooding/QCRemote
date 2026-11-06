import { LitElement, html, css, classMap } from '../vendor/lit.js';
import { StoreController } from './store-controller.js';
import './qc-now-playing.js';
import './qc-preset-list.js';
import './qc-knob.js';

// The shell: a fixed header carrying the link state, a fixed "now playing"
// panel, and the scrolling preset list under it.
//
// The safe-area padding is on this element rather than on <body> because in
// iOS standalone mode the status bar and home indicator overlap the page, and
// the header is the thing that has to clear them. `100dvh` (not `vh`) is what
// keeps the layout from being sized for a viewport that includes Safari's
// collapsing toolbars.
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

    .dot.online {
      background: var(--qc-ok);
    }

    .dot.connecting {
      background: var(--qc-warn);
      animation: blink 1s steps(2, end) infinite;
    }

    .dot.offline {
      background: var(--qc-bad);
    }

    @keyframes blink {
      50% {
        opacity: 0.2;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .dot.connecting {
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

    .probe {
      display: flex;
      align-items: center;
      gap: 1.25rem;
      padding: 1rem 1.25rem 2rem;
      border-top: 1px solid var(--qc-line);
      color: var(--qc-text-dim);
      font-size: 0.75rem;
      line-height: 1.5;
    }

    .error {
      padding: 0.5rem 1rem;
      background: var(--qc-bad-dim);
      color: var(--qc-text);
      font-size: 0.75rem;
    }
  `;

  #store = new StoreController(this, window.qcStore);

  render() {
    const { link, error } = this.#store.state;
    return html`
      <header>
        <span class="title">QC Remote</span>
        <span class="link">
          <span class=${classMap({ dot: true, [link]: true })}></span>
          ${link}
        </span>
      </header>
      ${error ? html`<p class="error">${error}</p>` : html`<span></span>`}
      <div class="scroll">
        <qc-now-playing></qc-now-playing>
        <qc-preset-list></qc-preset-list>
        <div class="probe">
          <qc-knob label="Gain"></qc-knob>
          <qc-knob label="Master" .value=${0.68}></qc-knob>
          <p>
            Milestone 3 rendering probe: SVG dial, Pointer Events, drag
            vertically. Not connected to the board.
          </p>
        </div>
      </div>
    `;
  }
}

customElements.define('qc-app', QcApp);
