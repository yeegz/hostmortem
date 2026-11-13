#include "collectors/prober.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include "collectors/dns_wire.hpp"
#include "collectors/procfs_parse.hpp"
#include "core/unique_fd.hpp"

namespace hm {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int remaining_ms(Clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  return left < 0 ? 0 : static_cast<int>(left);
}

std::optional<std::string> slurp(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Parses an IPv4/IPv6 literal into a sockaddr_storage with the given port.
std::optional<std::pair<sockaddr_storage, socklen_t>> make_addr(const std::string& host, std::uint16_t port) {
  sockaddr_storage ss{};
  auto* v4 = reinterpret_cast<sockaddr_in*>(&ss);
  if (inet_pton(AF_INET, host.c_str(), &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    return std::pair{ss, static_cast<socklen_t>(sizeof(sockaddr_in))};
  }
  auto* v6 = reinterpret_cast<sockaddr_in6*>(&ss);
  if (inet_pton(AF_INET6, host.c_str(), &v6->sin6_addr) == 1) {
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(port);
    return std::pair{ss, static_cast<socklen_t>(sizeof(sockaddr_in6))};
  }
  return std::nullopt;
}

bool is_loopback(const std::string& a) { return a.rfind("127.", 0) == 0 || a == "::1"; }

std::uint16_t icmp_checksum(const void* data, std::size_t len) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint32_t sum = 0;
  for (std::size_t i = 0; i + 1 < len; i += 2) sum += static_cast<std::uint32_t>((p[i] << 8) | p[i + 1]);
  if (len & 1) sum += static_cast<std::uint32_t>(p[len - 1] << 8);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return htons(static_cast<std::uint16_t>(~sum));
}

}  // namespace

Prober::Prober(Emitter& em, const Config& cfg, SipKey name_key)
    : em_(em), cfg_(cfg), key_(name_key), worker_([this](std::stop_token st) { resolver_worker(st); }) {}

Prober::~Prober() {
  worker_.request_stop();
  // A worker stuck inside getaddrinfo() cannot be interrupted; jthread's
  // destructor joins it. glibc's resolver timeout bounds that wait.
}

std::string Prober::display_name(const std::string& name) const {
  for (const auto& c : cfg_.clear_names) {
    if (c == name) return name;
  }
  return hash_name(key_, name);
}

void Prober::resolver_worker(std::stop_token stop) {
  std::unique_lock lock(mu_);
  while (!stop.stop_requested()) {
    if (!cv_.wait(lock, stop, [&] { return request_.has_value(); })) return;
    const std::string name = *request_;
    request_.reset();
    busy_ = true;
    lock.unlock();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const auto t0 = Clock::now();
    const int rc = ::getaddrinfo(name.c_str(), nullptr, &hints, &res);
    const double ms = ms_since(t0);
    if (res) ::freeaddrinfo(res);
    lock.lock();
    busy_ = false;
    answer_ = Answer{name, rc, ms};
    cv_.notify_all();
  }
}

void Prober::probe_resolver(const std::string& name) {
  std::unique_lock lock(mu_);
  const std::string shown = display_name(name);
  if (busy_) {
    em_.emit(kind::probe_dns_resolver, Severity::warning, "resolver",
             {{"ok", false}, {"name", shown}, {"err", std::string("resolver_blocked")},
              {"latency_ms", static_cast<double>(cfg_.probe_timeout.count())}});
    return;
  }
  answer_.reset();
  request_ = name;
  cv_.notify_all();
  const bool got = cv_.wait_for(lock, cfg_.probe_timeout, [&] { return answer_.has_value() && answer_->name == name; });
  if (!got) {
    em_.emit(kind::probe_dns_resolver, Severity::warning, "resolver",
             {{"ok", false}, {"name", shown}, {"err", std::string("timeout")},
              {"latency_ms", static_cast<double>(cfg_.probe_timeout.count())}});
    return;
  }
  const bool ok = answer_->rc == 0;
  Attrs a{{"ok", ok}, {"name", shown}, {"latency_ms", answer_->ms}};
  if (!ok) a["err"] = std::string(::gai_strerror(answer_->rc));
  em_.emit(kind::probe_dns_resolver, ok ? Severity::debug : Severity::warning, "resolver", std::move(a));
}

void Prober::probe_direct(const std::string& server, const std::string& name) {
  const std::string shown = display_name(name);
  auto fail = [&](const std::string& err, double ms) {
    em_.emit(kind::probe_dns_direct, Severity::warning, server,
             {{"ok", false}, {"server", server}, {"name", shown}, {"err", err}, {"latency_ms", ms}});
  };
  auto addr = make_addr(server, 53);
  if (!addr) return fail("bad_server_address", 0);
  UniqueFd fd(::socket(addr->first.ss_family, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!fd.valid()) return fail("socket:" + errno_name(errno), 0);
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr->first), addr->second) != 0) {
    return fail("connect:" + errno_name(errno), 0);
  }
  const std::uint16_t id = ++dns_id_;
  auto q = dns::build_query(id, name, 1);
  if (!q) return fail("bad_name", 0);
  const auto t0 = Clock::now();
  const auto deadline = t0 + cfg_.probe_timeout;
  if (::send(fd.get(), q->data(), q->size(), 0) < 0) return fail("send:" + errno_name(errno), ms_since(t0));
  std::array<std::uint8_t, 1500> buf;
  for (;;) {
    pollfd p{fd.get(), POLLIN, 0};
    const int pr = ::poll(&p, 1, remaining_ms(deadline));
    if (pr == 0) return fail("timeout", ms_since(t0));
    if (pr < 0) {
      if (errno == EINTR) continue;
      return fail("poll:" + errno_name(errno), ms_since(t0));
    }
    const ssize_t n = ::recv(fd.get(), buf.data(), buf.size(), 0);
    if (n < 0) {
      // ICMP port unreachable surfaces here as ECONNREFUSED on a connected UDP socket.
      return fail("recv:" + errno_name(errno), ms_since(t0));
    }
    auto h = dns::parse_header(buf.data(), static_cast<std::size_t>(n));
    if (!h || !h->response || h->id != id) continue;  // stray or spoofed: ignore and keep waiting
    const double ms = ms_since(t0);
    const bool ok = h->rcode == 0 && h->ancount > 0;
    Attrs a{{"ok", ok}, {"server", server}, {"name", shown}, {"rcode", std::string(dns::rcode_name(h->rcode))},
            {"answers", static_cast<std::int64_t>(h->ancount)}, {"latency_ms", ms}};
    if (!ok) a["err"] = std::string(dns::rcode_name(h->rcode));
    em_.emit(kind::probe_dns_direct, ok ? Severity::debug : Severity::warning, server, std::move(a));
    return;
  }
}

void Prober::probe_icmp(const std::string& target, const std::string& role) {
  auto fail = [&](const std::string& err, double ms) {
    em_.emit(kind::probe_icmp, Severity::warning, target,
             {{"ok", false}, {"target", target}, {"role", role}, {"err", err}, {"rtt_ms", ms}});
  };
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  if (inet_pton(AF_INET, target.c_str(), &dst.sin_addr) != 1) return fail("not_an_ipv4_literal", 0);

  UniqueFd fd;
  if (icmp_mode_ != 2) {
    fd.reset(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_ICMP));
    if (fd.valid()) icmp_mode_ = 1;
  }
  if (!fd.valid()) {
    fd.reset(::socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_ICMP));
    icmp_mode_ = fd.valid() ? 2 : 0;
  }
  if (!fd.valid()) return;  // reported once as prober.icmp unavailable

  icmphdr req{};
  req.type = ICMP_ECHO;
  req.un.echo.id = htons(static_cast<std::uint16_t>(::getpid() & 0xffff));
  req.un.echo.sequence = htons(++icmp_seq_);
  req.checksum = icmp_checksum(&req, sizeof req);  // the kernel recomputes it for ping sockets
  const std::uint16_t want_seq = req.un.echo.sequence;
  const auto t0 = Clock::now();
  const auto deadline = t0 + cfg_.probe_timeout;
  if (::sendto(fd.get(), &req, sizeof req, 0, reinterpret_cast<sockaddr*>(&dst), sizeof dst) < 0) {
    return fail("send:" + errno_name(errno), 0);
  }
  std::array<std::uint8_t, 1500> buf;
  for (;;) {
    pollfd p{fd.get(), POLLIN, 0};
    const int pr = ::poll(&p, 1, remaining_ms(deadline));
    if (pr == 0) return fail("timeout", ms_since(t0));
    if (pr < 0) {
      if (errno == EINTR) continue;
      return fail("poll:" + errno_name(errno), ms_since(t0));
    }
    sockaddr_in from{};
    socklen_t fl = sizeof from;
    const ssize_t n = ::recvfrom(fd.get(), buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 0) return fail("recv:" + errno_name(errno), ms_since(t0));
    std::size_t off = 0;
    if (icmp_mode_ == 2) {  // raw sockets deliver the IP header too
      if (n < 20) continue;
      off = static_cast<std::size_t>(buf[0] & 0x0f) * 4;
    }
    if (static_cast<std::size_t>(n) < off + sizeof(icmphdr)) continue;
    icmphdr rep;
    std::memcpy(&rep, buf.data() + off, sizeof rep);
    if (from.sin_addr.s_addr != dst.sin_addr.s_addr) continue;
    if (rep.type != ICMP_ECHOREPLY || rep.un.echo.sequence != want_seq) {
      if (rep.type == ICMP_DEST_UNREACH) return fail("unreachable", ms_since(t0));
      continue;
    }
    em_.emit(kind::probe_icmp, Severity::debug, target,
             {{"ok", true}, {"target", target}, {"role", role}, {"rtt_ms", ms_since(t0)}});
    return;
  }
}

void Prober::probe_tcp(const TcpTarget& t) {
  const std::string label = t.host + ":" + std::to_string(t.port);
  auto fail = [&](const std::string& err, double ms) {
    em_.emit(kind::probe_tcp, Severity::warning, label,
             {{"ok", false}, {"target", label}, {"err", err}, {"latency_ms", ms}});
  };
  auto addr = make_addr(t.host, t.port);
  if (!addr) return fail("not_an_ip_literal", 0);
  UniqueFd fd(::socket(addr->first.ss_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!fd.valid()) return fail("socket:" + errno_name(errno), 0);
  const auto t0 = Clock::now();
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr->first), addr->second) != 0 && errno != EINPROGRESS) {
    return fail(errno_name(errno), ms_since(t0));
  }
  pollfd p{fd.get(), POLLOUT, 0};
  const int pr = ::poll(&p, 1, static_cast<int>(cfg_.probe_timeout.count()));
  if (pr == 0) return fail("timeout", ms_since(t0));
  int err = 0;
  socklen_t len = sizeof err;
  ::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &err, &len);
  if (pr < 0 || err != 0) return fail(errno_name(err ? err : errno), ms_since(t0));
  em_.emit(kind::probe_tcp, Severity::debug, label, {{"ok", true}, {"target", label}, {"latency_ms", ms_since(t0)}});
}

std::vector<std::string> Prober::upstream_servers() {
  if (!cfg_.dns_servers.empty()) return cfg_.dns_servers;
  // systemd-resolved: /etc/resolv.conf names the 127.0.0.53 stub; the real
  // upstreams are listed here.
  std::vector<std::string> out;
  for (const char* path : {"/run/systemd/resolve/resolv.conf", cfg_.resolv_conf.c_str()}) {
    if (auto t = slurp(path)) {
      for (auto& s : procfs::parse_resolv_conf(*t)) {
        if (!is_loopback(s)) out.push_back(std::move(s));
      }
      if (!out.empty()) break;
    }
  }
  return out;
}

void Prober::run(std::stop_token stop) {
  do {
    const auto servers = upstream_servers();
    for (const auto& name : cfg_.dns_names) {
      if (stop.stop_requested()) return;
      probe_resolver(name);
      for (const auto& s : servers) probe_direct(s, name);
    }
    for (const auto& target : cfg_.icmp_targets) {
      if (stop.stop_requested()) return;
      if (target == "gateway") {
        auto route = slurp("/proc/net/route");
        auto gw = route ? procfs::parse_default_route(*route) : std::nullopt;
        if (!gw) {
          em_.emit(kind::probe_icmp, Severity::warning, "gateway",
                   {{"ok", false}, {"target", std::string("gateway")}, {"role", std::string("gateway")},
                    {"err", std::string("no_default_route")}, {"rtt_ms", 0.0}});
        } else {
          probe_icmp(gw->gateway, "gateway");
        }
      } else {
        probe_icmp(target, "target");
      }
    }
    for (const auto& t : cfg_.tcp_targets) probe_tcp(t);

    if (!reported_) {
      em_.status("prober", "active");
      if (cfg_.icmp_targets.empty()) {
        em_.status("prober.icmp", "unavailable", "no ICMP targets configured");
      } else {
        em_.status("prober.icmp", icmp_mode_ > 0 ? "active" : "unavailable",
                   icmp_mode_ > 0 ? "" : "no ping socket (net.ipv4.ping_group_range excludes this gid) and no CAP_NET_RAW");
      }
      em_.status("prober.dns_direct", servers.empty() ? "unavailable" : "active",
                 servers.empty() ? "no non-loopback upstream DNS server found; set probe.dns_servers" : "");
      reported_ = true;
    }
  } while (sleep_for(stop, cfg_.probe_interval));
}

}  // namespace hm
