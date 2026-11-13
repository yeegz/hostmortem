// hmd: wires collectors -> queue -> writer -> store + detector, and owns the
// process lifecycle (signalfd, ordered shutdown).
#pragma once
#include <chrono>
#include <optional>

#include "core/config.hpp"
#include "core/result.hpp"

namespace hm {

struct AgentOptions {
  Config config;
  std::optional<std::chrono::milliseconds> run_for;  // lab/testing: stop after this long
  bool enable_kmsg = true;
};

// Returns the process exit code.
int run_agent(const AgentOptions& opts);

}  // namespace hm
