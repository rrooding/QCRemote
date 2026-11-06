import { LitElement, html, svg, css } from '../vendor/lit.js';

// A rendering probe for Milestone 3's block parameters, not a wired control.
// It exists in this PoC to answer issue #84's question 2 with something that
// can actually be dragged on a phone rather than an opinion:
//
// - The dial is SVG, so it is one declarative arc that scales to any DPI and
//   needs no redraw loop. Canvas would mean owning a render loop and
//   re-implementing hit testing; DOM/CSS alone can draw the ring (conic
//   gradients) but not the tick marks and value arc as cleanly.
// - Dragging uses Pointer Events with setPointerCapture, which is one code
//   path for touch, mouse and pencil. `touch-action: none` is what stops iOS
//   from stealing the vertical drag to scroll the page - without it the knob
//   is unusable on a phone, and no amount of preventDefault() fixes it
//   reliably because the scroll starts on the compositor thread.
// - Only the arc's `stroke-dasharray` and the pointer's `transform` change
//   while dragging, both compositor-friendly, so a drag doesn't trigger
//   layout.
const kRadius = 34;
const kCircumference = 2 * Math.PI * kRadius;
// Sweep: 270 degrees, the usual amp-knob range, leaving a gap at the bottom.
const kSweepDeg = 270;
const kStartDeg = 135;
// Pixels of vertical drag for the full range. Vertical, not rotational,
// because a rotational gesture on a 70 px dial is unusable with a thumb.
const kDragRangePx = 160;

export class QcKnob extends LitElement {
  static properties = {
    label: { type: String },
    value: { type: Number },
  };

  static styles = css`
    :host {
      display: inline-grid;
      justify-items: center;
      gap: 0.35rem;
      touch-action: none;
      user-select: none;
      -webkit-tap-highlight-color: transparent;
    }

    svg {
      display: block;
      width: 78px;
      height: 78px;
      overflow: visible;
    }

    .track {
      fill: none;
      stroke: var(--qc-surface-2);
      stroke-width: 7;
      stroke-linecap: round;
    }

    .value {
      fill: none;
      stroke: var(--qc-accent);
      stroke-width: 7;
      stroke-linecap: round;
    }

    .hub {
      fill: var(--qc-surface-2);
      stroke: var(--qc-line);
    }

    .pointer {
      stroke: var(--qc-text);
      stroke-width: 2.5;
      stroke-linecap: round;
    }

    .label {
      font-size: 0.65rem;
      letter-spacing: 0.1em;
      text-transform: uppercase;
      color: var(--qc-text-dim);
    }

    .readout {
      font-size: 0.8rem;
      font-variant-numeric: tabular-nums;
      color: var(--qc-text);
    }
  `;

  #dragStartY = 0;
  #dragStartValue = 0;

  constructor() {
    super();
    this.label = 'Gain';
    this.value = 0.42;
  }

  render() {
    const dash = kCircumference * (kSweepDeg / 360);
    const filled = dash * this.value;
    const angle = (kStartDeg + kSweepDeg * this.value) * (Math.PI / 180);

    return html`
      <svg
        viewBox="0 0 80 80"
        role="slider"
        tabindex="0"
        aria-label=${this.label}
        aria-valuemin="0"
        aria-valuemax="100"
        aria-valuenow=${Math.round(this.value * 100)}
        @pointerdown=${this.#onPointerDown}
        @pointermove=${this.#onPointerMove}
        @pointerup=${this.#onPointerUp}
        @keydown=${this.#onKeydown}
      >
        ${svg`
          <g transform="rotate(${kStartDeg} 40 40)">
            <circle class="track" cx="40" cy="40" r=${kRadius}
                    stroke-dasharray="${dash} ${kCircumference}"></circle>
            <circle class="value" cx="40" cy="40" r=${kRadius}
                    stroke-dasharray="${filled} ${kCircumference}"></circle>
          </g>
          <circle class="hub" cx="40" cy="40" r="21"></circle>
          <line class="pointer" x1="40" y1="40"
                x2=${40 + Math.cos(angle) * 17} y2=${40 + Math.sin(angle) * 17}></line>
        `}
      </svg>
      <span class="readout">${Math.round(this.value * 100)}</span>
      <span class="label">${this.label}</span>
    `;
  }

  #onPointerDown(event) {
    event.target.setPointerCapture(event.pointerId);
    this.#dragStartY = event.clientY;
    this.#dragStartValue = this.value;
  }

  #onPointerMove(event) {
    if (!event.target.hasPointerCapture(event.pointerId)) return;
    const delta = (this.#dragStartY - event.clientY) / kDragRangePx;
    this.value = Math.min(1, Math.max(0, this.#dragStartValue + delta));
  }

  #onPointerUp(event) {
    event.target.releasePointerCapture(event.pointerId);
  }

  #onKeydown(event) {
    const step = event.shiftKey ? 0.01 : 0.05;
    if (event.key === 'ArrowUp' || event.key === 'ArrowRight') {
      this.value = Math.min(1, this.value + step);
    } else if (event.key === 'ArrowDown' || event.key === 'ArrowLeft') {
      this.value = Math.max(0, this.value - step);
    } else {
      return;
    }
    event.preventDefault();
  }
}

customElements.define('qc-knob', QcKnob);
