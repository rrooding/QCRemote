# ADR 0001: Record architecture decisions as ADRs

Date: 2026-09-14
Status: Accepted

## Context

Early backlog issues (devkit choice, BLE transport choice) originally said to write their
outcome into a `docs/*.md` file (`docs/OPEN-QUESTIONS.md`, `docs/DEVKIT-DECISION.md`).
That works until the file accumulates enough unrelated decisions that finding *why* a past
choice was made — not just what it was — gets hard, and there's no consistent place to note
that a later decision superseded an earlier one.

## Decision

Consequential architecture and tooling decisions (devkit/RTOS choice, BLE transport choice,
language/standard version, similar one-way-door calls) get recorded as one ADR per decision
under `docs/adr/`, numbered sequentially (`0001-`, `0002-`, ...), using the format in
[`docs/adr/template.md`](template.md). Day-to-day implementation notes, protocol findings,
and anything that isn't itself a decision (e.g. "here's what we learned porting the
protocol") stay as regular docs, not ADRs.

A superseded ADR isn't deleted or edited to reflect the new choice — it's left as-is with its
Status changed to `Superseded by ADR-XXXX`, and the new ADR references it back. The history
of *why* something changed is the point.

## Consequences

Anyone asking "why does the firmware target Zephyr" or "why BLE-MIDI instead of custom GATT"
has one file to read instead of hunting through issue threads or a general notes file. The
cost is one more file per real decision and a small amount of ceremony (numbering, the
Context/Decision/Consequences shape) for calls that turn out to be trivial in hindsight —
acceptable since the format itself is deliberately short, not a heavyweight RFC process.
