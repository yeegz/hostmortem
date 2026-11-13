# Contributing

1. Build and test in the dev container: `scripts/dev.sh 'cmake --preset debug && cmake --build --preset debug && ctest --preset debug'`.
2. Every PR keeps `-Wall -Wextra -Wconversion -Werror` clean and passes the
   `asan` and `tsan` presets.
3. New collectors report `collector.status` with a reason when degraded (FR6).
   New playbooks cite event ids and come with a scenario fixture in
   `lab/gen_fixtures.py` plus, where possible, a real-fault lab scenario.
4. Never delete or weaken a failing test to make CI green.
5. Decisions with alternatives get an ADR in `docs/adr/`.
6. Measurements go in `docs/PERFORMANCE.md` only with the script that produced them.
