// RAII owner for a POSIX file descriptor. Move-only: exactly one owner closes it.
#pragma once
#include <unistd.h>
#include <utility>

namespace hm {

class UniqueFd {
 public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  UniqueFd& operator=(UniqueFd&& o) noexcept {
    if (this != &o) reset(std::exchange(o.fd_, -1));
    return *this;
  }
  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }
  int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) ::close(fd_);  // close errors are not actionable for us
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace hm
