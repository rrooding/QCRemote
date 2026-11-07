import { LitElement, html, css, repeat, classMap } from '../vendor/lit.js';
import { StoreController } from './store-controller.js';

// The preset list: the Quad Cortex's own folder/preset structure, drawn with
// our own CSS (issue #84 question 8 - no Neural DSP artwork, icons or fonts).
//
// `repeat()` rather than a plain `.map()`: keyed by the board's own preset
// index, so a catalog update that changes one name patches one <li> instead
// of re-creating 48 of them. That matters less at 48 presets than it will at
// a full setlist, but the keying is free and the alternative is a list that
// loses scroll position and focus on every push.
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
      /* 56px: comfortably past the ~44px minimum tap target, because this is
         the control you reach for mid-song. */
      min-height: 56px;
      padding: 0 0.75rem;
      border-radius: 10px;
      color: var(--qc-text);
      /* Only transform/opacity/background are animated anywhere in this PoC -
         they stay off the layout path, which is what keeps a flick-scroll at
         60 fps on a phone. */
      transition: background-color 120ms ease-out, transform 90ms ease-out;
      -webkit-tap-highlight-color: transparent;
      touch-action: manipulation;
      cursor: pointer;
      user-select: none;
    }

    li:active {
      transform: scale(0.985);
      background: var(--qc-surface-2);
    }

    li.current {
      background: var(--qc-surface-2);
      box-shadow: inset 0 0 0 1px var(--qc-accent-dim);
    }

    li.pending {
      /* Deliberately a different signal from .current: the tap was sent and
         the board hasn't answered yet. See Store#switchPreset. */
      background: var(--qc-surface-2);
      animation: pulse 900ms ease-in-out infinite;
    }

    @keyframes pulse {
      50% {
        opacity: 0.55;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      li,
      li.pending {
        transition: none;
        animation: none;
      }
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

    .mark {
      flex: none;
      width: 0.5rem;
      height: 0.5rem;
      border-radius: 50%;
      background: var(--qc-accent);
      opacity: 0;
    }

    li.current .mark {
      opacity: 1;
    }

    .empty {
      padding: 2rem 1rem;
      color: var(--qc-text-dim);
      text-align: center;
    }
  `;

  #store = new StoreController(this, window.qcStore);

  render() {
    const { catalog, current, pending } = this.#store.state;
    if (catalog.folders.length === 0) {
      return html`<p class="empty">
        No presets yet${catalog.incomplete ? ' - waiting for the board to finish listing' : ''}.
      </p>`;
    }

    return html`${repeat(
      catalog.folders,
      (folder, i) => `${i}:${folder.name}`,
      (folder, folderIndex) => html`
        <h2>${folder.name}</h2>
        <ol>
          ${repeat(
            folder.presets,
            (preset) => preset.index,
            (preset) => html`
              <li
                class=${classMap({
                  current: current.index === preset.index,
                  pending: pending === preset.index,
                })}
                role="button"
                tabindex="0"
                aria-current=${current.index === preset.index ? 'true' : 'false'}
                @click=${() => this.#select(folderIndex, preset.index)}
                @keydown=${(event) => this.#onKeydown(event, folderIndex, preset.index)}
              >
                <span class="slot">${preset.index}</span>
                <span class="name">${preset.name}</span>
                <span class="mark"></span>
              </li>
            `
          )}
        </ol>
      `
    )}`;
  }

  #select(folderIndex, presetIndex) {
    this.#store.store.switchPreset(folderIndex, presetIndex);
  }

  #onKeydown(event, folderIndex, presetIndex) {
    // A list this is driven by touch still has to work from a Mac keyboard,
    // and role="button" without Enter/Space handling is a lie to a screen
    // reader.
    if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      this.#select(folderIndex, presetIndex);
    }
  }
}

customElements.define('qc-preset-list', QcPresetList);
