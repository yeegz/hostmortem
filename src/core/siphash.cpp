#include "core/siphash.hpp"

#include <cstdio>

namespace hm {
namespace {

constexpr std::uint64_t rotl(std::uint64_t x, int b) noexcept {
  return (x << b) | (x >> (64 - b));
}

std::uint64_t load_le64(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

struct State {
  std::uint64_t v0, v1, v2, v3;
  void round() noexcept {
    v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
    v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
    v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
    v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
  }
};

}  // namespace

std::uint64_t siphash24(const SipKey& key, std::string_view data) noexcept {
  const std::uint64_t k0 = load_le64(key.data());
  const std::uint64_t k1 = load_le64(key.data() + 8);
  State s{k0 ^ 0x736f6d6570736575ULL, k1 ^ 0x646f72616e646f6dULL,
          k0 ^ 0x6c7967656e657261ULL, k1 ^ 0x7465646279746573ULL};

  const auto* in = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::size_t len = data.size();
  const std::size_t full = len - (len % 8);
  for (std::size_t i = 0; i < full; i += 8) {
    const std::uint64_t m = load_le64(in + i);
    s.v3 ^= m;
    s.round();
    s.round();
    s.v0 ^= m;
  }
  std::uint64_t b = static_cast<std::uint64_t>(len) << 56;
  for (std::size_t i = 0; i < len % 8; ++i) {
    b |= static_cast<std::uint64_t>(in[full + i]) << (8 * i);
  }
  s.v3 ^= b;
  s.round();
  s.round();
  s.v0 ^= b;
  s.v2 ^= 0xff;
  for (int i = 0; i < 4; ++i) s.round();
  return s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
}

std::string hash_name(const SipKey& key, std::string_view name) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "h:%016llx",
                static_cast<unsigned long long>(siphash24(key, name)));
  return buf;
}

}  // namespace hm
