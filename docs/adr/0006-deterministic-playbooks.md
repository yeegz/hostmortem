# ADR-0006: Deterministic playbooks; "unknown" is a first-class answer

**Context.** Five of the nine validated public incidents behind this project
were misattributed at first or never resolved. A diagnostic tool that is
confidently wrong is worse than none.

**Decision.** Twelve playbooks, each a pure function over the incident window.
Each one adds integer evidence points for, records contradicting evidence
against, and lists missing telemetry. Fixed thresholds: 4 or more points is
supported, 1 to 3 is possible, and a decisive contradiction rules the
hypothesis out. The overall conclusion is "unknown" when nothing is supported,
or when the top two supported hypotheses are within one point. Every
non-ruled-out hypothesis must cite at least one stored event id
(`validate_citations`, enforced in tests and in the scenario harness).
Scores are never shown as percentages.

**AI.** None in the MVP. A later summariser may only rephrase structured
output, and a validator must reject any sentence that does not cite an
existing event id.

**Consequences.** Playbooks are unit-testable without Linux, and the same event
stream always yields the same diagnosis. The cost is that real-world rules
are hand-tuned. The evaluation harness (20 randomised runs per scenario,
release gate: zero confident wrong attributions) comes in v0.3.
