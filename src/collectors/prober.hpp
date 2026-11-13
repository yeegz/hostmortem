#pragma once
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "collectors/collector.hpp"
#include "core/config.hpp"
#include "core/siphash.hpp"

namespace hm {

// Active probes, run every probe_interval:
//   probe.dns.resolver  getaddrinfo() through the system resolver (nsswitch,
//                       stub, systemd-resolved: everything an app would use)
//   probe.dns.direct    a raw UDP query straight to each upstream server
//   probe.icmp          echo to the default gateway and configured targets
//   probe.tcp           connect() to configured host:port targets
// Comparing resolver vs direct is what separates a local resolver fault from
// an upstream one (blueprint "active-versus-passive comparison").
class Prober final : public Collector {
 public:
  Prober(Emitter& em, const Config& cfg, SipKey name_key);
  ~Prober() override;
  std::string_view name() const override { return "prober"; }
  void run(std::stop_token stop) override;

 private:
  void probe_resolver(const std::string& name);
  void probe_direct(const std::string& server, const std::string& name);
  void probe_icmp(const std::string& target, const std::string& role);
  void probe_tcp(const TcpTarget& t);
  std::vector<std::string> upstream_servers();
  std::string display_name(const std::string& name) const;
  void resolver_worker(std::stop_token stop);

  Emitter& em_;
  Config cfg_;
  SipKey key_;
  int icmp_mode_ = -1;  // -1 unknown, 0 none, 1 ping socket, 2 raw
  std::uint16_t dns_id_ = 0;
  std::uint16_t icmp_seq_ = 0;
  bool reported_ = false;

  // getaddrinfo() cannot be cancelled and may block for (timeout x attempts)
  // seconds. It runs on one long-lived worker thread; the prober waits at
  // most probe_timeout for it. A resolver still blocked from the previous
  // round is itself reported as a failure.
  std::mutex mu_;
  std::condition_variable_any cv_;
  std::optional<std::string> request_;
  struct Answer {
    std::string name;
    int rc = 0;
    double ms = 0;
  };
  std::optional<Answer> answer_;
  bool busy_ = false;
  std::jthread worker_;
};

}  // namespace hm
