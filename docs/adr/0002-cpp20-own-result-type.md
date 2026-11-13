# ADR-0002: C++20 with a small Result<T>

**Context.** Targets include Ubuntu 22.04 (GCC 11), which lacks
`std::expected` (C++23). Exceptions crossing threads are hard to reason about
and the research rules forbid them at module edges.

**Decision.** C++20. Errors are values: `hm::Result<T>` (a 40-line
`std::variant<T, Error>` wrapper) at every module boundary; `Error` carries a
stable machine code (`sqlite_full`) and a human message. Collector threads
catch anything that escapes `run()` and log it; nothing propagates across a
thread.

**Alternatives.** `tl::expected` (fine, but an extra dependency for one
type); exceptions (rejected for thread boundaries); error codes plus out-params
(rejected: easy to ignore; `Result` is `[[nodiscard]]`).

**Consequences.** Swap to `std::expected` when the floor moves to GCC 12+ with
`-std=c++23`; the interface is deliberately the same shape.
