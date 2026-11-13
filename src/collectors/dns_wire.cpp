#include "collectors/dns_wire.hpp"

namespace hm::dns {

Result<std::vector<std::uint8_t>> build_query(std::uint16_t id, std::string_view name, std::uint16_t qtype) {
  if (!name.empty() && name.back() == '.') name.remove_suffix(1);
  if (name.empty() || name.size() > 253) return make_error("bad_name", "DNS name length");
  std::vector<std::uint8_t> q;
  q.reserve(18 + name.size());
  auto put16 = [&](std::uint16_t v) {
    q.push_back(static_cast<std::uint8_t>(v >> 8));
    q.push_back(static_cast<std::uint8_t>(v & 0xff));
  };
  put16(id);
  put16(0x0100);  // RD
  put16(1);       // QDCOUNT
  put16(0);
  put16(0);
  put16(0);
  while (!name.empty()) {
    const auto dot = name.find('.');
    const std::string_view label = name.substr(0, dot);
    if (label.empty() || label.size() > 63) return make_error("bad_name", "DNS label length");
    q.push_back(static_cast<std::uint8_t>(label.size()));
    q.insert(q.end(), label.begin(), label.end());
    if (dot == std::string_view::npos) break;
    name.remove_prefix(dot + 1);
    if (name.empty()) return make_error("bad_name", "empty trailing label");
  }
  q.push_back(0);
  put16(qtype);
  put16(1);  // IN
  return q;
}

std::optional<Header> parse_header(const std::uint8_t* d, std::size_t len) {
  if (d == nullptr || len < 12) return std::nullopt;
  Header h;
  h.id = static_cast<std::uint16_t>((d[0] << 8) | d[1]);
  h.response = (d[2] & 0x80) != 0;
  h.truncated = (d[2] & 0x02) != 0;
  h.rcode = d[3] & 0x0f;
  h.qdcount = static_cast<std::uint16_t>((d[4] << 8) | d[5]);
  h.ancount = static_cast<std::uint16_t>((d[6] << 8) | d[7]);
  return h;
}

const char* rcode_name(int rcode) noexcept {
  switch (rcode) {
    case 0: return "NOERROR";
    case 1: return "FORMERR";
    case 2: return "SERVFAIL";
    case 3: return "NXDOMAIN";
    case 4: return "NOTIMP";
    case 5: return "REFUSED";
    default: return "OTHER";
  }
}

}  // namespace hm::dns
