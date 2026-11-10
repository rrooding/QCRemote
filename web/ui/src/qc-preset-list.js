import { LitElement, html, css, repeat } from '../vendor/lit.js';
import { StoreController } from './store-controller.js';

// The Quad Cortex's own folder/preset structure, drawn with our own CSS
// (ADR 0011 decision 8 - no Neural DSP artwork, icons or fonts).
//
// Read-only: the board has no "currently loaded preset" on this wire (see
// store.js's own header comment) and tapping a row to switch presets is
// issue #78's job, not this one's - rendering rows as fake buttons with no
// handler would be a lie to a screen reader, so these are plain list items.
//
// `repeat()` rather than a plain `.map()`: keyed by the board's own preset
// index, so a catalog update that changes one name patches one <li>
// instead of re-creating 48 of them.
export class QcPresetList extends LitElement {
  static styles = css`
    :host {
      display: block;
    }

    h2 {
      position: sticky;
      top: 0;
      z-index: 1;
      margin: 0;
      padding: 0.75rem 1rem 0.4rem;
      font-size: 0.7rem;
      font-weight: 600;
      letter-spacing: 0.14em;
      text-transform: uppercase;
      color: var(--qc-text-dim);
      background: linear-gradient(var(--qc-bg) 70%, transparent);
      backdrop-filter: blur(6px);
    }

    ol {
      margin: 0 0 0.5rem;
      padding: 0 0.5rem;
      list-style: none;
    }

    li {
      display: flex;
      align-items: center;
      gap: 0.75rem;
      /* 56px: comfortably past the ~44px minimum tap target (ADR 0011
         question 6) - kept even though these rows aren't tappable yet, so
         the list doesn't have to be re-sized once #78 wires taps back in. */
      min-height: 56px;
      padding: 0 0.75rem;
      border-radius: 10px;
      color: var(--qc-text);
    }

    .slot {
      flex: none;
      width: 2.25rem;
      font-variant-numeric: tabular-nums;
      font-size: 0.8rem;
      color: var(--qc-text-dim);
      text-align: right;
    }

    .name {
      flex: 1;
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      font-size: 1rem;
    }

    .empty {
      padding: 2rem 1rem;
      color: var(--qc-text-dim);
      text-align: center;
    }
  `;

  #store = new StoreController(this, window.qcStore);

  render() {
    const { catalog } = this.#store.state;
    if (catalog.folders.length === 0) {
      return html`<p class="empty">
        No presets yet${catalog.incomplete ? ' - waiting for the board to finish listing' : ''}.
      </p>`;
    }

    return html`${repeat(
      catalog.folders,
      (folder, i) => `${i}:${folder.name}`,
      (folder) => html`
        <h2>${folder.name}</h2>
        <ol>
          ${repeat(
            folder.presets,
            (preset) => preset.index,
            (preset) => html`
              <li>
                <span class="slot">${preset.index}</span>
                <span class="name">${preset.name}</span>
              </li>
            `
          )}
        </ol>
      `
    )}`;
  }
}

customElements.define('qc-preset-list', QcPresetList);
