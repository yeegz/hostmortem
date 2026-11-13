#include "core/log.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace hm::log {
namespace {
std::atomic<int> g_level{static_cast<int>(Level::info)};
std::mutex g_mu;  // keeps lines whole when several threads log at once

const char* name(Level l) {
  switch (l) {
    case Level::debug: return "debug";
    case Level::info: return "info";
    case Level::warn: return "warn";
    case Level::error: return "error";
  }
  return "info";
}

// Escape control characters so hostile strings (interface names, kernel
// messages) cannot forge log lines.
std::string sanitize(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\x%02x", u);
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}
}  // namespace

void set_level(Level l) noexcept { g_level.store(static_cast<int>(l)); }

void write(Level l, std::string_view component, std::string_view message) noexcept {
  if (static_cast<int>(l) < g_level.load()) return;
  try {
    const std::string line = std::string("level=") + name(l) + " component=" + sanitize(component) +
                             " msg=\"" + sanitize(message) + "\"\n";
    std::lock_guard lock(g_mu);
    std::fputs(line.c_str(), stderr);
  } catch (...) {
    // Logging must never take the agent down.
  }
}

}  // namespace hm::log
