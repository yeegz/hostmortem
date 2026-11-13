// Minimal structured logger for the agent's own diagnostics (stderr, one
// line per record, key=value). Agent *evidence* goes to the store, not here.
#pragma once
#include <string_view>

namespace hm::log {

enum class Level { debug, info, warn, error };
void set_level(Level l) noexcept;
void write(Level l, std::string_view component, std::string_view message) noexcept;

inline void debug(std::string_view c, std::string_view m) noexcept { write(Level::debug, c, m); }
inline void info(std::string_view c, std::string_view m) noexcept { write(Level::info, c, m); }
inline void warn(std::string_view c, std::string_view m) noexcept { write(Level::warn, c, m); }
inline void error(std::string_view c, std::string_view m) noexcept { write(Level::error, c, m); }

}  // namespace hm::log
