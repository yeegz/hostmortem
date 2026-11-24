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

TEST(Redact, ReplacesAddressesAndKeepsOtherNumbers) {
  EXPECT_EQ(redact_ips("gw 192.168.1.1 via eth0"), "gw x.x.x.x via eth0");
  EXPECT_EQ(redact_ips("server=10.0.0.53:53"), "server=x.x.x.x:53");
  EXPECT_EQ(redact_ips("addr fe80::1ff:fe23:4567:890a dev"), "addr x:x::x dev");
  EXPECT_EQ(redact_ips("mac 52:54:00:12:34:56"), "mac x:x::x");
  EXPECT_EQ(redact_ips("kernel 6.8.0-45-generic, 3 failures, 1.5 ms"), "kernel 6.8.0-45-generic, 3 failures, 1.5 ms");
  EXPECT_EQ(redact_ips("ratio 1:2"), "ratio 1:2");
}

namespace {
// Two incidents whose windows share event #9, as overlapping windows do.
HostReport two_incident_report() {
  HostReport r;
  r.generated_wall_ns = 1'790'000'000'000'000'000LL;
  r.event_count = 1;
  r.boots.push_back({1, "b1aaaaaaaaaa", 1'790'000'000'000'000'000LL, "6.8.0"});
  r.coverage.collectors["kmsg"] = {CollectorState::active, ""};
  r.coverage.collectors["prober.icmp"] = {CollectorState::unavailable, "no <route>"};
  auto a = sample();
  auto b = sample();
  b.incident.id = 2;
  r.incidents = {b, a};
  return r;
}
}  // namespace

TEST(HostReportHtml, EveryIncidentEveryLinkResolvesAndNoScript) {
  const auto html = render_html_report(two_incident_report());
  EXPECT_EQ(html.find("<script"), std::string::npos);
  EXPECT_NE(html.find("default-src 'none'"), std::string::npos) << "strict CSP: no network requests";
  EXPECT_NE(html.find("id=\"inc1\""), std::string::npos);
  EXPECT_NE(html.find("id=\"inc2\""), std::string::npos);
  EXPECT_NE(html.find("no &lt;route&gt;"), std::string::npos) << "coverage reasons are escaped";
  // Shared event #9 gets a distinct row id per incident, and every in-page
  // link has a target.
  EXPECT_NE(html.find("id=\"i1e9\""), std::string::npos);
  EXPECT_NE(html.find("id=\"i2e9\""), std::string::npos);
  std::size_t pos = 0, links = 0;
  while ((pos = html.find("href=\"#", pos)) != std::string::npos) {
    pos += 7;
    const auto end = html.find('"', pos);
    const auto target = html.substr(pos, end - pos);
    EXPECT_NE(html.find("id=\"" + target + "\""), std::string::npos) << "dangling link #" << target;
    ++links;
  }
  EXPECT_GE(links, 4u);
}

TEST(HostReportHtml, EmptyStoreSuggestsTheDrill) {
  HostReport r;
  const auto html = render_html_report(r);
  EXPECT_NE(html.find("hm drill"), std::string::npos);
  EXPECT_NE(html.find("0 incidents"), std::string::npos);
}

TEST(HostReportHtml, RedactedReportHoldsNoAddresses) {
  auto r = two_incident_report();
  for (auto& d : r.incidents) {
    d.incident.subject = "192.0.2.1";
    d.events[0].event.subject = "10.1.2.3";
    d.events[0].event.attrs["server"] = std::string("2001:db8::53");
    d = redact_diagnosis(std::move(d));
  }
  r.redacted = true;
  const auto html = render_html_report(r);
  EXPECT_EQ(html.find("192.0.2.1"), std::string::npos);
  EXPECT_EQ(html.find("10.1.2.3"), std::string::npos);
  EXPECT_EQ(html.find("2001:db8"), std::string::npos);
  EXPECT_NE(html.find("addresses are redacted"), std::string::npos);
}

TEST(Report, SingleIncidentLinksResolve) {
  const auto html = render_html(sample(), false);
  EXPECT_NE(html.find("href=\"#i1e9\""), std::string::npos);
  EXPECT_NE(html.find("id=\"i1e9\""), std::string::npos);
}
