import { LitElement, html, css, repeat, classMap } from '../vendor/lit.js';
import { StoreController } from './store-controller.js';

const kSceneLetters = ['A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'];

// What is loaded right now, live. This is the part the owner watches while
// playing, so it is the part that has to update without a reload.
//
// The scene row is drawn here as a probe for the Milestone 3 layout, not as a
// working control: the board has no scene message yet (#15), so the buttons
// render the board's reported scene and otherwise do nothing. Drawing them
// now is what makes it concrete that the QC's layout can be reproduced in
// plain CSS without any of its artwork.
export class QcNowPlaying extends LitElement {
  static styles = css`
    :host {
      display: block;
      padding: 1rem 1rem 1.1rem;
      background: var(--qc-surface);
      border-bottom: 1px solid var(--qc-line);
    }

    .folder {
      font-size: 0.7rem;
      font-weight: 600;
      letter-spacing: 0.14em;
      text-transform: uppercase;
      color: var(--qc-text-dim);
    }

    .name {
      margin: 0.15rem 0 0;
      font-size: 1.6rem;
      font-weight: 600;
      line-height: 1.15;
      letter-spacing: -0.01em;
      color: var(--qc-text);
      /* Two lines, then ellipsis: preset names run long and a reflowing
         header makes the list below jump on every change. */
      display: -webkit-box;
      -webkit-line-clamp: 2;
      -webkit-box-orient: vertical;
      overflow: hidden;
    }

    .name.idle {
      color: var(--qc-text-dim);
      font-weight: 500;
    }

    .scenes {
      display: flex;
      gap: 0.4rem;
      margin: 0.9rem 0 0;
      padding: 0;
      list-style: none;
    }

    .scene {
      flex: 1;
      display: grid;
      place-items: center;
      min-width: 0;
      height: 2.25rem;
      border-radius: 8px;
      background: var(--qc-surface-2);
      box-shadow: inset 0 0 0 1px var(--qc-line);
      color: var(--qc-text-dim);
      font-size: 0.8rem;
      font-weight: 600;
      font-variant-numeric: tabular-nums;
    }

    .scene.active {
      background: var(--qc-accent-dim);
      box-shadow: inset 0 0 0 1px var(--qc-accent);
      color: var(--qc-text);
    }

    .meta {
      display: flex;
      align-items: baseline;
      gap: 0.5rem;
      margin-top: 0.8rem;
      font-size: 0.75rem;
      color: var(--qc-text-dim);
      font-variant-numeric: tabular-nums;
    }
  `;

  #store = new StoreController(this, window.qcStore);

  render() {
    const { catalog, current, pending, qcConnected } = this.#store.state;
    const folder = current.folder !== null ? catalog.folders[current.folder] : undefined;
    const name = current.name ?? (qcConnected ? 'No preset reported yet' : 'Quad Cortex not attached');

    return html`
      <p class="folder">${folder?.name ?? 'Current preset'}</p>
      <h1 class=${classMap({ name: true, idle: current.name == null })}>${name}</h1>
      <ol class="scenes">
        ${repeat(
          kSceneLetters,
          (letter) => letter,
          (letter, i) => html`<li class=${classMap({ scene: true, active: current.scene === i })}>
            ${letter}
          </li>`
        )}
      </ol>
      <p class="meta">
        <span>${catalog.presetCount} presets</span>
        <span>generation ${catalog.generation}</span>
        ${catalog.incomplete ? html`<span>listing incomplete</span>` : null}
        ${pending !== null ? html`<span>switching to ${pending}&hellip;</span>` : null}
      </p>
    `;
  }
}

customElements.define('qc-now-playing', QcNowPlaying);
