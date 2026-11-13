#include <cstdint>
#include <string_view>

#include "core/json_codec.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view s(reinterpret_cast<const char*>(data), size);
  auto e = hm::event_from_json_line(s);
  if (e) {
    // Anything accepted must round-trip.
    auto again = hm::event_from_json_line(hm::event_to_json_line(*e));
    if (!again) __builtin_trap();
  }
  (void)hm::attrs_from_json(s);
  return 0;
}
