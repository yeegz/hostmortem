#include <cstdint>
#include <string_view>

#include "core/config.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  (void)hm::parse_config(std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
