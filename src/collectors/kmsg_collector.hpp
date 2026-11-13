#pragma once
#include "collectors/collector.hpp"

namespace hm {

// Reads /dev/kmsg from the start of the current boot's ring buffer.
// Kernel records are emitted under source "kernel" with the kernel's own
// record sequence number as seq, so an agent restart that re-reads the ring
// produces exact duplicates that UNIQUE(boot_id, source, seq) absorbs
// (idempotent ingest). Collector health uses a separate source ("kmsg") so
// its counter can never collide with kernel sequence numbers.
class KmsgCollector final : public Collector {
 public:
  KmsgCollector(Emitter& status, Emitter& records, std::string path = "/dev/kmsg")
      : em_(status), rec_(records), path_(std::move(path)) {}
  std::string_view name() const override { return "kmsg"; }
  void run(std::stop_token stop) override;

 private:
  Emitter& em_;
  Emitter& rec_;
  std::string path_;
};

}  // namespace hm
