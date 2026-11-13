#include <cstdint>
#include <string_view>

#include "collectors/dns_wire.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  (void)hm::dns::parse_header(data, size);
  (void)hm::dns::build_query(1, std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
