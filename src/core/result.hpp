// Result<T>: a small std::expected stand-in. std::expected is C++23 and the
// supported targets include Ubuntu 22.04 (GCC 11), so we carry our own.
// Errors are values: no exception crosses a module or thread boundary.
#pragma once
#include <cassert>
#include <string>
#include <utility>
#include <variant>

namespace hm {

struct Error {
  std::string code;     // stable, machine-readable ("sqlite_full", "eacces")
  std::string message;  // human-readable detail
};

template <typename T>
class [[nodiscard]] Result {
 public:
  Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}  // NOLINT(implicit)
  Result(Error err) : v_(std::in_place_index<1>, std::move(err)) {}  // NOLINT(implicit)

  bool ok() const noexcept { return v_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  T& value() & { assert(ok()); return std::get<0>(v_); }
  const T& value() const& { assert(ok()); return std::get<0>(v_); }
  T&& value() && { assert(ok()); return std::get<0>(std::move(v_)); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T&& operator*() && { return std::move(*this).value(); }

  const Error& error() const { assert(!ok()); return std::get<1>(v_); }

 private:
  std::variant<T, Error> v_;
};

struct Unit {};
using Status = Result<Unit>;
inline Status ok_status() { return Unit{}; }

inline Error make_error(std::string code, std::string message) {
  return Error{std::move(code), std::move(message)};
}

}  // namespace hm
