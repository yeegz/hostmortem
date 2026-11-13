#include <cstdint>
#include <cstring>
#include <vector>

#include "collectors/netlink_parse.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // recv() buffers are 4-byte aligned; copy into an aligned buffer.
  std::vector<std::uint32_t> buf((size + 3) / 4);
  if (size) std::memcpy(buf.data(), data, size);
  bool bad = false;
  (void)hm::nl::parse(buf.data(), size, &bad);
  return 0;
}
