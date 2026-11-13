#pragma once
#include <map>

#include "collectors/collector.hpp"
#include "collectors/netlink_parse.hpp"
#include "core/unique_fd.hpp"

namespace hm {

// Subscribes to rtnetlink link/address/route multicast groups, takes an
// initial dump so the timeline starts with known state, and emits changes.
// On ENOBUFS (the kernel dropped notifications because we fell behind) it
// records the loss and re-dumps, instead of silently diverging from reality.
class NetlinkCollector final : public Collector {
 public:
  explicit NetlinkCollector(Emitter& em) : em_(em) {}
  std::string_view name() const override { return "netlink"; }
  void run(std::stop_token stop) override;

 private:
  bool open_socket();
  bool request_dump(int type);
  bool drain_dump(std::stop_token& stop);
  void handle(const nl::Message& m, bool initial);

  struct LinkState {
    std::string ifname;
    std::string oper;
    bool up = false;
    bool carrier = false;
  };
  Emitter& em_;
  UniqueFd fd_;
  std::uint32_t seq_ = 1;
  std::map<int, LinkState> links_;
};

}  // namespace hm
