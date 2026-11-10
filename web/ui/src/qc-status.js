import { LitElement, html, css } from '../vendor/lit.js';
import { StoreController } from './store-controller.js';

// What the board has actually told us, live. This mirrors what the inline
// test page this replaces showed (QcWifiWebTransport.hpp's kIndexHtml,
// before issue #88): a catalog summary and the last preset-change event's
// command/count - the board has no "currently loaded preset" on this wire
// yet (see store.js's own header comment), so that pair is genuinely the
// most "live" information there is to show. Preset identity by index/name
// is issue #15's job once the board emits it.
export class QcStatus extends LitElement {
  static styles = css`
    :host {
      display: block;
      padding: 1rem 1rem 1.1rem;
      background: var(--qc-surface);
      border-bottom: 1px solid var(--qc-line);
    }

    .headline {
      margin: 0;
      font-size: 1.1rem;
      font-weight: 600;
      line-height: 1.25;
      color: var(--qc-text);
    }

    .headline.idle {
      color: var(--qc-text-dim);
      font-weight: 500;
    }

    .meta {
      display: flex;
      flex-wrap: wrap;
      align-items: baseline;
      gap: 0.5rem;
      margin-top: 0.6rem;
      font-size: 0.75rem;
      color: var(--qc-text-dim);
      font-variant-numeric: tabular-nums;
    }
  `;

  #store = new StoreController(this, window.qcStore);

  render() {
    const { catalog, lastChange } = this.#store.state;
    return html`
      ${lastChange
        ? html`<p class="headline">Preset change (cmd ${lastChange.command}, #${lastChange.count})</p>`
        : html`<p class="headline idle">Waiting for a preset change&hellip;</p>`}
      <p class="meta">
        <span>${catalog.presetCount} presets</span>
        <span>generation ${catalog.generation}</span>
        ${catalog.incomplete ? html`<span>listing incomplete</span>` : null}
      </p>
    `;
  }
}

customElements.define('qc-status', QcStatus);
