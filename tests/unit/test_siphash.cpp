#include <gtest/gtest.h>

#include "core/siphash.hpp"

using namespace hm;

// Reference vectors from the SipHash paper's test suite (key 00..0f,
// message 00..len-1).
TEST(SipHash, MatchesReferenceVectors) {
  SipKey k;
  for (std::size_t i = 0; i < 16; ++i) k[i] = static_cast<std::uint8_t>(i);
  EXPECT_EQ(siphash24(k, ""), 0x726fdb47dd0e0e31ULL);
  std::string msg;
  for (int i = 0; i < 15; ++i) msg.push_back(static_cast<char>(i));
  EXPECT_EQ(siphash24(k, msg), 0xa129ca6149be45e5ULL);
}

TEST(SipHash, NameHashIsKeyed) {
  SipKey a{}, b{};
  b[0] = 1;
  EXPECT_NE(hash_name(a, "example.com"), hash_name(b, "example.com"));
  EXPECT_EQ(hash_name(a, "example.com"), hash_name(a, "example.com"));
  EXPECT_EQ(hash_name(a, "example.com").rfind("h:", 0), 0u);
}
