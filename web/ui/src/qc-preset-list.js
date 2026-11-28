import { LitElement, html, css, repeat } from '../vendor/lit.js';
import { StoreController } from './store-controller.js';

// The Quad Cortex's own folder/preset structure, drawn with our own CSS
// (ADR 0011 decision 8 - no Neural DSP artwork, icons or fonts).
//
// The Current preset's row is marked (aria-current). Tapping a row asks the
// Board to switch to it (issue #78): the row is marked pending (aria-busy),
// and the Current preset mark only moves when the Board reports the switch
// - confirmed, not optimistic (ADR 0011 decision 5).
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

    button {
      display: flex;
      align-items: center;
      gap: 0.75rem;
      width: 100%;
      /* 56px: comfortably past the ~44px minimum tap target (ADR 0011
         question 6). */
      min-height: 56px;
      padding: 0 0.75rem;
      border: 0;
      border-radius: 10px;
      background: none;
      color: var(--qc-text);
      font: inherit;
      text-align: left;
      cursor: pointer;
      touch-action: manipulation;
      -webkit-tap-highlight-color: transparent;
    }

    button:active {
      background: var(--qc-surface-2);
    }

    button:focus-visible {
      outline: 2px solid var(--qc-accent);
      outline-offset: -2px;
    }

    button:disabled {
      cursor: default;
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

    button[aria-current='true'] {
      background: var(--qc-accent-dim);
    }

    button[aria-current='true'] .slot {
      color: var(--qc-accent);
    }

    /* Waiting on the Board: an outline that breathes, not the Current mark. */
    button[aria-busy='true'] {
      box-shadow: inset 0 0 0 2px var(--qc-accent);
      animation: pending 1s ease-in-out infinite alternate;
    }

    @keyframes pending {
      from {
        box-shadow: inset 0 0 0 2px var(--qc-accent-dim);
      }
    }

    @media (prefers-reduced-motion: reduce) {
      button[aria-busy='true'] {
        animation: none;
      }
    }

    .empty {
      padding: 2rem 1rem;
      color: var(--qc-text-dim);
      text-align: center;
    }
  `;

  #store = new StoreController(this, window.qcStore);

  render() {
    const { catalog, current, pending, connection } = this.#store.state;
    // While one switch is pending (or there's no Board to ask) the other
    // rows can't be picked - the store would ignore the tap anyway, so
    // saying so is more honest than a button that does nothing.
    const rowsDisabled = pending !== null || connection !== 'online';
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
                <button
                  type="button"
                  aria-current=${preset.index === current.index ? 'true' : 'false'}
                  aria-busy=${preset.index === pending?.index ? 'true' : 'false'}
                  ?disabled=${rowsDisabled}
                  @click=${() => this.#store.switchPreset(preset.index)}
                >
                  <span class="slot">${preset.index}</span>
                  <span class="name">${preset.name}</span>
                </button>
              </li>
            `
          )}
        </ol>
      `
    )}`;
  }
}

customElements.define('qc-preset-list', QcPresetList);
