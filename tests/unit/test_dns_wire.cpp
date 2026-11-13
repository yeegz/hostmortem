#include <gtest/gtest.h>

#include "collectors/dns_wire.hpp"

using namespace hm;

TEST(DnsWire, BuildsQuery) {
  auto q = dns::build_query(0x1234, "example.com");
  ASSERT_TRUE(q.ok());
  const std::vector<std::uint8_t> want = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                                          7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
                                          0, 1, 0, 1};
  EXPECT_EQ(*q, want);
  EXPECT_TRUE(dns::build_query(1, "example.com.").ok());
}

TEST(DnsWire, RejectsBadNames) {
  EXPECT_FALSE(dns::build_query(1, "").ok());
  EXPECT_FALSE(dns::build_query(1, "a..b").ok());
  EXPECT_FALSE(dns::build_query(1, std::string(64, 'a') + ".com").ok());
  EXPECT_FALSE(dns::build_query(1, std::string(300, 'a')).ok());
}

TEST(DnsWire, ParsesHeader) {
  const std::uint8_t resp[] = {0x12, 0x34, 0x81, 0x83, 0, 1, 0, 0, 0, 0, 0, 0};
  auto h = dns::parse_header(resp, sizeof resp);
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->id, 0x1234);
  EXPECT_TRUE(h->response);
  EXPECT_EQ(h->rcode, 3);
  EXPECT_STREQ(dns::rcode_name(h->rcode), "NXDOMAIN");
  EXPECT_FALSE(dns::parse_header(resp, 11).has_value());
  EXPECT_FALSE(dns::parse_header(nullptr, 12).has_value());
}
