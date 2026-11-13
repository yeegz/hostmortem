// Minimal DNS wire format (RFC 1035) for the direct-upstream probe: build an
// A/AAAA query and validate a response header. We deliberately do not parse
// answers: the probe asks "did the server answer this query, with what
// rcode", which is enough to separate local resolver faults from upstream ones.
#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/result.hpp"

namespace hm::dns {

Result<std::vector<std::uint8_t>> build_query(std::uint16_t id, std::string_view name, std::uint16_t qtype = 1);

struct Header {
  std::uint16_t id = 0;
  bool response = false;
  bool truncated = false;
  int rcode = 0;
  std::uint16_t qdcount = 0, ancount = 0;
};

std::optional<Header> parse_header(const std::uint8_t* data, std::size_t len);

const char* rcode_name(int rcode) noexcept;

}  // namespace hm::dns
