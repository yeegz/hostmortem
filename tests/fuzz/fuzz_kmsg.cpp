#include <cstdint>
#include <string_view>

#include "collectors/kmsg_parse.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  auto r = hm::kmsg::parse_record(std::string_view(reinterpret_cast<const char*>(data), size));
  if (r) (void)hm::kmsg::classify(*r);
  return 0;
}
