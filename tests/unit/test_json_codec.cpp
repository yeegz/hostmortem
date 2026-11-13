#include <gtest/gtest.h>

#include "core/json_codec.hpp"
#include "test_util.hpp"

using namespace hm;

TEST(JsonCodec, RoundTripsAnEvent) {
  auto e = test::make_event("boot", "prober", 42, 7, "probe.dns.direct",
                            {{"ok", false}, {"latency_ms", 2000.0}, {"server", std::string("10.0.0.1")}},
                            Severity::warning, "10.0.0.1");
  auto back = event_from_json_line(event_to_json_line(e));
  ASSERT_TRUE(back.ok()) << back.error().message;
  EXPECT_EQ(back->boot_id, "boot");
  EXPECT_EQ(back->seq, 42u);
  EXPECT_EQ(back->mono_ns, e.mono_ns);
  EXPECT_EQ(back->severity, Severity::warning);
  EXPECT_EQ(back->attr_bool("ok"), false);
  EXPECT_EQ(back->attr_str("server"), "10.0.0.1");
}

TEST(JsonCodec, RejectsMalformedInput) {
  const char* bad[] = {
      "",
      "not json",
      "[]",
      R"({"v":1})",
      R"({"v":99,"boot_id":"b","source":"s","seq":1,"mono_ns":1,"kind":"k","severity":"info"})",
      R"({"v":1,"boot_id":"b","source":"s","seq":1,"mono_ns":-5,"kind":"k","severity":"info"})",
      R"({"v":1,"boot_id":"b","source":"s","seq":1,"mono_ns":1,"kind":"k","severity":"loud"})",
      R"({"v":1,"boot_id":"b","source":"s","seq":"x","mono_ns":1,"kind":"k","severity":"info"})",
      R"({"v":1,"boot_id":"b","source":"s","seq":1,"mono_ns":1,"kind":"k","severity":"info","attrs":{"a":[1]}})",
      R"({"v":1,"boot_id":"b","source":"s","seq":1,"mono_ns":1,"kind":"k","severity":"info","attrs":{"a":{"b":1}}})",
  };
  for (const char* line : bad) EXPECT_FALSE(event_from_json_line(line).ok()) << line;
}

TEST(JsonCodec, RejectsOversizedLine) {
  std::string big = R"({"v":1,"boot_id":"b","source":"s","seq":1,"mono_ns":1,"kind":"k","severity":"info","subject":")";
  big += std::string(70 * 1024, 'a');
  big += "\"}";
  auto r = event_from_json_line(big);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, "too_large");
}
