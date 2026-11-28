import { LitElement, html, css } from '../vendor/lit.js';
import { StoreController } from './store-controller.js';

// What the Board has told us, live: the Current preset, the Link to the
// Quad Cortex, and a Catalog summary (docs/api/README.md).
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
    const { catalog, current, link } = this.#store.state;
    return html`
      ${current.name !== null
        ? html`<p class="headline">${current.name}</p>`
        : html`<p class="headline idle">No preset reported yet</p>`}
      <p class="meta">
        ${current.index !== null ? html`<span>#${current.index}</span>` : null}
        <span>${link.qcConnected ? 'Quad Cortex connected' : 'Quad Cortex not connected'}</span>
        <span>${catalog.presetCount} presets</span>
        <span>generation ${catalog.generation}</span>
        ${catalog.incomplete ? html`<span>listing incomplete</span>` : null}
      </p>
    `;
  }
}

customElements.define('qc-status', QcStatus);
