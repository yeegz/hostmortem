#include "analysis/detector.hpp"

#include <algorithm>

#include "core/kinds.hpp"

namespace hm {

namespace {
TimePoint tp(const StoredEvent& e) { return {e.boot_ordinal, e.event.boot_id, e.event.mono_ns}; }
bool is_probe(const std::string& k) { return k.rfind("probe.", 0) == 0; }
}  // namespace

void Detector::adopt_open(const Incident& inc) {
  open_ = Open{inc.id, inc.opened};
}

std::optional<std::string> Detector::path_symptom(const Event& e) {
  auto& p = paths_[e.attr_str("target").value_or(e.subject)];
  const bool ok = e.attr_bool("ok").value_or(true);
  p.recent.push_back(ok);
  while (static_cast<int>(p.recent.size()) > cfg_.loss_window) p.recent.pop_front();
  if (!ok) {
    const auto failures = std::count(p.recent.begin(), p.recent.end(), false);
    const bool partial = failures < static_cast<long>(p.recent.size());
    if (partial && failures >= cfg_.loss_failures) return "path_loss";
    return std::nullopt;
  }
  const double rtt = e.attr_num("rtt_ms").value_or(0);
  const bool learned = p.base_samples >= cfg_.latency_min_baseline_samples;
  if (learned && rtt > std::max(3.0 * p.base_rtt, p.base_rtt + 50.0)) {
    if (++p.slow_streak >= cfg_.latency_streak) return "latency_excursion";
    return std::nullopt;
  }
  p.slow_streak = 0;
  p.base_rtt = p.base_rtt < 0 ? rtt : 0.9 * p.base_rtt + 0.1 * rtt;
  ++p.base_samples;
  return std::nullopt;
}

std::optional<std::string> Detector::symptom(const StoredEvent& se) {
  const Event& e = se.event;
  const std::string& k = e.kind;

  if (k == kind::probe_icmp) {
    if (auto why = path_symptom(e)) return why;
  }
  if (is_probe(k)) {
    const std::string key = k + "|" + e.subject;
    if (e.attr_bool("ok").value_or(true)) {
      fail_streak_[key] = 0;
      return std::nullopt;
    }
    int& streak = fail_streak_[key];
    if (streak == 0) {
      streak_start_[key] = {tp(se), e.id, e.wall_ns};
    }
    ++streak;
    if (streak >= cfg_.probe_fail_streak) return "probe_fail_streak";
    return std::nullopt;
  }
  if (k == kind::link_state) {
    const auto oper = e.attr_str("oper").value_or("");
    const bool carrier = e.attr_bool("carrier").value_or(true);
    if (e.attr_str("ifname").value_or("") == "lo") return std::nullopt;
    if (oper == "down" || oper == "lowerlayerdown" || !carrier) return "link_down";
    return std::nullopt;
  }
  if (k == kind::route_del && e.attr_bool("default").value_or(false)) return "default_route_lost";
  if (k == kind::kernel_oom || k == kind::cgroup_oom_kill) return "oom";
  if (k == kind::kernel_netdev_watchdog) return "nic_stall";
  if (k == kind::kernel_conntrack_full) return "conntrack_full";
  if (k == kind::clock_step) return "clock_step";
  if (k == kind::process_exit) return "process_exit";
  if (k == kind::process_restart) {
    auto& q = restarts_[e.attr_str("name").value_or(e.subject)];
    const TimePoint now = tp(se);
    q.push_back(now);
    while (!q.empty() && (q.front().boot_ordinal != now.boot_ordinal ||
                          now.mono_ns - q.front().mono_ns >
                              std::chrono::duration_cast<std::chrono::nanoseconds>(cfg_.restart_window).count())) {
      q.pop_front();
    }
    if (static_cast<int>(q.size()) >= cfg_.restart_burst) return "restart_loop";
    return "process_restart";
  }
  if (k == kind::net_tcp) {
    const double ppm = e.attr_num("retrans_ppm").value_or(0);
    const std::int64_t out = e.attr_int("out_segs").value_or(0);
    if (out < cfg_.retrans_min_segs) return std::nullopt;
    const double threshold = std::max(cfg_.retrans_floor_ppm,
                                      retrans_baseline_ppm_ < 0 ? 0.0 : retrans_baseline_ppm_ * cfg_.retrans_baseline_factor);
    if (ppm > threshold) return "retrans_excursion";
    // EWMA baseline from non-excursion samples only, so a storm does not
    // teach the detector that storms are normal.
    retrans_baseline_ppm_ = retrans_baseline_ppm_ < 0 ? ppm : 0.9 * retrans_baseline_ppm_ + 0.1 * ppm;
    return std::nullopt;
  }
  if (k == kind::sys_psi) {
    if (e.attr_num("cpu_some10").value_or(0) >= cfg_.psi_cpu_some10) return "cpu_pressure";
    if (e.attr_num("mem_full10").value_or(0) >= cfg_.psi_mem_full10) return "memory_pressure";
  }
  return std::nullopt;
}

std::vector<Detector::Action> Detector::observe(const StoredEvent& se) {
  std::vector<Action> actions;
  const TimePoint now = tp(se);
  const auto quiet_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(cfg_.quiet).count();

  if (open_) {
    if (now.boot_ordinal > open_->last_symptom.boot_ordinal) {
      // First event after a reboot: the incident survives the reboot and the
      // quiet timer restarts here, because mono time is not comparable across boots.
      open_->last_symptom = now;
    } else if (now.boot_ordinal == open_->last_symptom.boot_ordinal &&
               now.mono_ns - open_->last_symptom.mono_ns > quiet_ns) {
      Action a{Action::Type::close, {}};
      a.incident.id = open_->id;
      a.incident.closed = now;
      a.incident.status = IncidentStatus::closed;
      actions.push_back(std::move(a));
      open_.reset();
    }
  }

  const std::string probe_key = se.event.kind + "|" + se.event.subject;
  auto why = symptom(se);
  if (!why) return actions;

  if (open_) {
    open_->last_symptom = now;
    return actions;
  }
  Action a{Action::Type::open, {}};
  a.incident.trigger = *why;
  a.incident.subject = se.event.subject.empty() ? se.event.kind : se.event.subject;
  a.incident.opened = now;
  a.incident.opened_wall_ns = se.event.wall_ns;
  a.incident.trigger_event_id = se.event.id;
  if (*why == "probe_fail_streak") {
    // Open the incident at the first failure of the streak, not the Nth.
    const StreakStart& start = streak_start_[probe_key];
    a.incident.opened = start.at;
    a.incident.opened_wall_ns = start.wall_ns;
    a.incident.trigger_event_id = start.event_id;
  }
  open_ = Open{0, now};
  actions.push_back(std::move(a));
  return actions;
}

}  // namespace hm
