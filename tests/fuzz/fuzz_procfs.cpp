#include <cstdint>
#include <string_view>

#include "collectors/procfs_parse.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view s(reinterpret_cast<const char*>(data), size);
  using namespace hm::procfs;
  (void)parse_stat_cpu(s);
  (void)parse_meminfo(s);
  (void)parse_psi(s);
  (void)parse_snmp(s);
  (void)parse_conntrack_stat(s);
  (void)parse_default_route(s);
  (void)parse_resolv_conf(s);
  return 0;
}
