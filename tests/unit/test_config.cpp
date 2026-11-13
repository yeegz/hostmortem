#include <gtest/gtest.h>

#include "core/config.hpp"

using namespace hm;
using namespace std::chrono_literals;

TEST(Config, ParsesAllSections) {
  auto c = parse_config(R"(
# comment
[store]
path = /tmp/x.db
commit_interval = 2s
max_mb = 64
[probe]
interval = 15s
timeout = 1500ms
dns_names = example.com, example.org
dns_servers = 10.0.0.53
tcp_targets = 10.0.0.1:443, [::1]:80
icmp_targets = gateway, 1.1.1.1
[watch]
processes = nginx, myapp
[privacy]
clear_names = example.com
)");
  ASSERT_TRUE(c.ok()) << c.error().message;
  EXPECT_EQ(c->db_path, "/tmp/x.db");
  EXPECT_EQ(c->commit_interval, 2000ms);
  EXPECT_EQ(c->probe_timeout, 1500ms);
  EXPECT_EQ(c->dns_names.size(), 2u);
  ASSERT_EQ(c->tcp_targets.size(), 2u);
  EXPECT_EQ(c->tcp_targets[0].port, 443);
  EXPECT_EQ(c->watch_processes[1], "myapp");
  EXPECT_EQ(c->max_store_mb, 64);
}

TEST(Config, RejectsBadInputWithLineNumbers) {
  auto c = parse_config("[probe]\ninterval = soon\n");
  ASSERT_FALSE(c.ok());
  EXPECT_NE(c.error().message.find("line 2"), std::string::npos);
  EXPECT_FALSE(parse_config("[probe\n").ok());
  EXPECT_FALSE(parse_config("[nope]\nx = 1\n").ok());
  EXPECT_FALSE(parse_config("[probe]\ntcp_targets = host\n").ok());
  EXPECT_FALSE(parse_config("[probe]\ntcp_targets = host:99999\n").ok());
  EXPECT_FALSE(parse_config("[probe]\ninterval = 1s\ntimeout = 2s\n").ok()) << "timeout must be < interval";
  EXPECT_FALSE(parse_config("[store]\nmax_mb = 1\n").ok());
}

TEST(Config, Durations) {
  EXPECT_EQ(*parse_duration("250ms"), 250ms);
  EXPECT_EQ(*parse_duration("3"), 3000ms);
  EXPECT_EQ(*parse_duration("2m"), 120000ms);
  EXPECT_FALSE(parse_duration("-1s").ok());
  EXPECT_FALSE(parse_duration("1y").ok());
}
