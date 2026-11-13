#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "analysis/report.hpp"
#include "core/kinds.hpp"
#include "test_util.hpp"

using namespace hm;

namespace {
Diagnosis sample() {
  auto e = test::make_event("b1", "kmsg", 1, 1, kind::kernel_oom,
                            {{"comm", std::string("<script>alert(1)</script>")}, {"pid", std::int64_t{1}}},
                            Severity::error, "\x1b[31mevil");
  e.id = 9;
  std::vector<StoredEvent> evs{{e, 1}};
  Availability a;
  a.collectors["kmsg"] = {CollectorState::active, ""};
  Incident inc;
  inc.id = 1;
  inc.trigger = "oom";
  inc.subject = "<b>x</b>";
  return diagnose(inc, evs, {}, a, {1, "b1", 0}, {1, "b1", 1'000'000'000});
}
}  // namespace

TEST(Report, HtmlEscapesStoredStringsAndHasNoScript) {
  const auto html = render_html(sample(), true);
  EXPECT_EQ(html.find("<script>"), std::string::npos);
  EXPECT_NE(html.find("&lt;script&gt;"), std::string::npos);
  EXPECT_NE(html.find("Content-Security-Policy"), std::string::npos);
  EXPECT_NE(html.find("Synthetic data"), std::string::npos);
  EXPECT_EQ(html.find('\x1b'), std::string::npos);
}

TEST(Report, TextStripsControlCharacters) {
  const auto text = render_text(sample(), false);
  EXPECT_EQ(text.find('\x1b'), std::string::npos);
  EXPECT_NE(text.find("#9"), std::string::npos) << "facts cite event ids";
}

TEST(Report, JsonCarriesConclusionAndEvidenceIds) {
  auto j = diagnosis_json(sample());
  EXPECT_EQ(j["conclusion"]["verdict"], "supported");
  EXPECT_EQ(j["conclusion"]["leading"], "oom_kill");
  EXPECT_EQ(j["hypotheses"][0]["evidence"][0]["event_id"], 9);
}
