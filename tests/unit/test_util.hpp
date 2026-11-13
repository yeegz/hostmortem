#pragma once
#include <unistd.h>

#include <filesystem>
#include <random>
#include <string>

#include "core/event.hpp"
#include "store/store.hpp"

namespace hm::test {

class TempDir {
 public:
  TempDir() {
    std::random_device rd;
    path_ = std::filesystem::temp_directory_path() / ("hm-test-" + std::to_string(::getpid()) + "-" + std::to_string(rd()));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  std::string file(const std::string& name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

inline Event make_event(const std::string& boot, const std::string& source, std::uint64_t seq, std::int64_t mono_s,
                        const std::string& kind, Attrs attrs = {}, Severity sev = Severity::info,
                        const std::string& subject = "") {
  Event e;
  e.boot_id = boot;
  e.source = source;
  e.seq = seq;
  e.mono_ns = mono_s * 1'000'000'000LL;
  e.wall_ns = 1'790'000'000'000'000'000LL + e.mono_ns;
  e.wall_quality = WallQuality::synced;
  e.kind = kind;
  e.severity = sev;
  e.subject = subject;
  e.attrs = std::move(attrs);
  return e;
}

}  // namespace hm::test
