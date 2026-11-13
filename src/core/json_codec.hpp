// Event <-> JSON. Used for the attrs column, JSONL export/ingest and the
// CLI's --json output. Ingest treats input as untrusted: size and shape are
// validated and failures are returned, never thrown.
#pragma once
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "core/event.hpp"
#include "core/result.hpp"

namespace hm {

inline constexpr std::size_t kMaxEventJsonBytes = 64 * 1024;
inline constexpr std::size_t kMaxAttrs = 64;
inline constexpr std::size_t kMaxStringBytes = 4096;

std::string attrs_to_json(const Attrs& attrs);
Result<Attrs> attrs_from_json(std::string_view text);

nlohmann::json event_to_json(const Event& e);
std::string event_to_json_line(const Event& e);
Result<Event> event_from_json_line(std::string_view line);

}  // namespace hm
